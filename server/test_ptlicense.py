"""Chay: python3 test_ptlicense.py   (khong can pytest)"""
import secrets, tempfile, os
import ptlicense as L

DAY = L.DAY
T0 = 1_800_000_000
HW1, HW2, HW3 = (secrets.token_hex(32) for _ in range(3))


def fresh():
    d = tempfile.mkdtemp()
    return L.connect(os.path.join(d, "t.db")), secrets.token_bytes(32), L.Signer(L.Ed25519PrivateKey.generate())


def raises(code, fn, *a, **k):
    try:
        fn(*a, **k)
    except L.LicenseError as e:
        assert e.code == code, f"mong doi {code}, nhan {e.code}"
        return
    raise AssertionError(f"mong doi loi {code} nhung khong co loi")


def test_key_format_and_normalize():
    k = L.gen_key()
    assert k.startswith("PTL-") and len(k) == 27 and k.count("-") == 4
    body = L.normalize_key(k)
    assert L.normalize_key(k.lower()) == body                        # chu thuong
    assert L.normalize_key(k.replace("-", " ")) == body              # dau cach thay gach
    assert L.normalize_key(body) == body                             # khong co PTL-
    raises("invalid_key", L.normalize_key, "abc")
    raises("invalid_key", L.normalize_key, "PTL-" + "U" * 20)        # U khong thuoc bang chu cai


def test_plans_durations():
    for plan, days in {"trial": 3, "m1": 30, "m6": 180, "y1": 365}.items():
        db, sec, sg = fresh()
        k = L.create_licenses(db, sec, plan, now=T0)[0]
        r = L.activate(db, sec, sg, k, HW1, now=T0 + 5 * DAY)         # mua truoc, kich hoat sau 5 ngay
        assert r["expires_at"] == T0 + 5 * DAY + days * DAY, plan     # dem tu luc kich hoat
        p = L.verify_token(sg.pub, r["token"])
        assert p["hwid"] == HW1 and p["plan"] == plan and p["expires_at"] == r["expires_at"]
        now = T0 + 5 * DAY
        assert p["offline_until"] == min(r["expires_at"], now + L.OFFLINE_DAYS * DAY)


def test_custom_days_and_until():
    db, sec, sg = fresh()
    k = L.create_licenses(db, sec, "custom", days=45, now=T0)[0]
    assert L.activate(db, sec, sg, k, HW1, now=T0)["expires_at"] == T0 + 45 * DAY
    until = T0 + 100 * DAY
    k2 = L.create_licenses(db, sec, "custom", until=until, now=T0)[0]
    assert L.activate(db, sec, sg, k2, HW1, now=T0 + 10 * DAY)["expires_at"] == until   # ngay co dinh
    try:
        L.create_licenses(db, sec, "m1", days=5); raise AssertionError
    except ValueError:
        pass
    try:
        L.create_licenses(db, sec, "custom"); raise AssertionError
    except ValueError:
        pass


def test_idempotent_and_device_limit():
    db, sec, sg = fresh()
    k = L.create_licenses(db, sec, "m1", now=T0)[0]
    a = L.activate(db, sec, sg, k, HW1, now=T0)
    b = L.activate(db, sec, sg, k.lower(), HW1, now=T0 + 3600)       # cai lai may cu: OK, het han khong doi
    assert a["expires_at"] == b["expires_at"]
    raises("device_limit", L.activate, db, sec, sg, k, HW2, now=T0 + 7200)
    raises("invalid_key", L.activate, db, sec, sg, L.gen_key(), HW1, now=T0)
    raises("bad_request", L.activate, db, sec, sg, k, "xyz", now=T0)
    k2 = L.create_licenses(db, sec, "m1", devices=2, now=T0)[0]      # key 2 may
    L.activate(db, sec, sg, k2, HW1, now=T0); L.activate(db, sec, sg, k2, HW2, now=T0)
    raises("device_limit", L.activate, db, sec, sg, k2, HW3, now=T0)


def test_expiry_and_refresh():
    db, sec, sg = fresh()
    k = L.create_licenses(db, sec, "trial", now=T0)[0]
    tok = L.activate(db, sec, sg, k, HW1, now=T0)["token"]
    r = L.refresh(db, sg, tok, now=T0 + 2 * DAY)
    assert r["expires_at"] == T0 + 3 * DAY and r["offline_until"] == T0 + 3 * DAY   # bi chan boi het han
    raises("expired", L.refresh, db, sg, tok, now=T0 + 3 * DAY)
    raises("expired", L.activate, db, sec, sg, k, HW1, now=T0 + 3 * DAY + 1)
    raises("bad_token", L.refresh, db, sg, tok[:-3] + "AAA", now=T0)
    other = L.Signer(L.Ed25519PrivateKey.generate())
    raises("bad_token", L.refresh, db, other, tok, now=T0)                           # chu ky sai khoa


def test_token_tamper():
    db, sec, sg = fresh()
    k = L.create_licenses(db, sec, "m1", now=T0)[0]
    tok = L.activate(db, sec, sg, k, HW1, now=T0)["token"]
    data, sig = tok.split(".")
    forged = L._b64u(L._unb64u(data).replace(b'"plan":"m1"', b'"plan":"y1"')) + "." + sig
    assert L.verify_token(sg.pub, forged) is None


