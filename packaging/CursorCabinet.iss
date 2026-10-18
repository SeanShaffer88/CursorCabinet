#ifndef PackageDir
  #error PackageDir must identify the staged portable folder
#endif
#ifndef OutputPath
  #define OutputPath "..\releases"
#endif
#ifndef AppVersion
  #define AppVersion "0.3.0"
#endif
#ifndef TargetArch
  #define TargetArch "x64"
#endif

[Setup]
AppId={{EBFD397B-6FB6-4439-8297-C82FD4EBA133}
AppName=CursorCabinet
AppVersion={#AppVersion}
AppPublisher=Sean Shaffer
AppPublisherURL=https://github.com/SeanShaffer88/CursorCabinet
AppSupportURL=https://github.com/SeanShaffer88/CursorCabinet/issues
AppCopyright=Copyright (c) 2026 Sean Shaffer
VersionInfoDescription=CursorCabinet installer
DefaultDirName={localappdata}\Programs\CursorCabinet
DefaultGroupName=CursorCabinet
PrivilegesRequired=lowest
MinVersion=10.0.17763
#if TargetArch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif
OutputDir={#OutputPath}
OutputBaseFilename=CursorCabinet-{#AppVersion}-{#TargetArch}-Setup
Compression=lzma2/normal
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\assets\CursorCabinet.ico
WizardImageFile=..\assets\wizard.bmp
WizardSmallImageFile=..\assets\wizard-small.bmp
WizardImageStretch=yes
AppComments=Browse, preview, and apply free Windows cursor sets.
UninstallDisplayIcon={app}\CursorCabinet.exe
LicenseFile={#PackageDir}\LICENSE.txt
CloseApplications=yes
RestartApplications=no
SetupMutex=CursorCabinetSetup
DisableProgramGroupPage=yes
DisableWelcomePage=no
DisableDirPage=no

[Messages]
WelcomeLabel1=Welcome to CursorCabinet
WelcomeLabel2=Browse free cursor sets, preview their pointers, and choose a new look for Windows.%n%nCursorCabinet installs for your Windows account. No administrator access or extra runtime is required.
FinishedLabel=CursorCabinet is ready.%n%nBrowse the library, select a set, and choose Apply selected set. Your previous scheme is saved automatically.

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\CursorCabinet"; Filename: "{app}\CursorCabinet.exe"
Name: "{userdesktop}\CursorCabinet"; Filename: "{app}\CursorCabinet.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\CursorCabinet.exe"; Description: "Launch CursorCabinet"; Flags: nowait postinstall skipifsilent

; Keep the separate app-data folder: Windows may still be using its installed cursors.
; Only the application and its packaged files are removed by the generated uninstaller.
