$json = Get-Content "..\package.json" | ConvertFrom-Json
$ver = $json.version
Set-Content -Path "version.h" -Value "#define APP_VERSION \"$ver\""
