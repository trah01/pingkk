#ifndef SourceDir
  #define SourceDir "..\..\dist\bin"
#endif
#ifndef OutputDir
  #define OutputDir "..\.."
#endif
#ifndef AppVersion
  #define AppVersion "0.1.1"
#endif

#ifndef TargetArch
  #define TargetArch "x64"
#endif

[Setup]
AppId={{DDF63DE8-6F73-4F94-B5E9-AD8AEBA82EF2}
AppName=ping看看
AppVerName=ping看看 {#AppVersion}
AppVersion={#AppVersion}
AppPublisher=trah01
AppPublisherURL=https://github.com/trah01/pingkk
AppSupportURL=https://github.com/trah01/pingkk/issues
AppUpdatesURL=https://github.com/trah01/pingkk/releases
DefaultDirName={autopf}\pingkk
DefaultGroupName=ping看看
DisableProgramGroupPage=yes
OutputDir={#OutputDir}
OutputBaseFilename=pingkk-windows-{#TargetArch}-setup
SetupIconFile=..\..\assets\pingkk-icon.ico
UninstallDisplayIcon={app}\pingkk-gui.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
#if TargetArch == "arm64"
MinVersion=10.0.17763
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
MinVersion=6.1sp1
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif
PrivilegesRequired=admin
ChangesAssociations=no
ChangesEnvironment=yes
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#AppVersion}
VersionInfoCompany=trah01
VersionInfoDescription=ping看看安装程序
VersionInfoProductName=pingkk
VersionInfoProductVersion={#AppVersion}
VersionInfoCopyright=trah01

[Languages]
#if FileExists(AddBackslash(CompilerPath) + "Languages\ChineseSimplified.isl")
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
#else
  #pragma warning "ChineseSimplified.isl is unavailable; using the built-in English installer language."
#endif
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\ping看看"; Filename: "{app}\pingkk-gui.exe"
Name: "{group}\卸载 ping看看"; Filename: "{uninstallexe}"
Name: "{autodesktop}\ping看看"; Filename: "{app}\pingkk-gui.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\pingkk-gui.exe"; Description: "启动 ping看看"; Flags: nowait postinstall skipifsilent

[Code]
const
  EnvironmentKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';
  PingkkKey = 'Software\pingkk';

function PathContains(const PathValue, Entry: string): Boolean;
var
  NormalizedPath: string;
  NormalizedEntry: string;
begin
  NormalizedPath := Lowercase(';' + PathValue + ';');
  NormalizedEntry := Lowercase(';' + Entry + ';');
  Result := Pos(NormalizedEntry, NormalizedPath) > 0;
end;

procedure AddInstallDirToPath;
var
  PathValue: string;
  InstallDir: string;
begin
  InstallDir := ExpandConstant('{app}');
  if not RegQueryStringValue(HKLM, EnvironmentKey, 'Path', PathValue) then
    PathValue := '';
  if not PathContains(PathValue, InstallDir) then
  begin
    if (PathValue <> '') and (PathValue[Length(PathValue)] <> ';') then
      PathValue := PathValue + ';';
    RegWriteExpandStringValue(HKLM, EnvironmentKey, 'Path', PathValue + InstallDir);
    RegWriteDWordValue(HKLM, PingkkKey, 'PathAdded', 1);
  end
  else
  begin
    RegWriteDWordValue(HKLM, PingkkKey, 'PathAdded', 0);
  end;
end;

procedure RemoveInstallDirFromPath;
var
  PathValue: string;
  InstallDir: string;
  PaddedPath: string;
  Target: string;
  PathAdded: Cardinal;
begin
  PathAdded := 0;
  if not RegQueryDWordValue(HKLM, PingkkKey, 'PathAdded', PathAdded) or
     (PathAdded <> 1) then
    Exit;
  if not RegQueryStringValue(HKLM, EnvironmentKey, 'Path', PathValue) then
    Exit;
  InstallDir := ExpandConstant('{app}');
  PaddedPath := ';' + PathValue + ';';
  Target := ';' + InstallDir + ';';
  StringChangeEx(PaddedPath, Target, ';', True);
  while (Length(PaddedPath) > 0) and (PaddedPath[1] = ';') do
    Delete(PaddedPath, 1, 1);
  while (Length(PaddedPath) > 0) and (PaddedPath[Length(PaddedPath)] = ';') do
    Delete(PaddedPath, Length(PaddedPath), 1);
  RegWriteExpandStringValue(HKLM, EnvironmentKey, 'Path', PaddedPath);
  RegDeleteKeyIncludingSubkeys(HKLM, PingkkKey);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    AddInstallDirToPath;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RemoveInstallDirFromPath;
end;
