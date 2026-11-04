#requires -Version 5.1
<#
.SYNOPSIS
    下载并安装 Windows x64 版 libmpv（shinchiro mpv-dev 构建）。

.DESCRIPTION
    流程：
      1. 按 -Url 下载 shinchiro mpv-dev x86_64 压缩包（.7z 或 .zip）；
      2. 计算并打印 SHA-256（传入 -Sha256 时强制校验，不一致立即失败）；
      3. 解压出 libmpv-2.dll 与 include\mpv 到 app\libs\libmpv\；
      4. 把本次获取记录（包名/URL/日期/SHA-256）写回 docs\libmpv-source.md 的
         fetch-libmpv 标记区，便于审计与复现。

    发布页：https://github.com/shinchiro/mpv-winbuild-cmake/releases
    常见文件名：mpv-dev-x86_64-<日期>-git-<commit>.7z

.PARAMETER Url
    mpv-dev x86_64 压缩包的下载地址（必填）；也支持传入已下载好的本地 .7z/.zip 文件路径。

.PARAMETER Destination
    安装目录，默认 app\libs\libmpv。

.PARAMETER Sha256
    期望的压缩包 SHA-256（可选）。传入后校验实际下载文件，不一致直接失败；
    CI 里必须传，防止上游 release 被替换或下载到错误文件。

.PARAMETER SkipDocUpdate
    不更新 docs\libmpv-source.md 的获取记录。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\fetch-libmpv.ps1 -Url "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261101/mpv-dev-x86_64-20261101-git-48fca90e8c.7z"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Url,
    [string]$Destination = "",
    [string]$Sha256 = "",
    [switch]$SkipDocUpdate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$AppDir = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$RepoRoot = (Resolve-Path (Join-Path $AppDir "..")).Path
if ([string]::IsNullOrWhiteSpace($Destination)) { $Destination = Join-Path $AppDir "libs\libmpv" }

