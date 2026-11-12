; Inno Setup script for InkPDF
#define AppName "InkPDF"
#define AppVersion "1.0"

[Setup]
AppId={{6B1E4C52-3D7A-4F0B-9A8E-2C5D7F1A9E34}
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
OutputDir=Output
OutputBaseFilename=InkPDF-Setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\InkPDF.exe
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
Name: "openwith"; Description: "Show InkPDF in the ""Open with"" list for PDF files"

[Files]
Source: "dist\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\InkPDF"; Filename: "{app}\InkPDF.exe"
Name: "{autodesktop}\InkPDF"; Filename: "{app}\InkPDF.exe"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\InkPDF.pdf"; ValueType: string; ValueData: "PDF Document (InkPDF)"; Flags: uninsdeletekey; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\InkPDF.pdf\shell\open\command"; ValueType: string; ValueData: """{app}\InkPDF.exe"" ""%1"""; Tasks: openwith
Root: HKA; Subkey: "Software\Classes\.pdf\OpenWithProgids"; ValueType: none; ValueName: "InkPDF.pdf"; Flags: uninsdeletevalue; Tasks: openwith

[Run]
Filename: "{app}\InkPDF.exe"; Description: "Launch InkPDF"; Flags: nowait postinstall skipifsilent
