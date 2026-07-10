; =====================================================================
; Inno Setup Script for Mercury HF Modem (Windows Installer)
; =====================================================================

#define MyAppName "Mercury HF Modem"
#define MyAppVersion "1.9.9"
#define MyAppPublisher "© 2026 Rhizomatica Communications"
#define MyAppURL "https://github.com/Rhizomatica/mercury"
#define MyAppExeName "mercury-ui.exe"

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
OutputBaseFilename=Mercury_HF_Modem_Setup
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


[Files]
Source: "run_mercury.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "mercury.ini"; DestDir: "{app}"; Flags: ignoreversion
Source: "../LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "mercury_icon.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "libgcc_s_seh-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libhamlib-4.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libusb-1.0.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "libwinpthread-1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "mercury-ui.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\mercury_icon.ico"; Comment: "Launch Mercury HF Modem (GUI)"
Name: "{group}\Configure Hardware (mercury.ini)"; Filename: "notepad.exe"; Parameters: "{app}\mercury.ini"; Comment: "Edit Mercury Audio & PTT Configuration"
;Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstexe}"; Comment: "Uninstall Mercury HF Modem"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\mercury_icon.ico"; Tasks: desktopicon; Comment: "Launch Mercury HF Modem"


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