def test_revoke_extend_reset():
    db, sec, sg = fresh()
    k = L.create_licenses(db, sec, "m1", now=T0)[0]
    tok = L.activate(db, sec, sg, k, HW1, now=T0)["token"]
    L.extend(db, 1, 10)
    assert L.refresh(db, sg, tok, now=T0 + DAY)["expires_at"] == T0 + 40 * DAY   # extend khi con han: cong don
    # gia han key da het han: tinh tu luc gia han
    L.extend(db, 1, 30, now=T0 + 100 * DAY)
    assert db.execute("SELECT expires_at FROM licenses WHERE id=1").fetchone()[0] == T0 + 130 * DAY
    L.revoke(db, 1)
    raises("revoked", L.refresh, db, sg, tok, now=T0 + DAY)
    raises("revoked", L.activate, db, sec, sg, k, HW1, now=T0 + DAY)
    # reset may
    k2 = L.create_licenses(db, sec, "m1", now=T0)[0]
    L.activate(db, sec, sg, k2, HW1, now=T0)
    L.reset_devices(db, 2)
    L.activate(db, sec, sg, k2, HW2, now=T0)                                         # may moi vao duoc


def test_self_deactivate_limit():
    db, sec, sg = fresh()
    k = L.create_licenses(db, sec, "m1", now=T0)[0]
    hw = [secrets.token_hex(32) for _ in range(5)]
    tok = L.activate(db, sec, sg, k, hw[0], now=T0)["token"]
    for i in range(L.DEFAULT_RESETS):                                                # go + chuyen sang may moi
        assert L.deactivate(db, sg, tok, now=T0)["ok"]
        tok = L.activate(db, sec, sg, k, hw[i + 1], now=T0)["token"]
    raises("no_resets_left", L.deactivate, db, sg, tok, now=T0)


def test_trial_once_per_machine():
    db, sec, sg = fresh()
    k1, k2 = L.create_licenses(db, sec, "trial", count=2, now=T0)
    L.activate(db, sec, sg, k1, HW1, now=T0)
    raises("trial_used", L.activate, db, sec, sg, k2, HW1, now=T0 + 10 * DAY)        # may cu xin trial moi
    L.activate(db, sec, sg, k2, HW2, now=T0 + 10 * DAY)                              # may khac thi duoc


def test_activation_window():
    db, sec, sg = fresh()
    k = L.create_licenses(db, sec, "m1", activate_within_days=30, now=T0)[0]
    raises("activation_window_closed", L.activate, db, sec, sg, k, HW1, now=T0 + 31 * DAY)
    L.activate(db, sec, sg, k, HW1, now=T0 + 29 * DAY)


def test_key_not_stored_plain():
    db, sec, sg = fresh()
    k = L.create_licenses(db, sec, "m1", now=T0)[0]
    dump = " ".join(str(tuple(r)) for r in db.execute("SELECT * FROM licenses"))
    assert L.normalize_key(k) not in dump and k not in dump
    assert db.execute("SELECT key_hint FROM licenses").fetchone()[0] == k[-4:]


def test_concurrent_activation_race():
    import threading
    d = tempfile.mkdtemp(); path = os.path.join(d, "r.db")
    sec = secrets.token_bytes(32); sg = L.Signer(L.Ed25519PrivateKey.generate())
    db0 = L.connect(path)
    k = L.create_licenses(db0, sec, "m1", now=T0)[0]
    results = []
    def worker(hw):
        db = L.connect(path)
        try:
            L.activate(db, sec, sg, k, hw, now=T0); results.append("ok")
        except L.LicenseError as e:
            results.append(e.code)
    ths = [threading.Thread(target=worker, args=(secrets.token_hex(32),)) for _ in range(8)]
    [t.start() for t in ths]; [t.join() for t in ths]
    assert results.count("ok") == 1 and results.count("device_limit") == 7, results


