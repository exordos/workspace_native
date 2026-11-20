[Setup]
AppId=com.exordos.workspace.native.preview
AppName=Workspace Native Preview
AppVersion={#AppVersion}
AppPublisher=Exordos
DefaultDirName={localappdata}\Programs\Exordos\Workspace Native Preview
PrivilegesRequired=lowest
#if TargetArch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\bin\workspace-native.exe
Compression=lzma2/fast
SolidCompression=yes

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\Workspace Native Preview"; Filename: "{app}\bin\workspace-native.exe"; AppUserModelID: "com.exordos.workspace.native.preview"
