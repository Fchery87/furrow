; Furrow Windows Installer (Inno Setup)
; Build with: iscc /DAppVersion=1.2.0 /DBuildDir=..\build-win\Furrow_artefacts\Release installer-windows.iss
#ifndef AppVersion
  #define AppVersion "1.2.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\build-win\Furrow_artefacts\Release"
#endif

[Setup]
AppName=Furrow
AppVersion={#AppVersion}
AppPublisher=No Chaserz
DefaultDirName={autopf}\No Chaserz\Furrow
DefaultGroupName=Furrow
LicenseFile=..\LICENSE
OutputBaseFilename=Furrow-{#AppVersion}-Windows-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Types]
Name: "full"; Description: "Full installation (VST3, CLAP, Standalone)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 Plugin"; Types: full custom
Name: "clap"; Description: "CLAP Plugin"; Types: full custom
Name: "standalone"; Description: "Standalone Application"; Types: full custom

[Files]
Source: "{#BuildDir}\VST3\Furrow.vst3\*"; DestDir: "{commoncf64}\VST3\Furrow.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: vst3
Source: "{#BuildDir}\CLAP\Furrow.clap"; DestDir: "{commoncf64}\CLAP"; Flags: ignoreversion; Components: clap
Source: "{#BuildDir}\Standalone\Furrow.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: standalone
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\user-guide.md"; DestDir: "{app}\docs"; Flags: ignoreversion

[Icons]
Name: "{group}\Furrow"; Filename: "{app}\Furrow.exe"; Components: standalone
