; =====================================================================
; Inno Setup Script for Mercury HF Modem (Windows Installer)
; =====================================================================

#define MyAppName "Mercury HF Modem"
#define MyAppVersion "1.9.10"
#define MyAppPublisher "© 2026 Rhizomatica Communications"
#define MyAppURL "https://github.com/Rhizomatica/mercury"
#define MyAppExeName "mercury-ui.exe"

; --- Authenticode signing ----------------------------------------------
; The EXE files (payload + installer) are signed with osslsigncode on Linux,
; not with Inno's SignTool. The SimplySign cert has no .pfx — signing uses
; the cloud HSM via PKCS#11. See docs/WINDOWS-SIGNING.md.
;
; Build unsigned, then sign afterward:
;   1. ISCC installer.iss                         (builds Mercury_*.exe)
;   2. make sign-windows-bin BIN=Mercury_$(VERSION)_Setup.exe  (signs it on Linux)
;
; If you prefer Inno-side signing on Windows (requires SimplySign Desktop):
;   ISCC /DSIGN /Smercury="signtool sign /a /fd sha256 \
;         /tr http://time.certum.pl /td sha256 $f" installer.iss
;
; Old local-.pfx path (self-signed test / OV/EV token):
;   ISCC /DSIGN /Smercury="signtool sign /f cert.pfx /p PW /fd sha256 \
;         /tr http://timestamp.digicert.com /td sha256 $f" installer.iss
#undef SIGN
#define ExeFlags "ignoreversion"

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
; Emit the installer in the project root, next to the release ZIP, rather than
; in a windows-installer/Output/ subdirectory nobody thinks to look in.
; Relative to this script's directory.  Root *.exe is already gitignored.
OutputDir=..
OutputBaseFilename=Mercury_{#MyAppVersion}_Setup
Compression=lzma
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ChangesEnvironment=yes
ArchitecturesInstallIn64BitMode=x64

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"


[Dirs]
Name: "{app}"; Permissions: users-modify

[Files]
Source: "mercury.ini"; DestDir: "{app}"; Flags: ignoreversion; Permissions: users-modify
Source: "../LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "../assets/mercury.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "libgcc_s_seh-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libhamlib-4.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libusb-1.0.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libwinpthread-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "mercury-ui.exe"; DestDir: "{app}"; Flags: {#ExeFlags}
; Console/TNC build of the same modem.  Always installed — it costs ~2 MB and
; it is what makes remote support possible ("open the console shortcut and send
; me what it prints").  Deliberately NOT a component checkbox: an extra wizard
; choice is exactly the kind of decision a first-time user cannot make, and
; getting it wrong leaves them without the tool when they need help.
Source: "mercury.exe"; DestDir: "{app}"; Flags: {#ExeFlags}

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\mercury.ico"; Comment: "Launch Mercury HF Modem (GUI)"
Name: "{group}\Configure Hardware (mercury.ini)"; Filename: "notepad.exe"; Parameters: "{app}\mercury.ini"; Comment: "Edit Mercury Audio & PTT Configuration"
;Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstexe}"; Comment: "Uninstall Mercury HF Modem"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\mercury.ico"; Tasks: desktopicon; Comment: "Launch Mercury HF Modem"
; --- Advanced ---------------------------------------------------------------
; The console modem lives in its own Start Menu subfolder and never on the
; desktop, so the GUI stays the single obvious way in.  The shortcut opens a
; command prompt in the install folder showing the usage text, rather than
; running the modem: a bare double-click would flash a window and vanish, which
; looks broken.  This lands an advanced user (or someone being talked through a
; problem) at a prompt with mercury.exe on hand.
Name: "{group}\Advanced\Mercury Console (headless / TNC)"; Filename: "{cmd}"; Parameters: "/K mercury.exe -h"; WorkingDir: "{app}"; IconFilename: "{app}\mercury.ico"; Comment: "Command-line modem for headless, TNC and scripted use - advanced users"


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
