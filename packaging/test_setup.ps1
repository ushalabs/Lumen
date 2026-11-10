param(
    [string]$SetupPath = "",
    [string]$StageDir = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if (-not $SetupPath) {
    $SetupPath = Get-ChildItem -LiteralPath (Join-Path $projectRoot "dist") -Filter "Lumen-*-Setup-x64.exe" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $SetupPath -or -not (Test-Path -LiteralPath $SetupPath -PathType Leaf)) {
    throw "NSIS installer executable not found. Run packaging\build_setup.ps1 first."
}
$SetupPath = (Resolve-Path -LiteralPath $SetupPath).Path

if (-not $StageDir) {
    $StageDir = Join-Path $projectRoot "dist\stage"
}
if (-not (Test-Path -LiteralPath $StageDir -PathType Container)) {
    throw "Stage directory not found at $StageDir."
}

$mtExe = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\mt.exe"
if (-not (Test-Path -LiteralPath $mtExe -PathType Leaf)) {
    throw "mt.exe not found at $mtExe"
}

Write-Host "==> Checking installer FileVersionInfo..."
$ver = [Diagnostics.FileVersionInfo]::GetVersionInfo($SetupPath)
if ($ver.CompanyName -ne "UshaLabs") { throw "Unexpected CompanyName: '$($ver.CompanyName)'" }
if ($ver.ProductName -ne "Lumen") { throw "Unexpected ProductName: '$($ver.ProductName)'" }
if ($ver.FileDescription -match '\?') { throw "Encoding issue detected in FileDescription: '$($ver.FileDescription)'" }
Write-Host "    CompanyName=$($ver.CompanyName), ProductName=$($ver.ProductName), FileVersion=$($ver.FileVersion), ProductVersion=$($ver.ProductVersion)"

Write-Host "==> Checking installer embedded manifest (Win10/11 + DPI + asInvoker)..."
$tempManifest = Join-Path $env:TEMP ("lumen_setup_manifest_" + [guid]::NewGuid().ToString("N") + ".xml")
& $mtExe "-inputresource:$SetupPath;#1" "-out:$tempManifest" | Out-Null
$manifestXml = Get-Content -LiteralPath $tempManifest -Raw
Remove-Item -LiteralPath $tempManifest -Force -ErrorAction SilentlyContinue
if ($manifestXml -notmatch 'level="asInvoker"') { throw "Installer missing asInvoker execution level." }
if ($manifestXml -notmatch '8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a') { throw "Installer manifest missing Windows 10/11 supportedOS GUID." }
if ($manifestXml -notmatch 'PerMonitorV2') { throw "Installer manifest missing PerMonitorV2 DPI awareness." }

$testRegPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Lumen.PackagingTest"
if (Test-Path -LiteralPath $testRegPath) {
    throw "Existing Lumen.PackagingTest registry key found; refusing to overwrite."
}

$testRoot = Join-Path $env:TEMP ("lumen-nsis-test-" + [guid]::NewGuid().ToString("N"))
$installDir = Join-Path $testRoot "Lumen"

$pathBefore = $env:PATH
$qtPluginBefore = $env:QT_PLUGIN_PATH
$qtPlatformBefore = $env:QT_QPA_PLATFORM

try {
    Write-Host "==> Running silent isolated NSIS install to $installDir..."
    $proc = Start-Process -FilePath $SetupPath -ArgumentList "/S /ISOLATEDTEST /D=$installDir" -PassThru -Wait
    if ($proc.ExitCode -ne 0) {
        throw "Installer failed with exit code $($proc.ExitCode)"
    }

    Write-Host "==> Verifying installed payload matches staged binaries..."
    $stagedFiles = Get-ChildItem -LiteralPath $StageDir -File -Recurse
    foreach ($f in $stagedFiles) {
        $rel = $f.FullName.Substring($StageDir.Length + 1)
        $destFile = Join-Path $installDir $rel
        if (-not (Test-Path -LiteralPath $destFile -PathType Leaf)) {
            throw "Installed file missing: $rel"
        }
        $srcHash = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
        $dstHash = (Get-FileHash -LiteralPath $destFile -Algorithm SHA256).Hash
        if ($srcHash -ne $dstHash) {
            throw "Hash mismatch for installed file: $rel"
        }
    }

    foreach ($crt in @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll")) {
        if (-not (Test-Path -LiteralPath (Join-Path $installDir $crt) -PathType Leaf)) {
            throw "Installed folder missing VC++ CRT DLL: $crt"
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $installDir "Uninstall.exe") -PathType Leaf)) {
        throw "Uninstall.exe missing from installed folder."
    }

    Write-Host "==> Verifying 64-bit Add/Remove Programs registration..."
    $regItem = Get-ItemProperty -LiteralPath $testRegPath
    if ($regItem.Publisher -ne "UshaLabs") { throw "Uninstall Publisher mismatch: '$($regItem.Publisher)'" }
    if ($regItem.DisplayName -ne "Lumen") { throw "Uninstall DisplayName mismatch: '$($regItem.DisplayName)'" }
    if ($regItem.InstallLocation -ne $installDir) { throw "Uninstall InstallLocation mismatch: '$($regItem.InstallLocation)'" }

    Write-Host "==> Verifying Desktop & Start Menu test shortcuts..."
    $wsh = New-Object -ComObject WScript.Shell
    foreach ($lnkPath in @((Join-Path $installDir "Desktop\Lumen.lnk"), (Join-Path $installDir "StartMenu\Lumen.lnk"))) {
        if (-not (Test-Path -LiteralPath $lnkPath -PathType Leaf)) {
            throw "Missing shortcut: $lnkPath"
        }
        $sc = $wsh.CreateShortcut($lnkPath)
        if ($sc.TargetPath -ne (Join-Path $installDir "Lumen.exe")) {
            throw "Shortcut target mismatch ($lnkPath): $($sc.TargetPath)"
        }
        if ($sc.WorkingDirectory -ne $installDir) {
            throw "Shortcut WorkingDirectory mismatch ($lnkPath): $($sc.WorkingDirectory)"
        }
    }

    Write-Host "==> Testing installed Lumen.exe startup with sanitized system-only PATH..."
    $env:PATH = "$env:WINDIR\System32;$env:WINDIR"
    $env:QT_PLUGIN_PATH = ""
    $env:QT_QPA_PLATFORM = ""
    $startupReport = Join-Path $testRoot "startup_report.txt"
    $appProc = Start-Process -FilePath (Join-Path $installDir "Lumen.exe") -ArgumentList "--measure-startup `"$startupReport`"" -WorkingDirectory $installDir -PassThru
    if (-not $appProc.WaitForExit(15000)) {
        $appProc.Kill()
        throw "Installed Lumen.exe --measure-startup timed out."
    }
    if ($appProc.ExitCode -ne 0) {
        throw "Installed Lumen.exe exited with code $($appProc.ExitCode)."
    }
    if (-not (Test-Path -LiteralPath $startupReport -PathType Leaf)) {
        throw "Startup verification report was not written."
    }
    $reportText = Get-Content -LiteralPath $startupReport -Raw
    Write-Host $reportText.Trim()
    if ($reportText -notmatch 'desktop_bar_visible=1') {
        throw "Desktop bar did not become visible during isolated launch test."
    }

    Write-Host "==> Running silent uninstaller..."
    $uninstProc = Start-Process -FilePath (Join-Path $installDir "Uninstall.exe") -ArgumentList "/S _?=$installDir" -PassThru -Wait
    if ($uninstProc.ExitCode -ne 0) {
        throw "Uninstaller failed with exit code $($uninstProc.ExitCode)"
    }

    if (Test-Path -LiteralPath $testRegPath) {
        throw "Uninstall failed to clean up test registry key: $testRegPath"
    }
    if (Test-Path -LiteralPath (Join-Path $installDir "Lumen.exe")) {
        throw "Uninstall failed to delete Lumen.exe"
    }
    if (Test-Path -LiteralPath (Join-Path $installDir "vcruntime140.dll")) {
        throw "Uninstall failed to delete VC++ CRT DLL"
    }

    Write-Host ""
    Write-Host "==========================================================" -ForegroundColor Green
    Write-Host "  [+] NSIS Installer Verification PASSED!" -ForegroundColor Green
    Write-Host "  Setup:      $SetupPath" -ForegroundColor White
    Write-Host "  Publisher:  UshaLabs" -ForegroundColor White
    Write-Host "==========================================================" -ForegroundColor Green
} finally {
    $env:PATH = $pathBefore
    $env:QT_PLUGIN_PATH = $qtPluginBefore
    $env:QT_QPA_PLATFORM = $qtPlatformBefore
    if (Test-Path -LiteralPath $testRegPath) {
        Remove-Item -LiteralPath $testRegPath -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
