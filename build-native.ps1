param(
    [ValidateSet('x64','arm64')][string]$Architecture = 'x64',
    [string]$ToolsetOverlay,
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$buildPath = Join-Path $repoRoot "build\$Architecture"
$distPath = Join-Path $repoRoot "dist\$Architecture"
New-Item -ItemType Directory -Force $buildPath,$distPath | Out-Null
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswherePath)) { throw 'Install Visual Studio 2022 Build Tools with Desktop development with C++.' }
$vsPath = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) {
    $candidates = & $vswherePath -all -products '*' -property installationPath
    $vsPath = $candidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'VC\Auxiliary\Build\vcvarsall.bat') } | Select-Object -Last 1
}
if (!$vsPath) { throw 'MSVC C++ build tools were not found.' }
$toolsetPath = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVersion = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty Name
$hostArchitecture = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
$compilerPath = Join-Path $toolsetPath "bin\Host$hostArchitecture\$Architecture\cl.exe"
if ($ToolsetOverlay) { $compilerPath = Join-Path ([IO.Path]::GetFullPath($ToolsetOverlay)) "bin\Hostx64\$Architecture\cl.exe" }
if (!(Test-Path -LiteralPath $compilerPath)) { throw "The MSVC $Architecture compiler is missing. Install its C++ build tools component." }
$includePaths = "$toolsetPath\include;$sdkRoot\Include\$sdkVersion\ucrt;$sdkRoot\Include\$sdkVersion\shared;$sdkRoot\Include\$sdkVersion\um"
$libraryPaths = "$toolsetPath\lib\$Architecture;$sdkRoot\Lib\$sdkVersion\ucrt\$Architecture;$sdkRoot\Lib\$sdkVersion\um\$Architecture"
if ($ToolsetOverlay) { $libraryPaths = "$ToolsetOverlay\lib\$Architecture;$libraryPaths" }
$compilerDirectory = Split-Path $compilerPath -Parent
$sourcePath = Join-Path $repoRoot 'native'
$manifestPath = Join-Path $sourcePath 'CursorCabinet.manifest'
$testPath = Join-Path $repoRoot 'tests\core_tests.cpp'
$compilerFlags = '/nologo /std:c++17 /utf-8 /EHsc /W4 /WX /O2 /MT /GS /guard:cf /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00'
$script = @"
@echo off
set "PATH=$compilerDirectory;$toolsetPath\bin\Host$hostArchitecture\$hostArchitecture;$sdkRoot\bin\$sdkVersion\$hostArchitecture;%PATH%"
set "INCLUDE=$includePaths"
set "LIB=$libraryPaths"
cd /d "$buildPath"
rc /nologo /c65001 /I"$sourcePath" /fo "$buildPath\CursorCabinet.res" "$sourcePath\CursorCabinet.rc"
if errorlevel 1 exit /b 1
"$compilerPath" $compilerFlags /I"$sourcePath" "$sourcePath\core.cpp" "$sourcePath\app.cpp" "$buildPath\CursorCabinet.res" /Fe:"$distPath\CursorCabinet.exe" /link /SUBSYSTEM:WINDOWS,10.00 /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf /MANIFEST:EMBED /MANIFESTINPUT:"$manifestPath" user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib comctl32.lib uxtheme.lib
if errorlevel 1 exit /b 1
"@
if ($Test) {
    $script += @"

"$compilerPath" $compilerFlags /I"$sourcePath" "$sourcePath\core.cpp" "$testPath" /Fe:"$buildPath\CoreTests.exe" /link /SUBSYSTEM:CONSOLE /DYNAMICBASE /NXCOMPAT user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib
if errorlevel 1 exit /b 1
"$compilerPath" $compilerFlags /I"$sourcePath" "$sourcePath\core.cpp" "$repoRoot\tests\integration_tests.cpp" /Fe:"$buildPath\IntegrationTests.exe" /link /SUBSYSTEM:CONSOLE /DYNAMICBASE /NXCOMPAT user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib
if errorlevel 1 exit /b 1
"@
}
$commandPath = Join-Path $buildPath 'compile.cmd'
Set-Content -LiteralPath $commandPath -Value $script -Encoding ascii
& $env:ComSpec /d /c $commandPath
if ($LASTEXITCODE -ne 0) { throw "Native $Architecture build failed." }
if ($Test -and ($Architecture -eq $hostArchitecture)) {
    & (Join-Path $buildPath 'CoreTests.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Core tests failed.' }
}
Write-Host "Built $distPath\CursorCabinet.exe"
