<#
  Builds htixtp.sys with the WDK from NuGet, then makes a test-signed driver
  package (sys + inf + cat) and the user-facing TouchpadFix folder.
  Runs on GitHub's windows-2025-vs2026 image (Visual Studio with the WDK component).
#>
param(
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$logs = Join-Path $root 'out\logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null

# --- 1. Visual Studio developer shell (the install that carries the WDK component) ---
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = $null
foreach ($component in @('Microsoft.Windows.DriverKit', 'Component.Microsoft.Windows.DriverKit.BuildTools')) {
    $vsPath = & $vswhere -latest -products * -requires $component -property installationPath
    if ($vsPath) { break }
}
if (-not $vsPath) {
    $vsPath = & $vswhere -latest -products * -property installationPath
    Write-Warning "No Visual Studio with the WDK component found; using $vsPath"
}
Write-Host "Visual Studio: $vsPath"
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
Set-Location $root

# --- 2. Build ---
msbuild driver\htixtp.vcxproj -t:Rebuild "-p:Configuration=$Configuration" -p:Platform=x64 -p:TargetVersion=Windows10 `
    -nologo -clp:Verbosity=minimal `
    "-flp:LogFile=$logs\build.log;Verbosity=normal" `
    "-flp1:LogFile=$logs\errors.log;ErrorsOnly" `
    "-flp2:LogFile=$logs\warnings.log;WarningsOnly"
if ($LASTEXITCODE -ne 0) {
    Get-Content "$logs\errors.log" -ErrorAction SilentlyContinue | Select-Object -First 60 | ForEach-Object { Write-Host $_ }
    throw "msbuild failed with exit code $LASTEXITCODE"
}
Write-Host '--- warnings ---'
Get-Content "$logs\warnings.log" -ErrorAction SilentlyContinue | Select-Object -First 60 | ForEach-Object { Write-Host $_ }

$outDir = Join-Path $root "build\x64\$Configuration"
Get-ChildItem (Join-Path $root 'build') -Recurse | Select-Object FullName, Length | Out-File (Join-Path $logs 'tree.txt')
$sys = Get-ChildItem $outDir -Recurse -Filter 'htixtp.sys' | Select-Object -First 1
$inf = Get-ChildItem $outDir -Recurse -Filter 'htixtp.inf' | Select-Object -First 1
if (-not $sys -or -not $inf) { throw "Build outputs not found under $outDir (see out\logs\tree.txt)" }
Write-Host "sys: $($sys.FullName)"
Write-Host "inf: $($inf.FullName)"

# --- 3. Package folder ---
$pkgRoot = Join-Path $root 'out\TouchpadFix'
$drv = Join-Path $pkgRoot 'driver'
if (Test-Path $pkgRoot) { Remove-Item $pkgRoot -Recurse -Force }
New-Item -ItemType Directory -Force -Path $drv | Out-Null
Copy-Item $sys.FullName, $inf.FullName -Destination $drv

# --- 4. Test certificate and signing ---
function Find-Tool([string]$Name) {
    $found = @()
    $found += Get-ChildItem (Join-Path $root 'packages') -Recurse -Filter $Name -ErrorAction SilentlyContinue
    $found += Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter $Name -ErrorAction SilentlyContinue
    $pick = $found | Where-Object { $_.FullName -match '\\x64\\' } | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $pick) { $pick = $found | Sort-Object FullName -Descending | Select-Object -First 1 }
    if (-not $pick) { throw "$Name not found" }
    return $pick.FullName
}

$signtool = Find-Tool 'signtool.exe'
$inf2cat  = Find-Tool 'Inf2Cat.exe'
Write-Host "signtool: $signtool"
Write-Host "inf2cat:  $inf2cat"

$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=TouchpadFix Test Signing' `
    -CertStoreLocation Cert:\CurrentUser\My -HashAlgorithm sha256 -KeyAlgorithm RSA -KeyLength 2048 `
    -NotAfter (Get-Date).AddYears(20)
Export-Certificate -Cert $cert -FilePath (Join-Path $pkgRoot 'TouchpadFix.cer') | Out-Null

& $signtool sign /v /fd sha256 /sha1 $cert.Thumbprint /s My (Join-Path $drv 'htixtp.sys') *>&1 | Tee-Object -FilePath (Join-Path $logs 'sign_sys.log')
if ($LASTEXITCODE -ne 0) { throw 'signing htixtp.sys failed' }

$catalogMade = $false
foreach ($os in @('10_GE_X64', '10_NI_X64', '10_CO_X64', '10_X64')) {
    & $inf2cat "/driver:$drv" "/os:$os" /verbose *>&1 | Out-File (Join-Path $logs "inf2cat_$os.log")
    if ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $drv 'htixtp.cat'))) {
        Write-Host "Inf2Cat succeeded with /os:$os"
        $catalogMade = $true
        break
    }
    Write-Host "Inf2Cat /os:$os failed:"
    Get-Content (Join-Path $logs "inf2cat_$os.log") | Select-Object -Last 15 | ForEach-Object { Write-Host "  $_" }
}
if (-not $catalogMade) { throw 'Inf2Cat failed for every OS value' }

& $signtool sign /v /fd sha256 /sha1 $cert.Thumbprint /s My (Join-Path $drv 'htixtp.cat') *>&1 | Tee-Object -FilePath (Join-Path $logs 'sign_cat.log')
if ($LASTEXITCODE -ne 0) { throw 'signing htixtp.cat failed' }

# --- 5. Tools and instructions ---
Copy-Item (Join-Path $root 'tools\tpctl.ps1') -Destination $pkgRoot
Copy-Item (Join-Path $root 'tools\README.txt') -Destination $pkgRoot

Get-ChildItem $pkgRoot -Recurse | ForEach-Object {
    if (-not $_.PSIsContainer) {
        $hash = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
        "{0,-40} {1,10}  {2}" -f $_.FullName.Substring($pkgRoot.Length + 1), $_.Length, $hash
    }
} | Tee-Object -FilePath (Join-Path $logs 'package.txt')

Write-Host 'Package ready: out\TouchpadFix'
