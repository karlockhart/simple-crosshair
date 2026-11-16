<#
.SYNOPSIS
  Authenticode-signs files with Azure Artifact Signing (signtool + Microsoft's signing client).

.DESCRIPTION
  The account details come from signing\metadata.json and the credentials from your Azure CLI
  login, so the same script signs locally (after `az login`) and in GitHub Actions (after
  azure/login). The first run downloads Microsoft's signing client from NuGet and caches it
  under %LOCALAPPDATA%\ArtifactSigning. See SIGNING.md for the one-time setup.

.EXAMPLE
  .\sign.ps1 SimpleCrosshair.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0, ValueFromRemainingArguments)]
    [string[]] $Files,
    [string] $Metadata,  # default: signing\metadata.json next to this script
    [string] $ClientVersion = '1.0.151'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest

# (Windows PowerShell 5.1 leaves $PSScriptRoot empty in param defaults, so this is set here.)
if (-not $Metadata) { $Metadata = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'signing\metadata.json' }
$Files = @($Files | ForEach-Object { (Resolve-Path $_).Path })

# Account details
$meta = Get-Content -Raw $Metadata | ConvertFrom-Json
foreach ($key in 'Endpoint', 'CodeSigningAccountName', 'CertificateProfileName') {
    if (-not $meta.$key -or $meta.$key -like '<*') {
        throw "Fill in $key in $Metadata first (see SIGNING.md)."
    }
}

# signtool: the newest x64 one in the Windows SDK; the signing client needs 10.0.22621 or later
$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$signtool = Get-ChildItem $kits -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^10\.\d+\.\d+\.\d+$' -and [version]$_.Name -ge [version]'10.0.22621.0' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1
if (-not $signtool) {
    throw 'No signtool.exe from Windows SDK 10.0.22621 or later. Install one: winget install -e --id Microsoft.WindowsSDK.10.0.26100'
}

# Microsoft's signing client (the signtool "dlib"), cached per version
$cache = Join-Path $env:LOCALAPPDATA "ArtifactSigning\Microsoft.ArtifactSigning.Client\$ClientVersion"
$dlib = Join-Path $cache 'bin\x64\Azure.CodeSigning.Dlib.dll'
if (-not (Test-Path $dlib)) {
    Write-Host "Downloading Microsoft.ArtifactSigning.Client $ClientVersion from NuGet..."
    $pkg = Join-Path ([IO.Path]::GetTempPath()) "artifactsigning-client-$ClientVersion.zip"
    $url = "https://api.nuget.org/v3-flatcontainer/microsoft.artifactsigning.client/$ClientVersion/microsoft.artifactsigning.client.$ClientVersion.nupkg"
    Invoke-WebRequest $url -OutFile $pkg -UseBasicParsing
    if (Test-Path $cache) { Remove-Item -Recurse -Force $cache }
    Expand-Archive $pkg $cache
    Remove-Item $pkg
}
$clientSig = Get-AuthenticodeSignature $dlib
if ($clientSig.Status -ne 'Valid' -or $clientSig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    throw "$dlib is not validly signed by Microsoft ($($clientSig.Status)). Delete $cache and try again."
}

# Azure login (az login locally, azure/login in GitHub Actions)
if (-not (Get-Command az -ErrorAction SilentlyContinue)) {
    throw 'The Azure CLI is not installed. Install it (winget install -e --id Microsoft.AzureCLI), then run: az login'
}
az account show --only-show-errors 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Not signed in to Azure. Run: az login' }

# Sign, timestamp (so the signature outlives the 3-day certificate), and verify
& $signtool sign /fd SHA256 /tr 'http://timestamp.acs.microsoft.com' /td SHA256 /dlib $dlib /dmdf $Metadata @Files
if ($LASTEXITCODE -ne 0) { throw "signtool sign failed (exit code $LASTEXITCODE)." }

& $signtool verify /pa @Files
if ($LASTEXITCODE -ne 0) { throw "Signature check failed (exit code $LASTEXITCODE)." }

foreach ($file in $Files) {
    $sig = Get-AuthenticodeSignature $file
    Write-Host ("Signed {0}`n  by {1}" -f (Split-Path -Leaf $file), $sig.SignerCertificate.Subject)
}
