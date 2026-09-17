# 打包与发布（第八阶段）

这份文档对应规格里的 8.1 - 8.5。**能自动化的部分都自动化了**，剩下必须人工的部分
（干净机器验收、点界面上传）也写清了怎么做、要验证什么。

---

## 8.1 - 8.3 一键完成

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package_release.ps1
```

脚本会依次做完这几件事（每一步都检查结果，失败就停）：

| 步骤 | 做什么 | 对应规格 |
|---|---|---|
| 1 | 用 **Qt 自带的 CMake + Ninja + MSVC** 配置并编译 Release | 8.1 |
| 2 | 建 `dist\MuseMD-<版本>-win64\`，把 exe 放进去 | 8.3 |
| 3 | 跑 `windeployqt MarkdownEditor.exe --release --no-translations --no-system-d3d-compiler --no-opengl-sw` | 8.2 |
| 4 | 复制 `resources\` 进去 | 8.3 |
| 5 | **依赖自检**：关键文件清单 + `dumpbin /dependents` 导入表逐个核对 | 8.5（自动化部分） |
| 6 | **启动冒烟测试**：把打包好的 exe 跑起来，6 秒后仍活着 = 依赖没缺 | 8.5（自动化部分） |
| 7 | 压缩成 `dist\MuseMD-<版本>-win64.zip` | 8.3 |

参数：

```powershell
# 已经构建过，只想重新打包（跳过编译）
... -File tools\package_release.ps1 -SkipBuild

# Qt / 工具装在别的位置
... -File tools\package_release.ps1 -QtDir 'D:\QT\6.5.3\msvc2019_64'
```

### 也可以按规格手动做（Qt Creator 路线）

1. Qt Creator 左下角套件切 **Release** → 构建。
   产物在 `build-markdownEditor-...-Release\bin\MarkdownEditor.exe`。
2. 新建空文件夹，把 exe 复制进去。
3. 开始菜单打开「Qt 6.5.3 (MSVC 2019 64-bit) 命令提示符」，`cd` 到那个文件夹，执行：
   ```cmd
   windeployqt MarkdownEditor.exe --no-translations --no-system-d3d-compiler --no-opengl-sw
   ```
4. 把 `resources\` 也复制进去，压缩成 zip。

> 关于 `resources\`（诚实说明）：预览模板和两套 QSS 都是**编译进 exe 的**（`resources/resources.qrc`），
> 运行时从 `:/html/...`、`:/styles/...` 读，**不从磁盘读**。所以这一步对"程序能不能跑"没有影响，
> 保留它是因为规格里要求了、而且出问题时可以直接翻看这些资源。真正依赖磁盘的是：
> 用户自己打开的 `.md` 文件、以及 `%APPDATA%\Dev\MarkdownEditor\` 下的配置/索引/版本历史。

---

## 8.4 制作安装包（Inno Setup）

Inno Setup 本机没装，先装它：<https://jrsoftware.org/isdl.php>

编译脚本已经写好：`tools\installer.iss`（支持开始菜单、桌面快捷方式、卸载、`.md` 文件关联）。

```powershell
# 命令行编译（装完 Inno 后 ISCC.exe 通常在 C:\Program Files (x86)\Inno Setup 6\）
& 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' tools\installer.iss
```

或者直接双击 `tools\installer.iss` 用 Inno 的图形界面编译（F9）。

产物：`dist\installer\MuseMD-<版本>-setup.exe`

几个值得注意的点（都写在 .iss 的注释里了）：

- 安装包**不包含** Qt/Chromium 之外的用户数据；卸载不会碰 `%APPDATA%\Dev\MarkdownEditor\`
  （用户的笔记索引和版本历史留着，卸载不该删用户数据）。
- 文件关联写 `HKCR`，需要管理员权限（`PrivilegesRequired=admin`）。
- Inno Setup 6 **官方语言包里没有简体中文**（中文是社区翻译）。所以脚本里只用自带的英文，
  需要中文界面时按 `.iss` 里的注释加一行、把社区翻译的 `.isl` 放进去即可。

---

## 8.5 干净机器验收（必须人工做）

自动化能挡住的：**缺 DLL**（导入表核对 + 启动冒烟）。
自动化挡不住的：**界面和功能是不是真的正常**（这台开发机上 Chromium 起不来，
打包脚本不会去点界面）。所以请按下面这份清单在**没有装过 Qt 的 Windows** 上走一遍。

准备：把 `dist\MuseMD-1.0.0-win64.zip` 拷到目标机器，解压到任意目录（比如 `D:\Tools\MuseMD`）。

| # | 步骤 | 期望结果 |
|---|---|---|
| 1 | 双击 `MarkdownEditor.exe` | 窗口打开，不报"缺少 xxx.dll" |
| 2 | 看预览区 | 显示示例/空文档的渲染结果（不是白屏） |
| 3 | 输入一段 Markdown（标题、列表、代码块、表格） | 预览实时更新；打字不卡 |
| 4 | Ctrl+S 保存到一个新建文件夹 | 文件写出；标签页显示文件名 |
| 5 | 切暗色主题（Ctrl+Shift+T） | 菜单/面板/编辑器/预览**整体**变暗，没有残留白块 |
| 6 | 滚动编辑区和预览 | 跟手、不卡（日志里有帧统计数字可对照） |
| 7 | 导出 HTML 并打开 | 浏览器里排版正确、样式内联、图片在 |
| 8 | 导出 PDF | 生成 PDF，中文不乱码，代码块不跨页 |
| 9 | 拖一个 `.md` 到窗口里 | 打开成新标签 |
| 10 | 关掉程序再打开 | 恢复上次的文件与窗口状态 |
| 11 | 右键 → 属性 → 兼容性 | 无需任何兼容性设置 |
| 12 | 用记事本改一下正在编辑的文件，再切回程序 | 提示"文件已被外部修改"，可选重载 |

如果第 1 步就失败：把报错弹窗里的 DLL 名字发出来（那就是 `dumpbin` 检查没覆盖到的
系统组件，通常装 VC++ 2015-2022 可再发行包即可）。

---

## 在 GitHub 发布第一个版本

### 已经准备好的东西

- `dist\MuseMD-1.0.0-win64.zip`（发布附件）
- 版本号来自 `CMakeLists.txt` 的 `project(... VERSION 1.0.0)`，写进了 exe（`--version`/关于对话框）
- 发版说明草稿见下面的"Release 说明"一节，可直接粘贴

### 方式 A：命令行（有 gh CLI 时最省事）

```powershell
git tag -a v1.0.0 -m "第一个版本"
git push origin v1.0.0
gh release create v1.0.0 dist\MuseMD-1.0.0-win64.zip `
   --title "MuseMD 1.0.0 —— 第一个版本" `
   --notes-file docs\RELEASE_NOTES_v1.0.0.md
