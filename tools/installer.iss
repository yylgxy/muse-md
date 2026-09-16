; MuseMD / Markdown 编辑器 —— Inno Setup 安装包脚本（对应规格 8.4）
;
; 用法：
;   1) 先跑一次 tools\package_release.ps1（它会生成 dist\MuseMD-<版本>-win64\）
;   2) 编译本脚本：
;        & 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' tools\installer.iss
;      或者双击本文件用 Inno 的图形界面编译（F9）
;   3) 产物：dist\installer\MuseMD-1.0.0-setup.exe
;
; 设计取舍（都是有意的，改之前先读一下）：
;   * 只打包 dist 里那个已经部署好的目录（Qt DLL、插件、WebEngine 组件都在里面）。
;     安装包不自己挑文件 —— 那样迟早漏一个插件。
;   * 卸载**不删** %APPDATA%\Dev\MarkdownEditor\（配置、搜索索引、版本历史都在那儿）。
;     用户的笔记数据不该因为卸载程序而消失。
;   * 文件关联写 HKCR，需要管理员权限。
;   * Inno Setup 6 的官方语言包里**没有简体中文**（中文是社区翻译）。所以这里只用自带的
;     Default.isl（英文）。需要中文界面时：去 Inno 的翻译仓库下载 ChineseSimplified.isl，
;     放到 Inno 安装目录的 Languages\ 下，然后取消下面 [Languages] 里那一行的注释。

#define MyAppName "MuseMD"
#define MyAppNameCN "Markdown 编辑器"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "yylgxy"
#define MyAppURL "https://github.com/yylgxy/muse-md"
#define MyAppExeName "MarkdownEditor.exe"
; 打包目录：与 tools\package_release.ps1 生成的目录名保持一致
#define MyAppDistDir "..\dist\MuseMD-1.0.0-win64"

[Setup]
AppId={{8F3A2C51-7D64-4C1E-9B2A-0E6A5C1D7E31}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}（{#MyAppNameCN}）
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=no
; x64compatible = 64 位系统（含 ARM64 上的 x64 仿真）；本程序是 64 位 MSVC 构建
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 写文件关联要管理员权限
PrivilegesRequired=admin
; 关联了 .md，通知系统刷新图标缓存
ChangesAssociations=yes
OutputDir=..\dist\installer
OutputBaseFilename={#MyAppName}-{#MyAppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName}（{#MyAppNameCN}）
UninstallDisplayIcon={app}\{#MyAppExeName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
; 装了中文语言包之后取消下面这行的注释（见文件顶部说明）
; Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
Name: "assocmd"; Description: "把 .md / .markdown 文件关联到 {#MyAppName}"; GroupDescription: "文件关联："; Flags: checkedonce

[Files]
; 整个部署目录一起进去（exe + Qt DLL + 插件 + WebEngine 组件 + resources）
Source: "{#MyAppDistDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; ---- .md 与 .markdown 的文件关联（用户没勾选就不写）----
Root: HKCR; Subkey: ".md"; ValueType: string; ValueName: ""; ValueData: "MuseMD.Document"; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKCR; Subkey: ".markdown"; ValueType: string; ValueName: ""; ValueData: "MuseMD.Document"; Flags: uninsdeletevalue; Tasks: assocmd
Root: HKCR; Subkey: "MuseMD.Document"; ValueType: string; ValueName: ""; ValueData: "Markdown 文档"; Flags: uninsdeletekey; Tasks: assocmd
Root: HKCR; Subkey: "MuseMD.Document\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Tasks: assocmd
Root: HKCR; Subkey: "MuseMD.Document\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: assocmd

[Run]
; 装完问一句要不要马上打开（勾了才跑，而且不是静默安装）
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 只删安装目录里由程序生成、安装时不存在的东西。
; ★ 用户数据（%APPDATA%\Dev\MarkdownEditor\ 下的配置、搜索索引、版本历史）**不删** ——
;   卸载程序不该顺手删掉别人的笔记数据。
Type: filesandordirs; Name: "{app}\logs"