def test_public_trial_code():
    db, sec, sg = fresh()
    L.add_trial_code(db, sec, "033.247.3333", 3, "Zalo shop", now=T0)
    for i, form in enumerate(["033.247.3333", "0332473333", "033 247 3333", "033-247-3333", "+84 33 247 3333", "84332473333"]):
        hw = secrets.token_hex(32)
        r = L.activate(db, sec, sg, form, hw, now=T0)                 # moi cach go deu ra cung mot ma
        assert r["plan"] == "trial" and r["expires_at"] == T0 + 3 * DAY, form
        p = L.verify_token(sg.pub, r["token"])
        assert p["hwid"] == hw and p["plan"] == "trial"

    # cung may nhap lai: cap lai token, KHONG gia han
    r1 = L.activate(db, sec, sg, "0332473333", HW1, now=T0)
    r2 = L.activate(db, sec, sg, "033.247.3333", HW1, now=T0 + DAY)
    assert r1["expires_at"] == r2["expires_at"] == T0 + 3 * DAY
    assert L.refresh(db, sg, r2["token"], now=T0 + 2 * DAY)["expires_at"] == T0 + 3 * DAY

    # het han roi nhap lai: khong duoc dung thu lan nua
    raises("trial_used", L.activate, db, sec, sg, "033.247.3333", HW1, now=T0 + 4 * DAY)
    # nhung may do van mua key tra phi duoc binh thuong
    k = L.create_licenses(db, sec, "m1", now=T0)[0]
    assert L.activate(db, sec, sg, k, HW1, now=T0 + 4 * DAY)["plan"] == "m1"

    # so khac / sai -> invalid_key ; key that van chay nhu cu
    raises("invalid_key", L.activate, db, sec, sg, "033.247.0000", HW2, now=T0)
    raises("invalid_key", L.activate, db, sec, sg, "123456", HW2, now=T0)
    raises("invalid_key", L.activate, db, sec, sg, "PTL-0332-473333", HW2, now=T0)      # co chu -> khong coi la ma so

    # may da dung thu bang ma thi khong xin duoc them key trial
    k2 = L.create_licenses(db, sec, "trial", now=T0)[0]
    raises("trial_used", L.activate, db, sec, sg, k2, HW1, now=T0 + 5 * DAY)

    # thu hoi / tat ma
    db2, sec2, sg2 = fresh()
    L.add_trial_code(db2, sec2, "0332473333", 3, now=T0)
    L.activate(db2, sec2, sg2, "0332473333", HW1, now=T0)
    L.revoke(db2, 1)
    raises("revoked", L.activate, db2, sec2, sg2, "0332473333", HW1, now=T0 + 1)
    L.set_trial_code_enabled(db2, sec2, "033.247.3333", False)
    raises("trial_closed", L.activate, db2, sec2, sg2, "0332473333", HW2, now=T0 + 1)    # may moi bi tu choi
    L.set_trial_code_enabled(db2, sec2, "033.247.3333", True)
    assert L.activate(db2, sec2, sg2, "0332473333", HW2, now=T0 + 1)["plan"] == "trial"


def test_trial_code_ip_limit():
    db, sec, sg = fresh()
    L.add_trial_code(db, sec, "0332473333", now=T0)
    for _ in range(L.MAX_TRIALS_PER_IP):
        L.activate(db, sec, sg, "0332473333", secrets.token_hex(32), now=T0, client_ip="1.2.3.4")
    raises("trial_limit", L.activate, db, sec, sg, "0332473333", secrets.token_hex(32), now=T0, client_ip="1.2.3.4")
    L.activate(db, sec, sg, "0332473333", secrets.token_hex(32), now=T0, client_ip="5.6.7.8")          # IP khac: duoc
    L.activate(db, sec, sg, "0332473333", secrets.token_hex(32), now=T0 + 31 * DAY, client_ip="1.2.3.4")  # het cua so 30 ngay
    # IP that khong duoc luu
    dump = " ".join(str(tuple(r)) for r in db.execute("SELECT * FROM trial_ips"))
    assert "1.2.3.4" not in dump
    # may da co thu roi nhap lai KHONG tinh vao gioi han IP
    hw = secrets.token_hex(32)
    db3, sec3, sg3 = fresh()
    L.add_trial_code(db3, sec3, "0332473333", now=T0)
    for _ in range(10):
        L.activate(db3, sec3, sg3, "0332473333", hw, now=T0, client_ip="9.9.9.9")
    assert db3.execute("SELECT COUNT(*) FROM trial_ips").fetchone()[0] == 1


def test_trial_code_stored_hashed():
    db, sec, sg = fresh()
    L.add_trial_code(db, sec, "033.247.3333", now=T0)
    dump = " ".join(str(tuple(r)) for r in db.execute("SELECT * FROM trial_codes"))
    assert "0332473333" not in dump and "033.247.3333" not in dump


def test_client_rules():
    db, sec, sg = fresh()
    k = L.create_licenses(db, sec, "m1", now=T0)[0]
    p = L.verify_token(sg.pub, L.activate(db, sec, sg, k, HW1, now=T0)["token"])
    ok = lambda now, last=T0, hw=HW1: L.local_state(p, hw, now, last)
    assert ok(T0 + DAY, T0 + DAY) == "ok"
    assert ok(T0 + 6 * DAY, T0 + 6 * DAY) == "ok"                                     # offline 6 ngay: van chay
    assert ok(T0 + 8 * DAY, T0 + 8 * DAY) == "need_online"                            # qua 7 ngay: can noi mang
    assert ok(T0 + DAY, T0 + 5 * DAY) == "need_online"                                # dong ho bi lui 4 ngay
    assert ok(T0 + 5 * DAY - 100, T0 + 5 * DAY) == "ok"                               # lech nho (<5 phut) bo qua
    assert ok(T0 + 31 * DAY, T0 + 31 * DAY) == "expired"
    assert ok(T0 + DAY, T0 + DAY, hw=HW2) == "invalid"                                # chep license sang may khac


if __name__ == "__main__":
    n = 0
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn(); n += 1; print("OK  ", name)
    print(f"\n{n} test dat")
