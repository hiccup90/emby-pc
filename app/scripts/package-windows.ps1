#requires -Version 5.1
<#
.SYNOPSIS
    构建 Emby PC 的 Windows x64 Release 发布包（免安装 zip）。

.DESCRIPTION
    流程：
      1. cmake 配置 + 构建 Release；
      2. 复制 emby-pc.exe 与 libmpv-2.dll 到 dist\emby-pc-<version>-win64\；
      3. 调用 Qt windeployqt 部署 Qt 运行时（平台插件、翻译等）；
      4. 附带 docs\windows-acceptance.md、docs\libmpv-source.md；
      5. 压缩为 dist\emby-pc-<version>-win64.zip。

    前置条件：
      * 已安装 Qt 6.5+（MSVC 2022 x64 或 MinGW）与 CMake 3.21+；
      * app\libs\libmpv\libmpv-2.dll 已准备好（见 scripts\fetch-libmpv.ps1）。

.PARAMETER QtPrefix
    Qt 安装前缀，例如 C:\Qt\6.8.0\msvc2022_64。
    不传时依次尝试环境变量 QTDIR、PATH 中的 windeployqt.exe。

.PARAMETER BuildDir
    构建目录，默认 app\build-windows。

.PARAMETER Configuration
    构建配置，默认 Release（发布包应当用 Release）。

.PARAMETER Version
    版本号，默认读 app\CMakeLists.txt 的 project(... VERSION x.y.z)，读不到则用 0.1.0。

.PARAMETER Generator
    CMake 生成器。默认交给 CMake 自动选择（Windows 上通常是 Visual Studio 17 2022）。

.PARAMETER Arch
    配合 Visual Studio 生成器的平台，默认 x64。

.PARAMETER SkipBuild
    跳过 cmake 配置与构建，直接用 BuildDir 里已有产物打包（重打包/调试用）。

.PARAMETER NoZip
    只生成 dist 下的免安装目录，不压缩成 zip。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1 -QtPrefix C:\Qt\6.8.0\msvc2022_64

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1 -QtPrefix C:\Qt\6.8.0\msvc2022_64 -Version 0.1.1
#>
[CmdletBinding()]
param(
    [string]$QtPrefix = "",
    [string]$BuildDir = "",
    [string]$Configuration = "Release",
    [string]$Version = "",
    [string]$Generator = "",
    [string]$Arch = "x64",
    [switch]$SkipBuild,
    [switch]$NoZip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$AppDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$RepoRoot = (Resolve-Path (Join-Path $AppDir "..")).Path
$DistDir = Join-Path $RepoRoot "dist"

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$DisplayName,
        [Parameter(Mandatory = $true)][scriptblock]$Command
    )
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$DisplayName 失败（exit code $LASTEXITCODE）。"
    }
}

function Resolve-QtPrefix {
    param([string]$Prefix)
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($Prefix)) { $candidates += $Prefix }
    if (-not [string]::IsNullOrWhiteSpace($env:QTDIR)) { $candidates += $env:QTDIR }
    foreach ($candidate in $candidates) {
        $windeployqt = Join-Path $candidate "bin\windeployqt.exe"
        if (Test-Path $windeployqt) { return (Resolve-Path $candidate).Path }
    }
    $command = Get-Command "windeployqt.exe" -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return (Split-Path -Parent (Split-Path -Parent $command.Source))
    }
    throw "找不到 Qt（windeployqt.exe）。请用 -QtPrefix 指定 Qt 安装前缀，例如 -QtPrefix C:\Qt\6.8.0\msvc2022_64。"
}

function Resolve-Version {
    param([string]$Explicit)
    if (-not [string]::IsNullOrWhiteSpace($Explicit)) { return $Explicit }
    $cmakeLists = Join-Path $AppDir "CMakeLists.txt"
    if (Test-Path $cmakeLists) {
        $match = Select-String -Path $cmakeLists -Pattern 'project\s*\(\s*emby-pc\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)' -AllMatches -ErrorAction SilentlyContinue
        if ($null -ne $match -and $match.Matches.Count -gt 0) {
            return $match.Matches[0].Groups[1].Value
        }
    }
    return "0.1.0"
}

