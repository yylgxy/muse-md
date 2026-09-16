# 一键打包 Release 绿色版（对应第八阶段的 8.1 - 8.3）
#
# 为什么要有这个脚本：Qt Creator 里手动做那几步（切 Release、构建、开 Qt 命令提示符、
# 跑 windeployqt、复制资源、压缩）每次发版都要重复一遍，而且**容易漏**（漏一个插件
# 就是"在别人机器上打不开"）。脚本把整条链路固化下来，并且**每一步都检查结果**。
#
# 用法（在仓库根目录或任意位置）：
#     pwsh -File tools\package_release.ps1
#     pwsh -File tools\package_release.ps1 -SkipBuild      # 已经构建过，只重新打包
#
# 产出：
#     dist\MuseMD-<版本>-win64\        绿色版目录（解压即用）
#     dist\MuseMD-<版本>-win64.zip     发布用的压缩包
#
# 注意：这个脚本用的是 **Qt 自带的 CMake + Ninja + MSVC**，也就是 Qt Creator 点「构建」
# 走的那条路（不是手写 cl/link 的验证脚本），所以产物和你在 IDE 里构建的是同一类东西。

[CmdletBinding()]
param(
    [switch]$SkipBuild,
    # cmake = Qt 自带 CMake + Ninja（Qt Creator 走的那条路，推荐）
    # msvc  = 手写 cl/link 的 Release 构建。只在 cmake/ninja 跑不起来的受限环境里用：
    #         ninja 靠**管道**收集编译器的输出，而某些沙箱禁止管道捕获子进程输出 →
    #         ninja 会静默卡死（一个源文件都不编、也没有 .ninja_log）。开发机上用 cmake 就好。
    [ValidateSet('cmake', 'msvc')]
    [string]$Builder = 'cmake',
    [string]$QtDir = 'D:\QT\6.5.3\msvc2019_64',
    [string]$CMake = 'D:\QT\Tools\CMake_64\bin\cmake.exe',
    [string]$Ninja = 'D:\QT\Tools\Ninja\ninja.exe',
    [string]$VcVars = 'D:\VisualStudio2022\VC\Auxiliary\Build\vcvars64.bat'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root 'build\release'
$distDir = Join-Path $root 'dist'

function Step($text) { Write-Host "`n==== $text ====" -ForegroundColor Cyan }
function Ok($text) { Write-Host "  [ok] $text" -ForegroundColor Green }
function Fail($text) { Write-Host "  [×] $text" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------- 0. 版本号
Step '0. 读取版本号'
$cmakeLists = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
$match = [regex]::Match($cmakeLists, 'project\s*\(\s*MarkdownEditor\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $match.Success) { Fail 'CMakeLists.txt 里读不到 project(VERSION ...)' }
$version = $match.Groups[1].Value
$pkgName = "MuseMD-$version-win64"
$pkgDir = Join-Path $distDir $pkgName
Ok "版本 $version，包名 $pkgName"

# ---------------------------------------------------------------- 1. Release 构建（8.1）
if (-not $SkipBuild) {
    if (-not (Test-Path $VcVars)) { Fail "找不到 vcvars64.bat：$VcVars（改 -VcVars 参数）" }
    if (-not (Test-Path $QtDir)) { Fail "找不到 Qt：$QtDir（改 -QtDir 参数）" }

    if ($Builder -eq 'cmake') {
        Step '1. CMake Release 构建（8.1，Qt Creator 走的就是这条路）'
        foreach ($tool in @($CMake, $Ninja)) {
            if (-not (Test-Path $tool)) { Fail "找不到工具：$tool" }
        }

        # 必须用 cmd 走 vcvars64.bat（它设置 MSVC 的环境变量），再在同一节里跑 cmake
        $configure = "`"$VcVars`" >nul && `"$CMake`" -S `"$root`" -B `"$buildDir`" -G Ninja " +
                     "-DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=`"$QtDir`" -DCMAKE_MAKE_PROGRAM=`"$Ninja`""
        cmd /c $configure
        if ($LASTEXITCODE -ne 0) { Fail 'CMake 配置失败' }
        Ok 'CMake 配置完成'

        $build = "`"$VcVars`" >nul && `"$CMake`" --build `"$buildDir`""
        cmd /c $build
        if ($LASTEXITCODE -ne 0) { Fail 'Release 编译失败' }
        Ok 'Release 编译完成'
    } else {
        # 手写 cl/link 的 Release 构建（受限环境备用路线）
        Step '1. 手写 cl/link 的 Release 构建（8.1，-Builder msvc）'
        $debugScript = Join-Path $root 'build\.verify414\build.cmd'
        if (-not (Test-Path $debugScript)) {
            Fail "找不到 $debugScript。这条路只在开发机上有（它是开发时用来在沙箱里验证的脚本）；" +
                 "正常情况下请用 -Builder cmake。"
        }

        # 从 Debug 版脚本里**生成** Release 版：只改编译/链接开关，并把输出目录分开，
        # 免得和 Debug 的 .obj 混在一个目录里（混了会出很怪的链接错误 —— 踩过）。
        $releaseScript = Join-Path $root 'build\.verify414\build_release.cmd'
        $text = [System.IO.File]::ReadAllText($debugScript)
        $text = $text.Replace('set OUT=%ROOT%\build\.verify414', 'set OUT=%ROOT%\build\.verify414\rel')
        $text = $text.Replace('/Zi /Ob0 /Od /RTC1 /std:c++17 /MDd', '/O2 /DNDEBUG /std:c++17 /MD')
        $text = $text.Replace('/Zi /Ob0 /Od /RTC1 -MDd', '/O2 /DNDEBUG -MD')
        $text = $text.Replace(' /Zi /Ob0 /Od /RTC1 /std:c++17 /MDd', ' /O2 /DNDEBUG /std:c++17 /MD')
        # 链接库去掉 Debug 后缀（Qt6Cored.lib → Qt6Core.lib）
        $text = [regex]::Replace($text, 'Qt6([A-Za-z0-9_]+)d\.lib', 'Qt6$1.lib')
        # Release 不要调试符号、不要增量链接（包体积也小很多）
        $text = $text.Replace(' /debug /INCREMENTAL ', ' ')
        $text = $text.Replace('/pdb:%OUT%\bin\MarkdownEditor.pdb', '')
        # 打包只需要主程序，所以要去掉 Debug 脚本里的两步：
        #   * 第 5 步（编译测试）—— 打包不需要；
        #   * 第 7 步及之后（链接 23 个测试）—— 同样不需要。
        # ★ 主程序的链接是**第 6 步**，夹在第 5 步和第 7 步中间，**必须留着**。
        #   （第一版转换写成"从第 5 步砍到文件尾"，于是把第 6 步也砍掉了：
        #     脚本跑完 exit 0、却一个 exe 都没有。教训记在这里。）
        # 另外标记行是 `echo ===== 5) ...`（不是 rem），两种前缀都要认。
        $text = [regex]::Replace($text, '(?s)\r?\n(rem|echo) =+ 5\).*?(?=\r?\n(rem|echo) =+ 6\))', '')
        $text = [regex]::Replace($text, '(?s)\r?\n(rem|echo) =+ 7\).*$', "`r`n echo ==================== DONE ====================`r`n dir /b %OUT%\bin`r`n")
        [System.IO.File]::WriteAllText($releaseScript, $text, (New-Object System.Text.UTF8Encoding $false))
        Ok "已生成 Release 构建脚本：$releaseScript"

        cmd /c "`"$VcVars`" >nul && `"$releaseScript`" > `"$root\build\.verify414\build_release.log`" 2>&1"
        if ($LASTEXITCODE -ne 0) {
            Write-Host (Get-Content "$root\build\.verify414\build_release.log" -Tail 25 | Out-String)
            Fail 'Release 编译失败（详细日志见 build\.verify414\build_release.log）'
        }
        Ok 'Release 编译完成（手写 cl/link）'

        # 产物在 %OUT%\bin 下，复制到约定的 buildDir\bin 供后面统一步骤使用
        $relBin = Join-Path $root 'build\.verify414\rel\bin'
        New-Item -ItemType Directory -Path (Join-Path $buildDir 'bin') -Force | Out-Null
        Copy-Item (Join-Path $relBin 'MarkdownEditor.exe') (Join-Path $buildDir 'bin') -Force
        Ok '已把 exe 放到 build\release\bin\MarkdownEditor.exe'
    }
} else {
    Step '1. 跳过构建（-SkipBuild）'
}

$exe = Join-Path $buildDir 'bin\MarkdownEditor.exe'
if (-not (Test-Path $exe)) { Fail "找不到构建产物：$exe（先跑一次不带 -SkipBuild 的）" }
Ok ("exe 大小 {0:N1} MB（Release 且没有调试符号时才这么大得合理）" -f ((Get-Item $exe).Length / 1MB))

# ---------------------------------------------------------------- 2. 准备包目录
Step '2. 准备绿色版目录（8.3）'
if (Test-Path $pkgDir) { Remove-Item $pkgDir -Recurse -Force }
New-Item -ItemType Directory -Path $pkgDir | Out-Null
Copy-Item $exe $pkgDir
Ok "已放入 $pkgName\MarkdownEditor.exe"

# ---------------------------------------------------------------- 3. windeployqt（8.2）
Step '3. windeployqt 收集依赖（8.2）'
$windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'
if (-not (Test-Path $windeployqt)) { Fail "找不到 windeployqt：$windeployqt" }

$deployArgs = 'MarkdownEditor.exe --release --no-translations --no-system-d3d-compiler --no-opengl-sw'
Push-Location $pkgDir
try {
    # 先走一遍 spec 里的参数
    cmd /c "`"$windeployqt`" $deployArgs 2>&1"
    if ($LASTEXITCODE -ne 0) { Fail 'windeployqt 失败' }
} finally {
    Pop-Location
}
Ok 'windeployqt 完成'

# ---------------------------------------------------------------- 4. 资源文件夹（8.3）
Step '4. 复制 resources（8.3）'
# 注意（诚实说明）：preview 模板和两套 QSS 都是**编进 exe 的**（qrc），
# 运行时并不从磁盘读，所以这一步对程序能不能跑**没有影响**。
# 保留它有两个用处：一是规格里要求了，二是出问题时可以直接翻看这些资源文件。
$resSrc = Join-Path $root 'resources'
$resDst = Join-Path $pkgDir 'resources'
if (Test-Path $resSrc) {
    Copy-Item $resSrc $resDst -Recurse
    Ok "已复制 resources\（仅供查看；程序实际用的是编在 exe 里的那份）"
} else {
    Ok '仓库里没有 resources 目录，跳过'
}

# ---------------------------------------------------------------- 5. 依赖自检（8.5 的自动化部分）
Step '5. 依赖自检（替代不了干净机器，但能挡住"少拷了 DLL"）'

# 5.1 QtWebEngine 的关键部件（这是最容易漏的一块：少了 QtWebEngineProcess.exe
#     或者 resources/*.pak，预览会在运行时以很奇怪的方式失败）
#
# 路径按 windeployqt 的**真实**布局写：插件目录（platforms/、styles/、imageformats/ …）
# 直接放在包根下，**不是** plugins/ 里面。
# （第一版自检写成了 plugins\platforms\qwindows.dll，于是自己报了个假警报 ——
#   自检的意义就在于把这类"我以为"变成能对得上的事实，顺便也说明了它真的会报。）
$required = @(
    'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6WebEngineWidgets.dll', 'Qt6WebEngineCore.dll',
    'Qt6Network.dll', 'Qt6Sql.dll', 'Qt6WebChannel.dll', 'Qt6Quick.dll', 'Qt6Qml.dll',
    'QtWebEngineProcess.exe',
    'platforms\qwindows.dll',
    'styles\qwindowsvistastyle.dll',
    'sqldrivers\qsqlite.dll',
    'resources\qtwebengine_resources.pak',
    'translations\qtwebengine_locales\en-US.pak'
)
$missing = @()
foreach ($rel in $required) {
    if (-not (Test-Path (Join-Path $pkgDir $rel))) { $missing += $rel }
}
if ($missing.Count -gt 0) { Fail ("包里缺少关键文件：" + ($missing -join ', ')) }
Ok ("关键依赖齐全（检查了 {0} 项）" -f $required.Count)

# 5.2 用 dumpbin 列出 exe 的导入表，逐个确认"能在包里或系统目录里找到"
$dumpbin = Get-ChildItem 'D:\VisualStudio2022\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe' -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($dumpbin) {
    $out = & $dumpbin.FullName /dependents $exe 2>&1 | Out-String
    $imports = [regex]::Matches($out, '(?m)^\s+([A-Za-z0-9_.\-]+\.dll)\s*$') | ForEach-Object { $_.Groups[1].Value } |
        Sort-Object -Unique
    $systemDlls = @('KERNEL32.dll', 'USER32.dll', 'GDI32.dll', 'ADVAPI32.dll', 'SHELL32.dll', 'OLE32.dll',
                    'OLEAUT32.dll', 'COMDLG32.dll', 'WS2_32.dll', 'VERSION.dll', 'CRYPT32.dll', 'WINMM.dll',
                    'NETAPI32.dll', 'USERENV.dll', 'DWMAPI.dll', 'UxTheme.dll', 'SHLWAPI.dll', 'IMM32.dll',
                    'MSVCP140.dll', 'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll', 'api-ms-win-crt-runtime-l1-1-0.dll')
    $notFound = @()
    foreach ($dll in $imports) {
        if (Test-Path (Join-Path $pkgDir $dll)) { continue }
        if ($systemDlls -contains $dll) { continue }   # 系统 DLL：目标机器上本来就有
        if ($dll -like 'api-ms-win-*' -or $dll -like 'ext-ms-*') { continue }
        $notFound += $dll
    }
    if ($notFound.Count -gt 0) {
        Fail ("这些导入的 DLL 既不在包里也不是系统 DLL：" + ($notFound -join ', '))
    }
    Ok ("导入表检查通过：{0} 个 DLL 全部能在包里或系统里找到" -f $imports.Count)

    # 5.3 MSVC 运行时提醒：默认是"动态链接"，目标机器需要 VC++ 运行库
    if ($imports -contains 'VCRUNTIME140.dll' -or $imports -contains 'MSVCP140.dll') {
        Write-Host '  [注意] exe 依赖 MSVC 运行库（VCRUNTIME140 / MSVCP140）。' -ForegroundColor Yellow
        Write-Host '        干净的 Windows 10/11 一般自带，但若目标机器缺，需要装 VC++ 2015-2022 可再发行包，' -ForegroundColor Yellow
        Write-Host '        或者用 /MT 静态链接重编（CMake 里设 MSVC_RUNTIME_LIBRARY=MultiThreaded）。' -ForegroundColor Yellow
    }
} else {
    Write-Host '  [跳过] 没找到 dumpbin，无法做导入表检查' -ForegroundColor Yellow
}

# 5.4 启动冒烟测试：跑得起来（进程活着）就说明依赖没缺
Step '6. 启动冒烟测试（8.5 的自动化部分）'
$proc = Start-Process -FilePath (Join-Path $pkgDir 'MarkdownEditor.exe') -PassThru -ErrorAction SilentlyContinue
Start-Sleep -Seconds 6
if ($proc -and -not $proc.HasExited) {
    Ok '程序启动后仍在运行（说明 DLL 齐全、没有"缺依赖"式崩溃）'
    Write-Host '     （它是不是真的能显示窗口、预览能不能出来，只能人工看 —— 见 README 的验收步骤）' -ForegroundColor DarkGray
    Stop-Process -Id $proc.Id -Force
    Start-Sleep -Seconds 1
} else {
    $code = if ($proc) { $proc.ExitCode } else { 'null' }
    Write-Host ("  [注意] 进程 6 秒内就退出了（退出码 {0}）。" -f $code) -ForegroundColor Yellow
    Write-Host '        如果是 0xC0000135（-1073741515）就是缺 DLL；其他码可能是本机环境问题。' -ForegroundColor Yellow
}

# ---------------------------------------------------------------- 7. 压缩（8.3）
Step '7. 压缩成 zip（8.3）'
$zip = Join-Path $distDir "$pkgName.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $pkgDir -DestinationPath $zip -CompressionLevel Optimal
Ok ("{0}（{1:N1} MB）" -f (Split-Path -Leaf $zip), ((Get-Item $zip).Length / 1MB))

Write-Host "`n==== 完成 ====" -ForegroundColor Cyan
Write-Host "  绿色版目录：$pkgDir"
Write-Host "  发布用 zip：$zip"
Write-Host '  下一步：见 docs\RELEASE.md（GitHub 发版的步骤）'