```

### 方式 B：网页界面（本机没装 gh 时用这个）

1. 先把标签推上去（这一步本机就能做）：
   ```powershell
   git tag -a v1.0.0 -m "第一个版本"
   git push origin v1.0.0
   ```
2. 打开 <https://github.com/yylgxy/muse-md/releases/new>
3. **Choose a tag** 选 `v1.0.0`；标题填 `MuseMD 1.0.0 —— 第一个版本`
4. 说明内容粘贴 `docs\RELEASE_NOTES_v1.0.0.md`
5. 把 `dist\MuseMD-1.0.0-win64.zip` 拖到 "Attach binaries by dropping them here"
6. 点 **Publish release**

### 发布前自查

- [ ] `package_release.ps1` 跑完没有红色 `[×]`
- [ ] 干净机器上按 8.5 的清单走过一遍（至少 1、2、3、5、6、7 这几条）
- [ ] `README.md` 里的功能表与测试数量是当前的
- [ ] 版本号在 `CMakeLists.txt`、关于对话框、GitHub 标签三处一致

---

## 同步到 Gitee（国内分享更方便）

GitHub 在国内经常连不上，所以**同一份代码可以同时推到 Gitee**。这里的做法是把
本地仓库当成唯一源头，GitHub 和 Gitee 都是它的镜像 —— 不经过 GitHub 中转。

### 一次性配置

```powershell
git remote add gitee https://gitee.com/<你的用户名>/<仓库名>.git
git remote -v          # 确认有 origin(GitHub) 和 gitee 两个远端
```

### 每次发新版之后同步

```powershell
git push gitee main            # 推分支
git push gitee --tags          # 推标签（发行版要用）
```

### 认证怎么过

Gitee 用 HTTPS 推送要凭据，**推荐用私人令牌**（比账号密码安全，也能随时吊销）：

1. Gitee → 右上头像 → 设置 → 安全设置 → **私人令牌** → 生成新令牌
2. 权限至少勾 `projects`
3. 推送时用户名填 Gitee 用户名，**密码位置填这个令牌**
4. 令牌只显示一次，复制下来存好

Windows 上首次推送会弹出凭据窗口（Git Credential Manager），填一次之后会被记住 ✓。
也可以让它记住：

```powershell
git config --global credential.helper manager
```

### 离线传递源码（没有网络时也能给别人）

仓库可以打成一个**离线包**（含完整历史与标签，几百 KB）：

```powershell
git bundle create dist\MuseMD-repo.bundle --all     # 打包
git bundle verify dist\MuseMD-repo.bundle           # 验证
git clone dist\MuseMD-repo.bundle MuseMD-src        # 别人从它克隆
```

### Gitee 上放发布包

Gitee 的「发行版」可以挂附件，但**92 MB 的 zip 建议按需上传**：
在 Gitee 仓库页面 → 发行版 → 新建发行版 → 上传附件。日常分享用网盘/微信传 zip 更快。
注意**不要把 zip 提交进仓库**（Gitee 对单文件和仓库容量都有限制，而且二进制塞进 git 会越滚越大）。

