param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

# Read-only: do not launch the application or change scaling/window state.
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repoRoot "x64\$Configuration\capslock-switcher-settings.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "Build the settings executable first: $exe" }
$kitRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$mt = Get-ChildItem "$kitRoot\bin\*\x64\mt.exe" | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $mt) { throw 'Windows SDK manifest tool was not found.' }
$manifestPath = Join-Path ([IO.Path]::GetTempPath()) ("settings-dpi-" + [guid]::NewGuid() + '.manifest')
try {
    & $mt.FullName -nologo "-inputresource:$exe;#1" "-out:$manifestPath"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read the embedded executable manifest.' }
    [xml]$manifest = Get-Content -LiteralPath $manifestPath -Raw
    $dpi = $manifest.SelectSingleNode("//*[local-name()='dpiAwareness' and namespace-uri()='http://schemas.microsoft.com/SMI/2016/WindowsSettings']")
    if (-not $dpi -or $dpi.InnerText.Trim() -ne 'PerMonitorV2') {
        throw 'The settings executable must embed PerMonitorV2; main-process DPI settings do not apply to it.'
    }
    Write-Output "PASS: $Configuration settings executable embeds PerMonitorV2"
} finally {
    if (Test-Path -LiteralPath $manifestPath) { Remove-Item -LiteralPath $manifestPath }
}

# Check the live HWND too when this configuration's window is already open.
$running = Get-Process capslock-switcher-settings -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -eq $exe -and $_.MainWindowHandle -ne 0 }
if (-not $running) {
    Write-Output 'SKIP: live window check (settings window is not open)'
    return
}
if (-not ('SettingsDpiCheck' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SettingsDpiCheck {
    [DllImport("user32.dll")] public static extern IntPtr GetWindowDpiAwarenessContext(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool AreDpiAwarenessContextsEqual(IntPtr first, IntPtr second);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
}
'@
}
foreach ($process in $running) {
    $context = [SettingsDpiCheck]::GetWindowDpiAwarenessContext($process.MainWindowHandle)
    if (-not [SettingsDpiCheck]::AreDpiAwarenessContextsEqual($context, [IntPtr](-4))) {
        throw "Settings window PID $($process.Id) is not PerMonitorV2-aware."
    }
    $dpi = [SettingsDpiCheck]::GetDpiForWindow($process.MainWindowHandle)
    if ($dpi -eq 0) { throw 'The settings window no longer exists.' }
    Write-Output "PASS: live settings window is PerMonitorV2, DPI=$dpi"
}
