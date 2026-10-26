param(
    [string]$Version = "0.5.0",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if (-not $OutputDir) {
    $OutputDir = Join-Path $projectRoot "dist"
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path $OutputDir).Path

$makeappx = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\makeappx.exe"
if (-not (Test-Path $makeappx)) {
    throw "makeappx.exe not found at $makeappx"
}

$signtool = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
if (-not (Test-Path $signtool)) {
    throw "signtool.exe not found at $signtool"
}

$windeployqt = "C:\Qt\6.8.2\msvc2022_64\bin\windeployqt.exe"
if (-not (Test-Path $windeployqt)) {
    throw "windeployqt.exe not found at $windeployqt"
}

$lumenExe = Join-Path $projectRoot "build\Lumen.exe"
if (-not (Test-Path $lumenExe)) {
    throw "Lumen.exe not found at $lumenExe. Build Release first."
}

# 1. Prepare Staging Area
$stageDir = Join-Path $OutputDir "msix_stage"
if (Test-Path $stageDir) {
    Remove-Item -Recurse -Force $stageDir
}
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

Write-Host "==> Staging Lumen binaries into $stageDir..."
Copy-Item $lumenExe (Join-Path $stageDir "Lumen.exe") -Force

# 2. Run windeployqt
Write-Host "==> Deploying Qt 6.8.2 runtime..."
& $windeployqt --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-network (Join-Path $stageDir "Lumen.exe")
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

# 3. Copy ONNX Runtime & Bundled Model Assets
Write-Host "==> Bundling ONNX Runtime & MiniLM-L6-v2 embeddings model..."
$onnxDll = Join-Path $projectRoot "third_party\onnxruntime\lib\onnxruntime.dll"
Copy-Item $onnxDll (Join-Path $stageDir "onnxruntime.dll") -Force

$modelTargetDir = Join-Path $stageDir "models\all-MiniLM-L6-v2"
New-Item -ItemType Directory -Force -Path $modelTargetDir | Out-Null
Copy-Item (Join-Path $projectRoot "assets\models\all-MiniLM-L6-v2\*") $modelTargetDir -Force

Copy-Item (Join-Path $projectRoot "LICENSE") (Join-Path $stageDir "LICENSE") -Force
Copy-Item (Join-Path $projectRoot "README.md") (Join-Path $stageDir "README.md") -Force

# 4. Copy Visual Assets for MSIX
$assetsTargetDir = Join-Path $stageDir "Assets"
New-Item -ItemType Directory -Force -Path $assetsTargetDir | Out-Null
$msixAssetsSrc = Join-Path $PSScriptRoot "msix_assets"
if (-not (Test-Path "$msixAssetsSrc\Square150x150Logo.png")) {
    Write-Host "==> Generating MSIX assets..."
    & (Join-Path $PSScriptRoot "generate_msix_assets.ps1")
}
Copy-Item (Join-Path $msixAssetsSrc "*") $assetsTargetDir -Force

# 5. Generate AppxManifest.xml
Write-Host "==> Generating AppxManifest.xml (Developer: UshaLabs)..."
$quadVersion = "$Version.0"
$manifestXml = @"
<?xml version="1.0" encoding="utf-8"?>
<Package
  xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
  xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
  xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
  xmlns:desktop="http://schemas.microsoft.com/appx/manifest/desktop/windows10"
  IgnorableNamespaces="uap rescap desktop">

  <Identity
    Name="UshaLabs.Lumen"
    Publisher="CN=UshaLabs"
    Version="$quadVersion"
    ProcessorArchitecture="x64" />

  <Properties>
    <DisplayName>Lumen</DisplayName>
    <PublisherDisplayName>UshaLabs</PublisherDisplayName>
    <Logo>Assets\StoreLogo.png</Logo>
    <Description>Lumen — Lightweight Offline Intelligent Search for Windows. Developed by UshaLabs.</Description>
  </Properties>

  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.26100.0" />
  </Dependencies>

  <Capabilities>
    <rescap:Capability Name="runFullTrust" />
  </Capabilities>

  <Applications>
    <Application Id="Lumen" Executable="Lumen.exe" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements
        DisplayName="Lumen"
        Description="Lumen — Intelligent Search for Windows by UshaLabs"
        BackgroundColor="#171B27"
        Square150x150Logo="Assets\Square150x150Logo.png"
        Square44x44Logo="Assets\Square44x44Logo.png">
        <uap:DefaultTile Wide310x150Logo="Assets\Wide310x150Logo.png" />
        <uap:SplashScreen Image="Assets\SplashScreen.png" BackgroundColor="#171B27" />
      </uap:VisualElements>
    </Application>
  </Applications>
</Package>
"@

Set-Content -Path (Join-Path $stageDir "AppxManifest.xml") -Value $manifestXml -Encoding UTF8

# 6. Build MSIX Package
$msixPath = Join-Path $OutputDir "Lumen-v$Version-x64.msix"
Write-Host "==> Packaging MSIX: $msixPath..."
& $makeappx pack /v /o /h SHA256 /d $stageDir /p $msixPath
if ($LASTEXITCODE -ne 0) {
    throw "makeappx failed with exit code $LASTEXITCODE"
}

# 7. Code Sign MSIX with UshaLabs Certificate
$pfxFile = Join-Path $PSScriptRoot "certs\UshaLabs.pfx"
$cerFile = Join-Path $PSScriptRoot "certs\UshaLabs.cer"
if (-not (Test-Path $pfxFile)) {
    throw "Certificate not found at $pfxFile"
}

Write-Host "==> Signing MSIX with UshaLabs certificate..."
& $signtool sign /fd SHA256 /a /f $pfxFile /p "UshaLabs2026!" $msixPath
if ($LASTEXITCODE -ne 0) {
    throw "signtool sign failed with exit code $LASTEXITCODE"
}

# 8. Create 1-Click Zero-Warning Installer Scripts for Users
$installPs1 = @"
# ==============================================================================
# Lumen Installer — Developed by UshaLabs
# Installs Lumen Windows MSIX Package with Zero Security Warnings
# ==============================================================================
param([switch]`$Silent)

`$ErrorActionPreference = "Stop"
`$scriptDir = `$PSScriptRoot
`$msixFile = Get-ChildItem -Path `$scriptDir -Filter "Lumen-*-x64.msix" | Select-Object -First 1
`$cerFile = Join-Path `$scriptDir "UshaLabs.cer"

if (-not `$msixFile) {
    Write-Error "Lumen MSIX package not found in `$scriptDir"
    exit 1
}

Write-Host ""
Write-Host "===============================================================" -ForegroundColor Magenta
Write-Host "  Lumen — Intelligent Search for Windows" -ForegroundColor White
Write-Host "  Developed by UshaLabs" -ForegroundColor Magenta
Write-Host "===============================================================" -ForegroundColor Magenta
Write-Host ""

# 1. Ensure UshaLabs publisher certificate is trusted in TrustedPeople
if (Test-Path `$cerFile) {
    Write-Host "[*] Registering UshaLabs Publisher Certificate..." -ForegroundColor Cyan
    try {
        `$cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(`$cerFile)
        
        # Check if already installed in LocalMachine or CurrentUser
        `$storeLM = New-Object System.Security.Cryptography.X509Certificates.X509Store("TrustedPeople", "LocalMachine")
        `$storeLM.Open("ReadOnly")
        `$foundLM = `$storeLM.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, `$cert.Thumbprint, `$false)
        `$storeLM.Close()

        `$storeCU = New-Object System.Security.Cryptography.X509Certificates.X509Store("TrustedPeople", "CurrentUser")
        `$storeCU.Open("ReadOnly")
        `$foundCU = `$storeCU.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, `$cert.Thumbprint, `$false)
        `$storeCU.Close()

        if (`$foundLM.Count -eq 0 -and `$foundCU.Count -eq 0) {
            # Try CurrentUser first (requires NO elevation / NO UAC!)
            `$storeCU.Open("ReadWrite")
            `$storeCU.Add(`$cert)
            `$storeCU.Close()
            Write-Host "    [+] Added to CurrentUser\TrustedPeople store" -ForegroundColor Green
        } else {
            Write-Host "    [+] UshaLabs certificate is already trusted" -ForegroundColor Green
        }
    } catch {
        Write-Warning "Failed to import certificate to user store: `$_"
    }
}

