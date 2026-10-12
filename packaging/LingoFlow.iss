; LingoFlow - Windows installer source (Inno Setup 6, >= 6.3; the GitHub
; windows-latest image ships 6.7.x).
;
; Compile through packaging/build-installer.ps1 ONLY (CI runs it in the
; Release workflow). The script sets the LINGOFLOW_* environment defines;
; every versioned value in this installer is derived from the release Git tag
; that way, so there is exactly one version source. A literal version in this
; file is a defect - scripts/release/tests/release-process.Tests.ps1 fails
; on it.
;
; Design notes (docs/releasing.md):
;   - per-user install by default (PrivilegesRequired=lowest): the venue
;     machines are locked down; no administrator, no reboot;
;   - {autopf}\LingoFlow for a per-user install resolves to
;     %LOCALAPPDATA%\Programs\LingoFlow - the same folder the scripted
;     portable install used, so upgrade paths are stable;
;   - user state lives in %APPDATA%\LingoFlow and the Windows credential
;     store: this installer never writes there and its uninstaller never
;     deletes there;
;   - the AGPL licence chain travels with the binary (installed next to the
;     exe as an uninstallable unit);
;   - only the explicitly listed files are packaged. Debug symbols are not
;     installer content. No drivers, no NDI runtime (NewTek EULA), no
;     VC++ redistributable (the build links the static CRT - verified by
;     the dumpbin gate in packaging/release-package.ps1).

#define MyAppName "LingoFlow"
#define MyAppVersion GetEnv('LINGOFLOW_APP_VERSION')
#define StageDir GetEnv('LINGOFLOW_STAGE_DIR')
#define RepoRoot GetEnv('LINGOFLOW_REPO_ROOT')
#define OutputDir GetEnv('LINGOFLOW_OUTPUT_DIR')

#if MyAppVersion == ""
  #error LingoFlow.iss: LINGOFLOW_APP_VERSION is not set - compile via packaging/build-installer.ps1
#endif
#if StageDir == ""
  #error LingoFlow.iss: LINGOFLOW_STAGE_DIR is not set - compile via packaging/build-installer.ps1
#endif
#if RepoRoot == ""
  #error LingoFlow.iss: LINGOFLOW_REPO_ROOT is not set - compile via packaging/build-installer.ps1
#endif
#if OutputDir == ""
  #error LingoFlow.iss: LINGOFLOW_OUTPUT_DIR is not set - compile via packaging/build-installer.ps1
#endif

[Setup]
; AppId identifies the product across upgrades - never change it in place.
; Per-user and machine installs share one AppId per user account, so running
; the setup again upgrades the existing install instead of orphaning it.
AppId={{4C6F3B8A-9E2D-4B1A-8F7C-2D5A1E0B9C43}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppName}
AppPublisherURL=https://github.com/PhillipMek/LingoFlow
AppUpdatesURL=https://github.com/PhillipMek/LingoFlow/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; Windows 10/11 x64 - the product's documented floor; Win11 builds share the
; same 10.0 major version.
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
; Setup exe version resource: Explorer details and winget-style inventory
; report the release version, not a per-build number.
VersionInfoVersion={#MyAppVersion}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoProductName={#MyAppName}
VersionInfoDescription={#MyAppName} Setup - installation program
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\LingoFlow.exe
; Close a running instance before an upgrade (files would be locked; the app
; saves nothing in its program folder anyway).
CloseApplications=yes
RestartApplications=no
SetupLogging=yes
DisableWelcomePage=no
Compression=lzma2/max
SolidCompression=yes
; The committed multi-resolution icon - the same art the exe carries.
SetupIconFile={#RepoRoot}\assets\LingoFlow.ico
OutputDir={#OutputDir}
OutputBaseFilename=LingoFlow-Setup-{#MyAppVersion}-x64

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "{#StageDir}\LingoFlow.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\VERSION.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\LICENSES\*"; DestDir: "{app}\LICENSES"; Flags: ignoreversion recursesubdirs createallsubdirs

; No WorkingDirectory parameter: the [Icons] default working directory is
; {app}, and Inno Setup 6.7 no longer accepts the parameter at all
; (measured on 6.7.3) - the old portable install.ps1 set it explicitly, the
; installer does not need to.
[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\LingoFlow.exe"; Comment: "LingoFlow - live speech translation"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\LingoFlow.exe"; Comment: "LingoFlow - live speech translation"; Tasks: desktopicon

[Run]
Filename: "{app}\LingoFlow.exe"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
