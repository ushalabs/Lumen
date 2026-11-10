param(
    [string]$Version = "",
    [string]$OutputDir = "",
    [string]$NsisCompiler = "",
    [string]$QtBinDir = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if (-not $Version) {
    $cmakeContent = Get-Content -LiteralPath (Join-Path $projectRoot "CMakeLists.txt") -Raw
    $verMatch = [regex]::Match($cmakeContent, 'project\(Lumen[\s\S]*?VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
    if ($verMatch.Success) {
        $Version = $verMatch.Groups[1].Value
    } else {
        $Version = "0.5.0"
    }
}

if (-not $OutputDir) {
    $OutputDir = Join-Path $projectRoot "dist"
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path $OutputDir).Path

# Locate NSIS compiler
if (-not $NsisCompiler) {
    $nsisCandidates = @(
        "c:\Users\HP\Desktop\Projects\Downshift\.tools\windows\nsis\nsis-3.12\makensis.exe",
        (Join-Path ${env:ProgramFiles(x86)} "NSIS\makensis.exe"),
        (Join-Path $env:ProgramFiles "NSIS\makensis.exe")
    )
    $cmdNsis = Get-Command "makensis.exe" -ErrorAction SilentlyContinue
    if ($cmdNsis) {
        $nsisCandidates = @($cmdNsis.Source) + $nsisCandidates
    }
    foreach ($candidate in $nsisCandidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            $NsisCompiler = $candidate
            break
        }
    }
}
if (-not $NsisCompiler -or -not (Test-Path -LiteralPath $NsisCompiler -PathType Leaf)) {
    throw "NSIS compiler (makensis.exe) not found. Pass -NsisCompiler or install NSIS 3.x."
}

# Locate windeployqt
if ($QtBinDir) {
    $windeployqt = Join-Path $QtBinDir "windeployqt.exe"
} else {
    $qtCandidates = @(
        "C:\Qt\6.8.2\msvc2022_64\bin\windeployqt.exe"
    )
    $cmdWindeploy = Get-Command "windeployqt.exe" -ErrorAction SilentlyContinue
    if ($cmdWindeploy) {
        $qtCandidates = @($cmdWindeploy.Source) + $qtCandidates
    }
    $windeployqt = $qtCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
}
if (-not $windeployqt -or -not (Test-Path -LiteralPath $windeployqt -PathType Leaf)) {
    throw "windeployqt.exe not found. Pass -QtBinDir or ensure Qt bin is on PATH."
}

# Locate MSVC x64 CRT redistributables so app runs on clean Windows 10/11 PCs without requiring separate VC++ Redistributable installation
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe not found at $vswhere; cannot locate Microsoft.VC143.CRT DLLs."
}
$vsInstallPath = & $vswhere -latest -version "[17.0,18.0)" -products "*" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstallPath) {
    throw "Visual Studio 2022 VC++ tools installation not found via vswhere."
}
$redistRoot = Get-ChildItem -LiteralPath (Join-Path $vsInstallPath.Trim() "VC\Redist\MSVC") -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $redistRoot) {
    throw "VC++ Redistributable version folder not found under $($vsInstallPath.Trim())\VC\Redist\MSVC."
}
$crtDir = Join-Path $redistRoot.FullName "x64\Microsoft.VC143.CRT"
if (-not (Test-Path -LiteralPath $crtDir -PathType Container)) {
    throw "Microsoft.VC143.CRT directory not found at $crtDir."
}

$lumenExe = Join-Path $projectRoot "build\Lumen.exe"
if (-not (Test-Path -LiteralPath $lumenExe -PathType Leaf)) {
    throw "Lumen.exe not found at $lumenExe. Build Release first."
}

# 1. Prepare Staging Area
$stageDir = Join-Path $OutputDir "stage"
if (Test-Path -LiteralPath $stageDir) {
    Remove-Item -Recurse -Force -LiteralPath $stageDir
}
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

Write-Host "==> Staging Lumen binaries into $stageDir..."
Copy-Item -LiteralPath $lumenExe -Destination (Join-Path $stageDir "Lumen.exe") -Force

# 2. Run windeployqt (with VCINSTALLDIR set so windeployqt sees MSVC context cleanly)
Write-Host "==> Deploying Qt 6 runtime..."
$env:PATH = "$(Split-Path $windeployqt -Parent);$env:PATH"
& $windeployqt --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-network (Join-Path $stageDir "Lumen.exe")
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