try {
    $QtPrefix = Resolve-QtPrefix -Prefix $QtPrefix
    $Version = Resolve-Version -Explicit $Version
    if ([string]::IsNullOrWhiteSpace($BuildDir)) { $BuildDir = Join-Path $AppDir "build-windows" }

    $libmpvDll = Join-Path $AppDir "libs\libmpv\libmpv-2.dll"
    if (-not (Test-Path $libmpvDll)) {
        throw "缺少 libmpv-2.dll：$libmpvDll。请先执行 scripts\fetch-libmpv.ps1 -Url <shinchiro mpv-dev x86_64 .7z 下载地址>。"
    }

    Write-Host "Emby PC Windows 打包"
    Write-Host "  AppDir    : $AppDir"
    Write-Host "  BuildDir  : $BuildDir"
    Write-Host "  QtPrefix  : $QtPrefix"
    Write-Host "  Version   : $Version"
    Write-Host "  Config    : $Configuration"

    if (-not $SkipBuild) {
        $cmakeCommand = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
        if ($null -eq $cmakeCommand) { throw "找不到 cmake.exe，请安装 CMake 3.21+ 并加入 PATH。" }

        $configureArgs = @(
            "-S", $AppDir,
            "-B", $BuildDir,
            "-DCMAKE_BUILD_TYPE=$Configuration",
            "-DCMAKE_PREFIX_PATH=$QtPrefix"
        )
        if (-not [string]::IsNullOrWhiteSpace($Generator)) {
            $configureArgs += @("-G", $Generator)
            if ($Generator -match "Visual Studio") { $configureArgs += @("-A", $Arch) }
        }

        Write-Step "CMake 配置"
        Invoke-Native -DisplayName "CMake 配置" -Command { & cmake @configureArgs }

        Write-Step "CMake 构建（$Configuration）"
        Invoke-Native -DisplayName "CMake 构建" -Command { & cmake --build $BuildDir --config $Configuration --parallel }
    } else {
        Write-Step "跳过构建（-SkipBuild）"
    }

    $exePath = $null
    $exeCandidates = @(
        (Join-Path $BuildDir "$Configuration\emby-pc.exe"),
        (Join-Path $BuildDir "emby-pc.exe")
    )
    foreach ($candidate in $exeCandidates) {
        if (Test-Path $candidate) { $exePath = (Resolve-Path $candidate).Path; break }
    }
    if ($null -eq $exePath) {
        $found = Get-ChildItem -Path $BuildDir -Filter "emby-pc.exe" -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $found) { $exePath = $found.FullName }
    }
    if ($null -eq $exePath) { throw "构建成功但找不到 emby-pc.exe（BuildDir=$BuildDir）。" }

    $stageName = "emby-pc-$Version-win64"
    $stageDir = Join-Path $DistDir $stageName
    if (Test-Path $stageDir) { Remove-Item -Path $stageDir -Recurse -Force }
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

    Write-Step "复制 emby-pc.exe 与 libmpv-2.dll"
    Copy-Item -Path $exePath -Destination (Join-Path $stageDir "emby-pc.exe") -Force
    Copy-Item -Path $libmpvDll -Destination (Join-Path $stageDir "libmpv-2.dll") -Force

    Write-Step "windeployqt 部署 Qt 运行时"
    $windeployqt = Join-Path $QtPrefix "bin\windeployqt.exe"
    $deployArgs = @(
        "--release",
        "--compiler-runtime",
        "--dir", $stageDir,
        (Join-Path $stageDir "emby-pc.exe")
    )
    Invoke-Native -DisplayName "windeployqt" -Command { & $windeployqt @deployArgs }

    $platformPlugin = Join-Path $stageDir "platforms\qwindows.dll"
    if (-not (Test-Path $platformPlugin)) {
        throw "windeployqt 未生成 platforms\qwindows.dll，Qt 运行时可能不完整。"
    }

    Write-Step "附带验收文档"
    $acceptance = Join-Path $RepoRoot "docs\windows-acceptance.md"
    if (Test-Path $acceptance) { Copy-Item -Path $acceptance -Destination (Join-Path $stageDir "WINDOWS-ACCEPTANCE.md") -Force }
    $libmpvDoc = Join-Path $RepoRoot "docs\libmpv-source.md"
    if (Test-Path $libmpvDoc) { Copy-Item -Path $libmpvDoc -Destination (Join-Path $stageDir "LIBMPV-SOURCE.md") -Force }

    $readme = @"
Emby PC $Version（Windows x64 免安装包）

运行：双击 emby-pc.exe。若提示缺少 DLL，请确认 libmpv-2.dll 与 emby-pc.exe 在同一目录。
日志：菜单「帮助 → 打开日志目录」，目录内含 app.log 与 mpv.log。
验收：见 WINDOWS-ACCEPTANCE.md。
libmpv 来源与许可：见 LIBMPV-SOURCE.md。
"@
    Set-Content -Path (Join-Path $stageDir "README-运行.txt") -Value $readme -Encoding UTF8

    Write-Host ""
    Write-Host "免安装目录已生成：$stageDir"
    if ($NoZip) {
        Write-Host "已指定 -NoZip，跳过压缩。" -ForegroundColor Yellow
    } else {
        Write-Step "压缩发布包"
        $zipPath = Join-Path $DistDir "$stageName.zip"
        if (Test-Path $zipPath) { Remove-Item -Path $zipPath -Force }
        Compress-Archive -Path $stageDir -DestinationPath $zipPath -CompressionLevel Optimal
        $sizeMb = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
        Write-Host ""
        Write-Host "打包完成：$zipPath（$sizeMb MB）" -ForegroundColor Green
    }
} catch {
    Write-Host ""
    Write-Host "打包失败：$($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
