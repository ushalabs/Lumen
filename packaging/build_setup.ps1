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

$nsisCompiler = "c:\Users\HP\Desktop\Projects\Downshift\.tools\windows\nsis\nsis-3.12\makensis.exe"
if (-not (Test-Path $nsisCompiler)) {
    throw "NSIS compiler not found at $nsisCompiler"
}

$windeployqt = "C:\Qt\6.8.2\msvc2022_64\bin\windeployqt.exe"
if (-not (Test-Path $windeployqt)) {
    throw "windeployqt not found at $windeployqt"
}

$lumenExe = Join-Path $projectRoot "build\Lumen.exe"
if (-not (Test-Path $lumenExe)) {
    throw "Lumen.exe not found at $lumenExe. Build Release first."
}

# 1. Prepare Staging Area
$stageDir = Join-Path $OutputDir "stage"
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

# 4. Generate NSIS File Lists
Write-Host "==> Generating installer file manifests..."
$installList = Join-Path $OutputDir "install-files.nsh"
$uninstallList = Join-Path $OutputDir "uninstall-files.nsh"

$installLines = [System.Collections.Generic.List[string]]::new()
$uninstallLines = [System.Collections.Generic.List[string]]::new()

function Quote-Nsis([string]$val) {
    return $val.Replace("$", "$$").Replace('"', '$\"')
}

$files = @(Get-ChildItem -LiteralPath $stageDir -File -Recurse)
foreach ($f in $files) {
    $rel = $f.FullName.Substring($stageDir.Length + 1)
    $parent = Split-Path $rel -Parent
    if ($parent) {
        $installLines.Add('SetOutPath "$INSTDIR\' + (Quote-Nsis $parent) + '"')
    } else {
        $installLines.Add('SetOutPath "$INSTDIR"')
    }
    $installLines.Add('File "' + (Quote-Nsis $f.FullName) + '"')
    $uninstallLines.Add('Delete "$INSTDIR\' + (Quote-Nsis $rel) + '"')
}

foreach ($d in (Get-ChildItem -LiteralPath $stageDir -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending)) {
    $rel = $d.FullName.Substring($stageDir.Length + 1)
    $uninstallLines.Add('RMDir "$INSTDIR\' + (Quote-Nsis $rel) + '"')
}

[System.IO.File]::WriteAllLines($installList, $installLines, [System.Text.UTF8Encoding]::new($true))
[System.IO.File]::WriteAllLines($uninstallList, $uninstallLines, [System.Text.UTF8Encoding]::new($true))

# 5. Compile Windows Setup Installer via NSIS
$setupExe = Join-Path $OutputDir "Lumen-v$Version-Setup-x64.exe"
Write-Host "==> Compiling NSIS Windows installer ($setupExe)..."
$nsiScript = Join-Path $projectRoot "packaging\Lumen.nsi"

& $nsisCompiler `
    "/DVERSION=$Version" `
    "/DPROJECT_ROOT=$projectRoot" `
    "/DSETUP_OUTPUT=$setupExe" `
    "/DINSTALL_FILES=$installList" `
    "/DUNINSTALL_FILES=$uninstallList" `
    $nsiScript

if ($LASTEXITCODE -ne 0) {
    throw "NSIS compilation failed with code $LASTEXITCODE"
}

# 6. Create Portable ZIP Distribution
$zipArchive = Join-Path $OutputDir "Lumen-v$Version-windows-x64.zip"
Write-Host "==> Creating portable ZIP bundle ($zipArchive)..."
if (Test-Path $zipArchive) {
    Remove-Item -Force $zipArchive
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stageDir, $zipArchive, [System.IO.Compression.CompressionLevel]::Optimal, $false)

# 7. Generate Checksums
$hashSetup = (Get-FileHash -LiteralPath $setupExe -Algorithm SHA256).Hash.ToLower()
$hashZip = (Get-FileHash -LiteralPath $zipArchive -Algorithm SHA256).Hash.ToLower()
$shaFile = Join-Path $OutputDir "SHA256SUMS.txt"
@(
    "$hashSetup  Lumen-v$Version-Setup-x64.exe",
    "$hashZip  Lumen-v$Version-windows-x64.zip"
) | Set-Content -LiteralPath $shaFile -Encoding ascii

# Clean temporary nsh lists
Remove-Item $installList -Force -ErrorAction SilentlyContinue
Remove-Item $uninstallList -Force -ErrorAction SilentlyContinue

$setupSizeMb = [math]::Round(((Get-Item $setupExe).Length / 1MB), 2)
$zipSizeMb = [math]::Round(((Get-Item $zipArchive).Length / 1MB), 2)

Write-Host "=========================================================="
Write-Host "  Lumen Windows Setup Package Built Successfully!"
Write-Host "  Developer:  UshaLabs"
Write-Host "  Installer:  $setupExe ($setupSizeMb MB)"
Write-Host "  Portable:   $zipArchive ($zipSizeMb MB)"
Write-Host "  SHA256:     $shaFile"
Write-Host "=========================================================="