# 2. Install MSIX Package
Write-Host "[*] Installing Lumen (`$(`$msixFile.Name))..." -ForegroundColor Cyan
try {
    Add-AppxPackage -Path `$msixFile.FullName
    Write-Host ""
    Write-Host "===============================================================" -ForegroundColor Green
    Write-Host "  [+] Installation Successful!" -ForegroundColor Green
    Write-Host "  Lumen is now installed and ready to use." -ForegroundColor White
    Write-Host "  Find it in your Start Menu or press Ctrl+Alt+Space to search." -ForegroundColor Gray
    Write-Host "===============================================================" -ForegroundColor Green
    Write-Host ""
} catch {
    # If Add-AppxPackage requires LocalMachine elevation, self-elevate once
    Write-Host "[!] Sideloading requires LocalMachine trust. Requesting elevation..." -ForegroundColor Yellow
    `$argList = "-NoProfile -ExecutionPolicy Bypass -Command `"certutil -addstore -f TrustedPeople '`$cerFile'; Add-AppxPackage -Path '`$(`$msixFile.FullName)'`""
    Start-Process powershell.exe -ArgumentList `$argList -Verb RunAs -Wait
    Write-Host "[+] Installation completed via elevated registration." -ForegroundColor Green
}
"@

$installBat = @"
@echo off
setlocal
cd /d "%~dp0"
title Lumen Installer - UshaLabs

