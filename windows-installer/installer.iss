; =====================================================================
; Inno Setup Script for Mercury HF Modem (Windows Installer)
; =====================================================================

#define MyAppName "Mercury HF Modem"
#define MyAppVersion "1.9.10"
#define MyAppPublisher "© 2026 Rhizomatica Communications"
#define MyAppURL "https://github.com/Rhizomatica/mercury"
#define MyAppExeName "mercury-ui.exe"

; --- Optional Authenticode signing --------------------------------------
; The SimplySign cert lives in Certum's cloud HSM — no .pfx file exists.
; The SimplySign Desktop app injects the cert into the Windows cert store.
; Once SimplySign Desktop is running and authenticated, sign with:
;
;   ISCC /DSIGN /Smercury="signtool sign /a /fd sha256 \
;         /tr http://time.certum.pl /td sha256 $f" installer.iss
;
; The /a flag auto-selects the cert from the Windows cert store.
; Without /DSIGN the installer builds unsigned exactly as before.
;
; For a local .pfx (self-signed test or OV/EV token), use:
;   ISCC /DSIGN /Smercury="signtool sign /f cert.pfx /p PW /fd sha256 \
;         /tr http://timestamp.digicert.com /td sha256 $f" installer.iss
;
; See docs/WINDOWS-SIGNING.md for the full signing workflow.
#ifdef SIGN
  #define ExeFlags "ignoreversion signonce"
#else
  #define ExeFlags "ignoreversion"
#endif

[Setup]
; NOTE: The value of AppId uniquely identifies this application. Do not use the same AppId value in installers for other applications.
AppId={{D3B073A1-C8A5-42E1-BC9B-5A3445C555DF}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={commonpf}\MercuryHF
DisableDirPage=no
DefaultGroupName=Mercury HF Modem
DisableProgramGroupPage=no
LicenseFile=../LICENSE
OutputBaseFilename=Mercury_{#MyAppVersion}_Setup
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ChangesEnvironment=yes
ArchitecturesInstallIn64BitMode=x64
#ifdef SIGN
SignTool=mercury
SignedUninstaller=yes
#endif

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"


[Dirs]
Name: "{app}"; Permissions: users-modify

[Files]
Source: "run_mercury.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "mercury.ini"; DestDir: "{app}"; Flags: ignoreversion; Permissions: users-modify
Source: "../LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "../assets/mercury.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "libgcc_s_seh-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libhamlib-4.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libusb-1.0.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libwinpthread-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "mercury-ui.exe"; DestDir: "{app}"; Flags: {#ExeFlags}

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\mercury.ico"; Comment: "Launch Mercury HF Modem (GUI)"
Name: "{group}\Configure Hardware (mercury.ini)"; Filename: "notepad.exe"; Parameters: "{app}\mercury.ini"; Comment: "Edit Mercury Audio & PTT Configuration"
;Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstexe}"; Comment: "Uninstall Mercury HF Modem"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\mercury.ico"; Tasks: desktopicon; Comment: "Launch Mercury HF Modem"


[Run]
; Launch Mercury HF Modem after installation completes
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: shellexec postinstall nowait skipifsilent

[UninstallRun]
; Clean up compiled objects and downloaded files on uninstall
Filename: "powershell.exe"; Parameters: "-Command ""Remove-Item -Path '{app}\mercury_temp', '{app}\.git', '{app}\build', '{app}\*.o' -Recurse -ErrorAction SilentlyContinue"""; Flags: runhidden

[Code]
// Inno Setup Custom Pascal Scripting for Installer Validation
function InitializeSetup(): Boolean;
begin
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    // Installation completed successfully
  end;
end;
