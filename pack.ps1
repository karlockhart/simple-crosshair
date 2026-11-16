<#
.SYNOPSIS
  Packages SimpleCrosshair.exe as an MSIX.

.DESCRIPTION
  pack.ps1          Microsoft Store build: uses the identity Partner Center gave the app
                    (store\store.json) and is left unsigned, since the Store signs it. Upload the
                    .msix in Partner Center.
  pack.ps1 -Direct  Build for installing outside the Store: uses your own publisher identity and
                    is signed with Azure Artifact Signing (sign.ps1), so it installs by
                    double-click on any Windows 10/11 PC. Also the way to test the package locally.

  Build SimpleCrosshair.exe first (build.bat). The version comes from simple-crosshair.rc
  (FILEVERSION); the Store needs the last part to be 0. See STORE.md.

.EXAMPLE
  .\pack.ps1 -Direct
#>
[CmdletBinding()]
param(
    [switch] $Direct,
    [string] $Version  # default: FILEVERSION in simple-crosshair.rc, e.g. 1.0.0.0
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $root 'SimpleCrosshair.exe'
if (-not (Test-Path $exe)) { throw 'SimpleCrosshair.exe not found. Run build.bat first.' }

if (-not $Version) {
    $m = Select-String -Path (Join-Path $root 'simple-crosshair.rc') -Pattern '^\s*FILEVERSION\s+(\d+),\s*(\d+),\s*(\d+),\s*(\d+)'
    if (-not $m) { throw 'No FILEVERSION in simple-crosshair.rc. Pass -Version 1.2.3.0' }
    $Version = ($m.Matches[0].Groups[1..4] | ForEach-Object { $_.Value }) -join '.'
}
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "Version must have four parts, e.g. 1.2.3.0 (got $Version)." }
if (-not $Direct -and $Version -notmatch '\.0$') { throw "The Store needs the last version part to be 0 (got $Version)." }

# Identity
$cfg = Get-Content -Raw (Join-Path $root 'store\store.json') | ConvertFrom-Json
if ($Direct) {
    $values = @{ IdentityName = $cfg.DirectIdentityName; Publisher = $cfg.DirectPublisher
                 PublisherDisplayName = $cfg.DirectPublisherDisplayName }
} else {
    $values = @{ IdentityName = $cfg.IdentityName; Publisher = $cfg.Publisher
                 PublisherDisplayName = $cfg.PublisherDisplayName }
}
foreach ($k in @($values.Keys)) {
    if (-not $values[$k] -or $values[$k] -like '<*') {
        throw "Fill in $k in store\store.json from Partner Center first (see STORE.md), or use -Direct."
    }
}
$values.Version = $Version

# SDK tools (makeappx, makepri) from the newest Windows SDK
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$sdk = Get-ChildItem $kits -Directory |
    Where-Object { $_.Name -match '^10\.\d+\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName 'x64\makeappx.exe')) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdk) { throw 'makeappx.exe not found. Install the Windows SDK: winget install -e --id Microsoft.WindowsSDK.10.0.26100' }
$makeappx = Join-Path $sdk.FullName 'x64\makeappx.exe'
$makepri = Join-Path $sdk.FullName 'x64\makepri.exe'

# Layout: exe + logos + filled-in manifest
$work = Join-Path $root 'msix'
$layout = Join-Path $work 'layout'
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force (Join-Path $layout 'Assets') | Out-Null
Copy-Item $exe $layout
Copy-Item (Join-Path $root 'store\Assets\*.png') (Join-Path $layout 'Assets')

$manifest = Get-Content -Raw (Join-Path $root 'store\AppxManifest.xml')
foreach ($k in $values.Keys) {
    $manifest = $manifest.Replace("`$($k)", [Security.SecurityElement]::Escape($values[$k]))
}
if ($manifest -match '\$\(\w+\)') { throw 'AppxManifest.xml still has an unfilled $(...) value.' }
[IO.File]::WriteAllText((Join-Path $layout 'AppxManifest.xml'), $manifest, (New-Object Text.UTF8Encoding $false))

# resources.pri maps the scale-/targetsize- logo variants to the names in the manifest
$priConfig = Join-Path $work 'priconfig.xml'
& $makepri createconfig /cf $priConfig /dq en-US /pv 10.0.0 /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri createconfig failed ($LASTEXITCODE)." }
& $makepri new /pr $layout /cf $priConfig /mn (Join-Path $layout 'AppxManifest.xml') /of (Join-Path $layout 'resources.pri') /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri new failed ($LASTEXITCODE)." }

$suffix = if ($Direct) { '' } else { '-store' }
$out = Join-Path $root "SimpleCrosshair-$Version$suffix.msix"
& $makeappx pack /d $layout /p $out /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makeappx pack failed ($LASTEXITCODE)." }
Remove-Item -Recurse -Force $work

if ($Direct) {
    & (Join-Path $root 'sign.ps1') $out
    Write-Host "Built and signed $(Split-Path -Leaf $out). Double-click it to install."
} else {
    Write-Host "Built $(Split-Path -Leaf $out). Upload it in Partner Center (the Store signs it)."
}
