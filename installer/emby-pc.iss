; Emby PC — Inno Setup 6 安装包脚本（T33 / M4-1）
;
; 本地编译：
;   iscc /DMyAppVersion=0.1.0 "/DSourceDir=..\dist\emby-pc-0.1.0-win64" installer\emby-pc.iss
; 说明：
;   - CI 会自动读取 app/CMakeLists.txt 的 project(VERSION) 并通过 /D 传入；
;   - SourceDir 由 CI 传绝对路径（本机打包可传相对路径，ISCC 以脚本目录为基准）；
;   - 产物：dist\emby-pc-<version>-setup.exe（≤150MB 目标）。

#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\dist\emby-pc-0.1.0-win64"
#endif

#define MyAppName "Emby PC"
#define MyAppPublisher "Emby PC Project"
#define MyAppExeName "emby-pc.exe"

[Setup]
; 固定 AppId：升级/卸载识别用，绝不要改（改了会被当成另一个产品）。
AppId={{EDD10CCD-4E71-4192-9B5D-7B3A071BB464}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
VersionInfoVersion={#MyAppVersion}.0
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; 安装到 Program Files 需要管理员权限。
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\dist
OutputBaseFilename=emby-pc-{#MyAppVersion}-setup
SetupIconFile=..\app\resources\win\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
AllowNoIcons=yes

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: checkedonce

[Files]
; 免安装包目录（windeployqt + libmpv + 文档）整体安装到 {app}。
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[Code]
// 卸载时询问是否删除用户数据（%LOCALAPPDATA%\emby-pc：日志/缓存/登录信息）。
// 默认保留，便于升级/回滚；静默卸载可用 /DELETEUSERDATA 显式删除。
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    DataDir := ExpandConstant('{localappdata}\emby-pc');
    if DirExists(DataDir) then
    begin
      if UninstallSilent then
      begin
        if Pos('/DELETEUSERDATA', Uppercase(GetCmdTail)) > 0 then
          DelTree(DataDir, True, True, True);
      end
      else
      begin
        if MsgBox('是否同时删除本地用户数据？' + #13#10 + DataDir + #13#10 +
                  '（日志、缓存、登录信息。选择“否”将保留，便于升级或回滚。）',
                  mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
          DelTree(DataDir, True, True, True);
      end;
    end;
  end;
end;
