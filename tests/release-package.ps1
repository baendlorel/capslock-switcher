param([string]$Archive)

# Read-only checks. Extract only the settings EXE/manifest into an isolated temporary directory.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content (Join-Path $repo 'package.json') -Raw | ConvertFrom-Json).version
if (-not $Archive) { $Archive = Join-Path $repo "capslock-switcher-v$version.zip" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Archive).Path)
$temp = Join-Path ([IO.Path]::GetTempPath()) ('capslock-package-check-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    $names = @($zip.Entries.FullName)
    foreach ($name in @('capslock-switcher.exe', 'capslock-switcher-settings.exe',
        'Microsoft.WindowsAppRuntime.dll', 'Microsoft.ui.xaml.dll', 'Microsoft.UI.Xaml.Controls.dll',
        'Microsoft.UI.pri', 'Microsoft.UI.Xaml.Controls.pri', 'capslock-switcher-settings.pri',
        'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll', 'README.md')) {
        if ($names -notcontains $name) { throw "Missing release dependency: $name" }
    }
    if (($names | Select-Object -Unique).Count -ne $names.Count) { throw 'Duplicate archive entries.' }
    $unwanted = @($names | Where-Object {
        $_ -match '(?i)\.(pdb|ilk|ini|log|obj|zip)$|Microsoft\.WindowsAppRuntime\.Bootstrap\.dll$' -or
        $_ -match '(^|[/\\])\.\.([/\\]|$)|^[/\\]|^[a-zA-Z]:'
    })
    if ($unwanted.Count) { throw "Unexpected archive entries: $($unwanted -join ', ')" }

    foreach ($name in 'capslock-switcher.exe', 'capslock-switcher-settings.exe') {
        $entry = $zip.GetEntry($name)
        $source = $entry.Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $zipHash = [BitConverter]::ToString($sha.ComputeHash($source)).Replace('-', '') }
        finally { $source.Dispose(); $sha.Dispose() }
        $current = Join-Path $repo "x64\Release\$name"
        if ($zipHash -ne (Get-FileHash -LiteralPath $current -Algorithm SHA256).Hash) {
            throw "ZIP contains a stale executable: $name"
        }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $temp $name))
    }
    $info = (Get-Item (Join-Path $temp 'capslock-switcher.exe')).VersionInfo
    if ($info.ProductName -ne 'CapsLockSwitcher' -or $info.FileVersion -ne $version) {
        throw 'Main executable version metadata does not match package.json.'
    }

    $kit = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots').KitsRoot10
    $mt = Get-ChildItem "$kit\bin\*\x64\mt.exe" | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $mt) { throw 'Windows SDK manifest tool not found.' }
    $manifestFile = Join-Path $temp 'settings.manifest'
    & $mt.FullName -nologo "-inputresource:$(Join-Path $temp 'capslock-switcher-settings.exe');#1" "-out:$manifestFile"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read settings manifest.' }
    [xml]$manifest = Get-Content -LiteralPath $manifestFile -Raw
    $dpi = $manifest.SelectSingleNode("//*[local-name()='dpiAwareness']")
    if ($dpi.InnerText -ne 'PerMonitorV2') { throw 'Missing PerMonitorV2 manifest.' }
    $os = $manifest.SelectSingleNode("//*[local-name()='supportedOS' and @Id='{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}']")
    if (-not $os) { throw 'Missing Windows 10/11 supportedOS declaration.' }
    $files = @($manifest.SelectNodes("//*[local-name()='file']"))
    if (-not $files.Count) { throw 'Missing self-contained WinRT registrations.' }
    foreach ($file in $files) {
        if ($names -notcontains $file.name) { throw "Manifest dependency absent from ZIP: $($file.name)" }
    }
    Write-Output "PASS: version $version; $($names.Count) ZIP entries; current executables; CRT/WinUI payload; Win10/11 + DPI manifest"
    Write-Output 'NOTE: these checks do not replace an actual Windows 10 clean-machine startup test.'
} finally {
    $zip.Dispose()
    # Only the three files created above; no recursive removal of a computed directory.
    foreach ($name in 'capslock-switcher.exe', 'capslock-switcher-settings.exe', 'settings.manifest') {
        $file = Join-Path $temp $name
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
    }
    Remove-Item -LiteralPath $temp
}