param([switch]$LiveCursor)

$ErrorActionPreference = 'Stop'
if ($LiveCursor -and (Get-Process capslock-switcher -ErrorAction SilentlyContinue)) {
    throw 'Exit CapsLock Switcher before live cursor checks; they temporarily replace system cursors.'
}
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio C++ tools were not found.' }
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$output = Join-Path $repoRoot 'x64\Regression'
New-Item -ItemType Directory -Force -Path $output | Out-Null
Push-Location (Join-Path $repoRoot 'capslock-switcher')
try {
    & rc.exe /nologo "/fo$output\resources.res" capslock-switcher.rc
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }
    & cl.exe /nologo /std:c++20 /EHsc /W4 /DUNICODE /D_UNICODE /Od /Zi /RTC1 `
        (Join-Path $PSScriptRoot 'regression.cpp') "/Fo$output\regression.obj" `
        "/Fd$output\regression.pdb" "/Fe$output\regression.exe" `
        /link /SUBSYSTEM:CONSOLE "$output\resources.res" user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Regression compilation failed.' }
    & "$output\regression.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Regression checks failed.' }
    if ($LiveCursor) {
        & cl.exe /nologo /std:c++20 /EHsc /W4 /DUNICODE /D_UNICODE /Od /Zi /RTC1 `
            (Join-Path $PSScriptRoot 'cursor-live.cpp') "/Fo$output\cursor-live.obj" `
            "/Fd$output\cursor-live.pdb" "/Fe$output\cursor-live.exe" `
            /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib advapi32.lib
        if ($LASTEXITCODE -ne 0) { throw 'Live cursor compilation failed.' }
        & "$output\cursor-live.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Live cursor checks failed; see pixel comparisons and restoration results above.' }
    }
} finally {
    Pop-Location
}
