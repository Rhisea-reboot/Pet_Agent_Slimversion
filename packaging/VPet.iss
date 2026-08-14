#define SourceDir GetEnv("VPET_RELEASE_DIR")

[Setup]
AppName=VPet
AppVersion=1.0.0
PrivilegesRequired=lowest
DefaultDirName={localappdata}\VPet
DefaultGroupName=VPet
OutputBaseFilename=VPet-Setup
Compression=lzma2
SolidCompression=yes
DiskSpanning=yes
DiskSliceSize=2000000000
LicenseFile={#SourceDir}\LICENSE

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{userprograms}\VPet"; Filename: "{app}\VPet.exe"
Name: "{userdesktop}\VPet"; Filename: "{app}\VPet.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"

[Run]
Filename: "{app}\VPet.exe"; Description: "Launch VPet"; Flags: nowait postinstall skipifsilent
