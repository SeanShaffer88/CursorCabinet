param(
    [string]$Version = '0.2.0',
    [string]$ReleaseDirectory = (Join-Path $PSScriptRoot '..\releases')
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$releaseRoot = [IO.Path]::GetFullPath($ReleaseDirectory)
$zip = Join-Path $releaseRoot "CursorCabinet-$Version-x64-Portable.zip"
$setup = Join-Path $releaseRoot "CursorCabinet-$Version-x64-Setup.exe"
$testRoot = Join-Path $repoRoot ("build\package-check-" + [Guid]::NewGuid().ToString('N'))
$install = Join-Path $testRoot 'Installed'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{EBFD397B-6FB6-4439-8297-C82FD4EBA133}_is1'
if (Test-Path -LiteralPath $uninstallKey) { throw 'An installed CursorCabinet copy already exists. Refusing to overwrite its installer registration during testing.' }
if (!(Test-Path -LiteralPath $zip) -or !(Test-Path -LiteralPath $setup)) { throw 'Build the x64 portable ZIP and installer before testing.' }
New-Item -ItemType Directory -Force $testRoot | Out-Null
$results = [Collections.Generic.List[string]]::new()
function Run-Checked([string]$executable,[string[]]$arguments) {
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "Process failed with exit code $($process.ExitCode): $([IO.Path]::GetFileName($executable))" }
}
try {
    Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $testRoot 'Portable')
    $portable = Join-Path $testRoot "Portable\CursorCabinet-$Version-x64-Portable"
    $portableExe = Join-Path $portable 'CursorCabinet.exe'
    if (!(Test-Path -LiteralPath $portableExe)) { throw 'The ZIP does not contain its documented launch executable.' }
    if (!(Test-Path -LiteralPath (Join-Path $portable 'Sources')) -or !(Test-Path -LiteralPath (Join-Path $portable 'LICENSE.txt'))) { throw 'Portable package is missing corresponding sources or license information.' }
    Run-Checked $portableExe @('--verify',('"'+(Join-Path $testRoot 'portable-verification.txt')+'"'))
    Run-Checked $portableExe @('--smoke-test',('"'+(Join-Path $testRoot 'portable-ui.txt')+'"'))
    $results.Add('PASS extracted portable EXE, artwork validation, native UI checks, and source/license contents')
    $installArgs = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-','/NOICONS','/TASKS=""',('/DIR="'+$install+'"'),('/LOG="'+(Join-Path $testRoot 'install.log')+'"'))
    Run-Checked $setup $installArgs
    if (!(Test-Path -LiteralPath $uninstallKey)) { throw 'Per-user uninstall registration was not created.' }
    $installedExe = Join-Path $install 'CursorCabinet.exe'
    if ((Get-FileHash -LiteralPath $installedExe).Hash -ne (Get-FileHash -LiteralPath $portableExe).Hash) { throw 'Installed executable does not match the portable build.' }
    $results.Add('PASS silent per-user installation and executable identity')
    Run-Checked $setup $installArgs
    if ((Get-FileHash -LiteralPath $installedExe).Hash -ne (Get-FileHash -LiteralPath $portableExe).Hash) { throw 'Upgrade changed the executable unexpectedly.' }
    $results.Add('PASS same-version reinstall/upgrade')
    Run-Checked $installedExe @('--verify',('"'+(Join-Path $testRoot 'installed-verification.txt')+'"'))
    Run-Checked $installedExe @('--smoke-test',('"'+(Join-Path $testRoot 'installed-ui.txt')+'"'))
    $results.Add('PASS installed EXE artwork validation and native UI checks')
    Run-Checked (Join-Path $install 'unins000.exe') @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="'+(Join-Path $testRoot 'uninstall.log')+'"'))
    for ($attempt=0;$attempt -lt 20 -and (Test-Path -LiteralPath $installedExe);$attempt++) { Start-Sleep -Milliseconds 250 }
    if ((Test-Path -LiteralPath $installedExe) -or (Test-Path -LiteralPath $uninstallKey)) { throw 'Uninstall left application files or its registration behind.' }
    $results.Add('PASS uninstall removes its executable and per-user registration')
    $report = Join-Path $releaseRoot 'package-validation-x64.txt'
    Set-Content -LiteralPath $report -Value $results -Encoding utf8
    $results | Write-Output
    Write-Output "Detailed test artifacts retained at $testRoot"
} catch {
    $results.Add("FAIL $($_.Exception.Message)")
    Set-Content -LiteralPath (Join-Path $testRoot 'failure.txt') -Value $results -Encoding utf8
    throw
}