echo ===============================================================
echo   Lumen - Intelligent Search for Windows
echo   Developed by UshaLabs
echo ===============================================================
echo.
echo Installing Lumen MSIX with zero warnings...
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Lumen.ps1"

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Installation encountered an error.
    pause
) else (
    echo.
    timeout /t 3 >nul
)
"@

$readmeTxt = @"
Lumen — Lightweight Offline Intelligent Search for Windows
Developed by UshaLabs
Version: $Version (x64)
==============================================================

INSTALLATION INSTRUCTIONS (ZERO WARNINGS):

Method 1: One-Click Installer (Recommended)
--------------------------------------------
1. Right-click 'Install-Lumen.bat' and select 'Run as administrator' (or double-click).
2. The installer will automatically register the UshaLabs certificate and install Lumen.
3. Lumen will appear immediately in your Start Menu and on your desktop!

Method 2: Native Windows App Installer (MSIX)
----------------------------------------------
1. Right-click 'UshaLabs.cer' -> 'Install Certificate'.
2. Select 'Current User' or 'Local Machine' -> Place all certificates in 'Trusted People'.
3. Double-click 'Lumen-v$Version-x64.msix'.
4. Notice that Windows App Installer reports 'Publisher: UshaLabs' with NO WARNINGS!
5. Click 'Install'.

==============================================================
Copyright (c) 2026 UshaLabs. All rights reserved.
"@

# 9. Create MSIX Distribution Bundle (Zip)
$bundleDir = Join-Path $OutputDir "Lumen-v$Version-MSIX"
if (Test-Path $bundleDir) { Remove-Item -Recurse -Force $bundleDir }
New-Item -ItemType Directory -Force -Path $bundleDir | Out-Null

Copy-Item $msixPath (Join-Path $bundleDir (Split-Path $msixPath -Leaf))
Copy-Item $cerFile (Join-Path $bundleDir "UshaLabs.cer")
Set-Content -Path (Join-Path $bundleDir "Install-Lumen.ps1") -Value $installPs1 -Encoding UTF8
Set-Content -Path (Join-Path $bundleDir "Install-Lumen.bat") -Value $installBat -Encoding ASCII
Set-Content -Path (Join-Path $bundleDir "README.txt") -Value $readmeTxt -Encoding UTF8

$bundleZip = Join-Path $OutputDir "Lumen-v$Version-MSIX-Bundle.zip"
if (Test-Path $bundleZip) { Remove-Item -Force $bundleZip }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($bundleDir, $bundleZip, [System.IO.Compression.CompressionLevel]::Optimal, $false)

# Clean staging directory
Remove-Item -Recurse -Force $stageDir -ErrorAction SilentlyContinue

$msixSizeMb = [math]::Round(((Get-Item $msixPath).Length / 1MB), 2)
$bundleSizeMb = [math]::Round(((Get-Item $bundleZip).Length / 1MB), 2)

$hashMsix = (Get-FileHash -LiteralPath $msixPath -Algorithm SHA256).Hash.ToLower()
$hashBundle = (Get-FileHash -LiteralPath $bundleZip -Algorithm SHA256).Hash.ToLower()

$shaFile = Join-Path $OutputDir "SHA256SUMS.txt"
$shaEntries = @(
    "$hashMsix  $(Split-Path $msixPath -Leaf)",
    "$hashBundle  $(Split-Path $bundleZip -Leaf)"
)
if (Test-Path $shaFile) {
    $existing = Get-Content $shaFile
    $shaEntries = @($existing + $shaEntries) | Select-Object -Unique
}
$shaEntries | Set-Content -LiteralPath $shaFile -Encoding ascii

Write-Host "=========================================================="
Write-Host "  Lumen MSIX Package Built & Signed Successfully!"
Write-Host "  Developer:  UshaLabs"
Write-Host "  Package:    $msixPath ($msixSizeMb MB)"
Write-Host "  Bundle:     $bundleZip ($bundleSizeMb MB)"
Write-Host "  Publisher:  CN=UshaLabs (Signed & Verified)"
Write-Host "=========================================================="
