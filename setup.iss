; ============================================================================
;  pt.Live - Inno Setup script (Inno Setup 6.4 tro len)
;  Dat file nay tai:  A:\all2\ptLive\setup.iss     (LUU DANG UTF-8 CO BOM - da co san)
;  Bien dich:         ISCC.exe setup.iss    (hoac mo bang Inno Setup Compiler, bam F9)
;  Ket qua:           A:\all2\ptLive\installer\pt.Live-0.1.0.exe
;
;  Can: Vietnamese.isl (ban dich khong chinh thuc cua Inno Setup) dat CANH file nay,
;       hoac trong thu muc Languages cua Inno Setup.
;       Tai tai: https://jrsoftware.org/files/istrans/   (muc Vietnamese)
; ============================================================================

#define MyAppName      "pt.Live"
; PHAI BANG "VERSION" trong CMakeLists.txt va "version" trong update.json tren VPS (xem docs/RELEASE_v0.1.0.md).
#define MyAppVersion   "0.1.0"
#define MyAppExe       "ptLive.exe"
#define MyTrialCode    "033.247.3333"
#define MyReleaseDir   "build\ptLive_artefacts\Release"

#if FileExists(SourcePath + "Vietnamese.isl")
  #define ViIsl SourcePath + "Vietnamese.isl"
#else
  #define ViIsl "compiler:Languages\Vietnamese.isl"
#endif

[Setup]
; GUID co dinh: KHONG DOI giua cac ban phat hanh (de ban moi cap nhat de len ban cu).
AppId={{F82EE1F7-5C1A-48AD-8227-31FD20F2E8CB}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} - {#MyTrialCode}
AppPublisher={#MyAppName}
VersionInfoVersion={#MyAppVersion}

; App chay quyen user (khong can admin), cai vao LocalAppData\Programs de khong bi VST crash.
; Mac dinh: C:\Users\<user>\AppData\Local\Programs\pt.Live
; PrivilegesRequired=lowest: KHONG hoi UAC (mac dinh cua Inno la admin). Bat buoc cho tu cap nhat trong app:
; bo cai chay am (/SILENT) tu app, khong hien hop thoai UAC giua chung.
PrivilegesRequired=lowest
DefaultDirName={localappdata}\Programs\{#MyAppName}
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
DisableDirPage=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; Logo / bieu tuong
SetupIconFile={#MyReleaseDir}\ptlive.ico
UninstallDisplayIcon={app}\ptlive.ico
UninstallDisplayName={#MyAppName}

; Cap nhat khi app dang chay: Windows Restart Manager tu dong dong ptLive.exe roi cai de.
CloseApplications=yes
RestartApplications=no
SetupMutex=ptLiveSetupMutex

OutputDir=installer
OutputBaseFilename=pt.Live-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "vietnamese"; MessagesFile: "{#ViIsl}"

[CustomMessages]
vietnamese.CreateDesktopIconVi=Tạo biểu tượng pt.Live trên màn hình &Desktop
vietnamese.AdditionalIconsVi=Biểu tượng bổ sung:
vietnamese.LaunchProgramVi=Chạy pt.Live ngay sau khi cài đặt

[Tasks]
; Khong co co "unchecked" => lan cai dau DUOC TICK SAN. "checkedonce" => cac lan cap nhat sau nho lua chon cua nguoi dung
; (xoa icon Desktop roi thi cap nhat im lang khong tu tao lai).
Name: "desktopicon"; Description: "{cm:CreateDesktopIconVi}"; GroupDescription: "{cm:AdditionalIconsVi}"; Flags: checkedonce

[Files]
Source: "{#MyReleaseDir}\ptLive.exe";      DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyReleaseDir}\vps_config.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyReleaseDir}\vps_cert.pem";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#MyReleaseDir}\ptlive.ico";      DestDir: "{app}"; Flags: ignoreversion

[Icons]
; QUAN TRONG (pin taskbar): moi shortcut deu tro CUNG MOT file {app}\ptLive.exe, khong tham so,
; va KHONG dat AppUserModelID. App cung khong tu dat AppUserModelID, nen Windows nhom cua so
; theo duong dan exe => bam icon da ghim thi mo dung icon do, khong sinh them icon thu 2.
; Khong duoc them AppUserModelID vao day neu code app chua dat cung gia tri do.
; User-level shortcuts (khong can admin): Start Menu + Desktop cua nguoi dung hien tai.
Name: "{userprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; IconFilename: "{app}\ptlive.ico"
Name: "{userdesktop}\{#MyAppName}";  Filename: "{app}\{#MyAppExe}"; IconFilename: "{app}\ptlive.ico"; Tasks: desktopicon

[Run]
; App chay quyen user (khong can admin).
Filename: "{app}\{#MyAppExe}"; Description: "{cm:LaunchProgramVi}"; Flags: nowait postinstall skipifsilent shellexec

; GHI CHU
; 1) Du lieu nguoi dung (license.dat, log, crash dump, cua so) nam o %APPDATA%\ptLive (Roaming).
;    Go cai dat KHONG xoa thu muc do, nen cai lai khong mat ban quyen.
; 2) Installer chay quyen user nen {userdesktop}/{userprograms} la Desktop/Start Menu cua nguoi dung.
; 3) Nguoi dung da ghim ban cu: cai ban moi vao CUNG thu muc de pin van dung. Doi thu muc cai
;    thi pin cu tro vao exe cu va se xuat hien 2 icon.

; ============================================================================
; [Code] - kiem tra CPU co AVX2 (P0-6)
; pt.Live build voi /arch:AVX2: CPU khong co AVX2 thi chet ngay khi mo (illegal instruction), truoc ca
; crash handler. Installer chan som va noi ro ly do.
; Windows chi bao dung co AVX2 (PF_AVX2_INSTRUCTIONS_AVAILABLE = 40) tu Windows 10 1709 (build 16299).
; Ban Windows cu hon thi KHONG kiem tra duoc: cho cai, khong chan nham may co AVX2.
; ============================================================================
[Code]
function IsProcessorFeaturePresent(Feature: Cardinal): Boolean;
  external 'IsProcessorFeaturePresent@kernel32.dll stdcall';

function CanCheckAvx2(): Boolean;
var
  V: TWindowsVersion;
begin
  GetWindowsVersionEx(V);
  Result := (V.Major > 10) or ((V.Major = 10) and (V.Build >= 16299));
end;

function InitializeSetup(): Boolean;
begin
  Result := True;

  if CanCheckAvx2() and (not IsProcessorFeaturePresent(40)) then
  begin
    MsgBox('Máy này không hỗ trợ AVX2 nên không chạy được pt.Live.' + #13#10 +
           'Yêu cầu: CPU Intel Haswell (thế hệ 4) trở lên hoặc AMD Ryzen, Windows 10 trở lên.',
           mbCriticalError, MB_OK);
    Result := False;
  end;
end;
