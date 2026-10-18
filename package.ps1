param(
    [ValidateSet('x64','arm64')][string]$Architecture = 'x64',
    [string]$Version = '0.3.0',
    [string]$InnoCompiler = (Join-Path $PSScriptRoot 'build\tools\InnoSetup\ISCC.exe'),
    [switch]$SkipInstaller
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'Use a numeric major.minor.patch version.' }
$dist = Join-Path $PSScriptRoot "dist\$Architecture"
$output = Join-Path $PSScriptRoot 'releases'
$stage = Join-Path $output "CursorCabinet-$Version-$Architecture-Portable"
if (!(Test-Path -LiteralPath (Join-Path $dist 'CursorCabinet.exe'))) { throw 'Build the native executable first.' }
if (!(Test-Path -LiteralPath (Join-Path $dist 'Packs'))) { throw 'Download pinned packs first: ./scripts/download-packs.ps1' }
if (!(Test-Path -LiteralPath (Join-Path $dist 'Sources'))) { throw 'Corresponding artwork sources are required for packaging.' }
if (Test-Path -LiteralPath $stage) { throw "Staging folder already exists: $stage. Use a new version or explicitly clear the old staging folder." }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item -LiteralPath (Join-Path $dist 'CursorCabinet.exe') -Destination $stage
foreach ($name in @('Packs','Sources','packs.lock.json')) { Copy-Item -LiteralPath (Join-Path $dist $name) -Destination $stage -Recurse }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE') -Destination (Join-Path $stage 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.md') -Destination $stage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'VALIDATION.md') -Destination $stage
$sourceStage = Join-Path $stage 'Sources\CursorCabinet'
New-Item -ItemType Directory -Force $sourceStage | Out-Null
foreach ($name in @('native','tests','scripts','packaging','assets','tools','.clang-format','AGENTS.md','VALIDATION.md','build-native.ps1','package.ps1','README.md','LICENSE','THIRD_PARTY_NOTICES.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $sourceStage -Recurse
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = "$stage.zip"
if (Test-Path -LiteralPath $zip) { throw "Archive already exists: $zip" }
[IO.Compression.ZipFile]::CreateFromDirectory($stage,$zip,[IO.Compression.CompressionLevel]::Optimal,$true)
if (!$SkipInstaller) {
    if (!(Test-Path -LiteralPath $InnoCompiler)) { throw 'Inno Setup compiler is required; supply -InnoCompiler or use -SkipInstaller.' }
    & $InnoCompiler /Q "/DPackageDir=$stage" "/DOutputPath=$output" "/DAppVersion=$Version" "/DTargetArch=$Architecture" (Join-Path $PSScriptRoot 'packaging\CursorCabinet.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
}
$artifacts = @(Get-Item -LiteralPath $zip)
$setup = Join-Path $output "CursorCabinet-$Version-$Architecture-Setup.exe"
if (!$SkipInstaller) { $artifacts += Get-Item -LiteralPath $setup }
$hashes = $artifacts | ForEach-Object { $h=Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256; "$($h.Hash.ToLowerInvariant())  $($_.Name)" }
Set-Content -LiteralPath (Join-Path $output "SHA256SUMS-$Architecture.txt") -Value $hashes -Encoding ascii
Write-Host "Packaged $Architecture artifacts in $output"