# 3. Bundle app-local Microsoft VC++ 14.3 x64 CRT DLLs for standalone Windows 10/11 compatibility
Write-Host "==> Bundling x64 Microsoft VC++ CRT runtime from $crtDir..."
$crtDlls = Get-ChildItem -LiteralPath $crtDir -Filter "*.dll" -File
if ($crtDlls.Count -eq 0) {
    throw "No CRT DLLs found in $crtDir"
}
foreach ($dll in $crtDlls) {
    Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $stageDir $dll.Name) -Force
}

foreach ($requiredDll in @("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll")) {
    if (-not (Test-Path -LiteralPath (Join-Path $stageDir $requiredDll) -PathType Leaf)) {
        throw "Required VC++ CRT DLL missing from staging area: $requiredDll"
    }
}

# 4. Copy ONNX Runtime & Bundled Model Assets
Write-Host "==> Bundling ONNX Runtime & MiniLM-L6-v2 embeddings model..."
$onnxDll = Join-Path $projectRoot "third_party\onnxruntime\lib\onnxruntime.dll"
Copy-Item -LiteralPath $onnxDll -Destination (Join-Path $stageDir "onnxruntime.dll") -Force

$modelTargetDir = Join-Path $stageDir "models\all-MiniLM-L6-v2"
New-Item -ItemType Directory -Force -Path $modelTargetDir | Out-Null
Copy-Item (Join-Path $projectRoot "assets\models\all-MiniLM-L6-v2\*") $modelTargetDir -Force

Copy-Item -LiteralPath (Join-Path $projectRoot "LICENSE") -Destination (Join-Path $stageDir "LICENSE") -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "README.md") -Destination (Join-Path $stageDir "README.md") -Force

# 5. Generate NSIS File Lists
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

# 6. Compile Windows Setup Installer via NSIS
$setupExe = Join-Path $OutputDir "Lumen-v$Version-Setup-x64.exe"
if (Test-Path -LiteralPath $setupExe) {
    Remove-Item -LiteralPath $setupExe -Force
}
Write-Host "==> Compiling NSIS Windows installer ($setupExe)..."
$nsiScript = Join-Path $projectRoot "packaging\Lumen.nsi"

& $NsisCompiler `
    "/V2" `
    "/DVERSION=$Version" `
    "/DPROJECT_ROOT=$projectRoot" `
    "/DSETUP_OUTPUT=$setupExe" `
    "/DINSTALL_FILES=$installList" `
    "/DUNINSTALL_FILES=$uninstallList" `
    $nsiScript

if ($LASTEXITCODE -ne 0) {
    throw "NSIS compilation failed with code $LASTEXITCODE"
}

# 7. Create Portable ZIP Distribution
$zipArchive = Join-Path $OutputDir "Lumen-v$Version-windows-x64.zip"
Write-Host "==> Creating portable ZIP bundle ($zipArchive)..."
if (Test-Path -LiteralPath $zipArchive) {
    Remove-Item -LiteralPath $zipArchive -Force
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stageDir, $zipArchive, [System.IO.Compression.CompressionLevel]::Optimal, $false)

# 8. Generate Clean, Deduplicated SHA256SUMS.txt
$shaFile = Join-Path $OutputDir "SHA256SUMS.txt"
$shaLines = Get-ChildItem -LiteralPath $OutputDir -File |
    Where-Object { $_.Name -match '^Lumen-.*\.(exe|zip|msix)$' } |
    Sort-Object Name |
    ForEach-Object {
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower()
        "$hash  $($_.Name)"
    }
Set-Content -LiteralPath $shaFile -Value $shaLines -Encoding ascii

# Clean temporary nsh lists
Remove-Item $installList -Force -ErrorAction SilentlyContinue
Remove-Item $uninstallList -Force -ErrorAction SilentlyContinue

$setupSizeMb = [math]::Round(((Get-Item -LiteralPath $setupExe).Length / 1MB), 2)
$zipSizeMb = [math]::Round(((Get-Item -LiteralPath $zipArchive).Length / 1MB), 2)

Write-Host "=========================================================="
Write-Host "  Lumen Windows NSIS Installer Built Successfully!"
Write-Host "  Developer:  UshaLabs"
Write-Host "  Version:    $Version (x64, Windows 10 & Windows 11)"
Write-Host "  Installer:  $setupExe ($setupSizeMb MB)"
Write-Host "  Portable:   $zipArchive ($zipSizeMb MB)"
Write-Host "  SHA256:     $shaFile"
Write-Host "=========================================================="