function Write-Step([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Find-SevenZip {
    $command = Get-Command "7z.exe" -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    $command = Get-Command "7za.exe" -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    $bases = @($env:ProgramFiles)
    $pf86 = [Environment]::GetEnvironmentVariable("ProgramFiles(x86)")
    if (-not [string]::IsNullOrWhiteSpace($pf86)) { $bases += $pf86 }
    foreach ($base in $bases) {
        if (-not [string]::IsNullOrWhiteSpace($base)) {
            $candidate = Join-Path $base "7-Zip\7z.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    return $null
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("emby-pc-libmpv-" + [Guid]::NewGuid().ToString("N"))
try {
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

    $archiveName = ""
    $archivePath = ""
    if ($Url -match '^(https?|ftp)://') {
        $uri = [Uri]$Url
        $archiveName = [System.IO.Path]::GetFileName($uri.AbsolutePath)
        if ([string]::IsNullOrWhiteSpace($archiveName)) { $archiveName = "libmpv-download.bin" }
        $archivePath = Join-Path $tempDir $archiveName
        Write-Step "下载 $Url"
        Invoke-WebRequest -Uri $Url -OutFile $archivePath -UseBasicParsing
    } elseif (Test-Path -LiteralPath $Url) {
        # 支持本地文件：先手工下载好 .7z/.zip，再传给脚本（适合公司网络无法直连 GitHub 的情况）。
        $archiveName = [System.IO.Path]::GetFileName($Url)
        $archivePath = Join-Path $tempDir $archiveName
        Write-Step "使用本地文件 $Url"
        Copy-Item -LiteralPath $Url -Destination $archivePath -Force
    } else {
        throw "参数既不是 http(s) URL，也不是存在的本地文件：$Url"
    }
    if (-not (Test-Path $archivePath)) { throw "获取压缩包失败：$archivePath 不存在。" }
    $archiveSize = (Get-Item $archivePath).Length
    if ($archiveSize -le 0) { throw "下载失败：$archiveName 文件大小为 0。" }
    Write-Host ("  已下载：{0}（{1:N1} MB）" -f $archiveName, ($archiveSize / 1MB))

    Write-Step "计算 SHA-256"
    # 注意：PowerShell 变量名大小写不敏感，参数 $Sha256 与局部变量必须换名避免覆盖。
    $expectedSha256 = $(if ([string]::IsNullOrWhiteSpace($Sha256)) { "" } else { $Sha256.Trim().ToLowerInvariant() })
    $actualSha256 = (Get-FileHash -Path $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host "  SHA-256: $actualSha256"
    if (-not [string]::IsNullOrWhiteSpace($expectedSha256)) {
        if ($actualSha256 -ne $expectedSha256) {
            throw "SHA-256 校验失败：期望 $expectedSha256，实际 $actualSha256。请确认下载地址与版本是否匹配。"
        }
        Write-Host "  SHA-256 校验通过（与 -Sha256 一致）" -ForegroundColor Green
    }

    Write-Step "解压"
    $extractDir = Join-Path $tempDir "extract"
    New-Item -ItemType Directory -Path $extractDir -Force | Out-Null
    $extension = [System.IO.Path]::GetExtension($archiveName).ToLowerInvariant()
    if ($extension -eq ".zip") {
        Expand-Archive -Path $archivePath -DestinationPath $extractDir -Force
    } elseif ($extension -eq ".7z") {
        $sevenZip = Find-SevenZip
        if ($null -ne $sevenZip) {
            & $sevenZip "x" $archivePath ("-o" + $extractDir) "-y"
            if ($LASTEXITCODE -ne 0) { throw "7-Zip 解压失败（exit code $LASTEXITCODE）。" }
        } else {
            $tarCommand = Get-Command "tar.exe" -ErrorAction SilentlyContinue
            if ($null -eq $tarCommand) {
                throw "无法解压 .7z：未找到 7z.exe，也没有 Windows 自带的 tar.exe。请安装 7-Zip 后重试，或改用 .zip 包。"
            }
            & $tarCommand.Source -xf $archivePath -C $extractDir
            if ($LASTEXITCODE -ne 0) { throw "tar 解压 .7z 失败（exit code $LASTEXITCODE）。" }
        }
    } else {
        throw "不支持的压缩包格式：$extension（只支持 .7z / .zip）。"
    }

    Write-Step "定位 libmpv-2.dll 与 include\mpv"
    $dll = Get-ChildItem -Path $extractDir -Filter "libmpv-2.dll" -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $dll) { throw "压缩包内找不到 libmpv-2.dll，请确认下载的是 mpv-dev x86_64 包。" }

    $headerDir = $null
    $headers = Get-ChildItem -Path $extractDir -Filter "client.h" -Recurse -File -ErrorAction SilentlyContinue
    foreach ($header in $headers) {
        if ($header.Directory.Name -eq "mpv" -and (Test-Path (Join-Path $header.Directory.FullName "render.h"))) {
            $headerDir = $header.Directory.FullName
            break
        }
    }
    if ($null -eq $headerDir) { throw "压缩包内找不到 include\mpv\client.h（mpv 头文件）。" }

    Write-Step "安装到 $Destination"
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Copy-Item -Path $dll.FullName -Destination (Join-Path $Destination "libmpv-2.dll") -Force

    $destInclude = Join-Path $Destination "include\mpv"
    if (Test-Path $destInclude) { Remove-Item -Path $destInclude -Recurse -Force }
    New-Item -ItemType Directory -Path $destInclude -Force | Out-Null
    Copy-Item -Path (Join-Path $headerDir "*") -Destination $destInclude -Recurse -Force

    $packageName = [System.IO.Path]::GetFileNameWithoutExtension($archiveName)
    $fetchDate = Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz"

    if (-not $SkipDocUpdate) {
        $docPath = Join-Path $RepoRoot "docs\libmpv-source.md"
        if (-not (Test-Path $docPath)) {
            Write-Warning "未找到 $docPath，跳过获取记录更新。"
        } else {
            Write-Step "更新 docs\libmpv-source.md 获取记录"
            $recordLines = @(
                "<!-- fetch-libmpv:begin -->",
                "| 项 | 值 |",
                "|---|---|",
                "| 包文件名 | $archiveName |",
                "| 版本/构建 | $packageName |",
                "| 下载 URL | $Url |",
                "| 获取日期 | $fetchDate |",
                "| SHA-256 | $actualSha256 |",
                "| 安装路径 | $Destination |",
                "<!-- fetch-libmpv:end -->"
            )
            $lines = @(Get-Content -Path $docPath -Encoding UTF8)
            $beginIndex = [Array]::IndexOf($lines, "<!-- fetch-libmpv:begin -->")
            $endIndex = [Array]::IndexOf($lines, "<!-- fetch-libmpv:end -->")
            if ($beginIndex -ge 0 -and $endIndex -gt $beginIndex) {
                $newLines = New-Object System.Collections.Generic.List[string]
                if ($beginIndex -gt 0) { $newLines.AddRange([string[]]$lines[0..($beginIndex - 1)]) }
                $newLines.AddRange([string[]]$recordLines)
                if ($endIndex -lt ($lines.Count - 1)) { $newLines.AddRange([string[]]$lines[($endIndex + 1)..($lines.Count - 1)]) }
                Set-Content -Path $docPath -Value $newLines.ToArray() -Encoding UTF8
            } else {
                Set-Content -Path $docPath -Value ($recordLines + $lines) -Encoding UTF8
            }
            Write-Host "  已更新：$docPath"
        }
    }

    Write-Host ""
    Write-Host "libmpv 安装完成" -ForegroundColor Green
    Write-Host "  压缩包  : $archiveName"
    Write-Host "  URL     : $Url"
    Write-Host "  SHA-256 : $actualSha256"
    Write-Host "  目标目录: $Destination"
    Write-Host ("  DLL     : {0}" -f (Join-Path $Destination "libmpv-2.dll"))
    Write-Host ("  头文件  : {0}" -f (Join-Path $Destination "include\mpv"))
} catch {
    Write-Host ""
    Write-Host "获取 libmpv 失败：$($_.Exception.Message)" -ForegroundColor Red
    exit 1
} finally {
    if (Test-Path $tempDir) { Remove-Item -Path $tempDir -Recurse -Force -ErrorAction SilentlyContinue }
}
