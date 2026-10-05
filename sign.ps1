<#
  Firma OpenDock.exe con Authenticode y publica su SHA-256.

  Las versiones publicadas las firma SignPath Foundation desde el CI (release.yml);
  este script es para firmar a mano con un certificado propio.

  Solo una firma con certificado de confianza pública quita el aviso de SmartScreen
  ("Windows protegió su PC") y reduce los falsos positivos de antivirus:
    - Azure Artifact Signing (antes Trusted Signing): ~10 USD/mes, la vía más barata.
    - Certificado OV/EV de una CA (Sectigo, DigiCert, SSL.com...).
  Un certificado autofirmado NO sirve en otros equipos.

  Uso:
    .\sign.ps1 -Thumbprint <huella del cert en CurrentUser\My>
    .\sign.ps1 -Pfx cert.pfx
    .\sign.ps1 -AzureMetadata metadata.json -AzureDlib <ruta>\Azure.CodeSigning.Dlib.dll
#>
param(
  [string]$Thumbprint,
  [string]$Pfx,
  [string]$AzureMetadata,
  [string]$AzureDlib,
  [string]$File = "OpenDock.exe",
  [string]$Timestamp = "http://timestamp.acs.microsoft.com"
)
$ErrorActionPreference = "Stop"

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signtool) { throw "No se encontró signtool.exe: instala el Windows SDK (componente 'Signing Tools')." }

$common = @("sign", "/fd", "SHA256", "/tr", $Timestamp, "/td", "SHA256", "/d", "OpenDock")
if ($AzureMetadata) {
  & $signtool.FullName @common /dlib $AzureDlib /dmdf $AzureMetadata $File
} elseif ($Pfx) {
  $pw = Read-Host "Contraseña del PFX" -AsSecureString
  $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR([Runtime.InteropServices.Marshal]::SecureStringToBSTR($pw))
  & $signtool.FullName @common /f $Pfx /p $plain $File
} elseif ($Thumbprint) {
  & $signtool.FullName @common /sha1 $Thumbprint $File
} else {
  throw "Indica -Thumbprint, -Pfx o -AzureMetadata."
}
if ($LASTEXITCODE) { throw "signtool falló ($LASTEXITCODE)" }

& $signtool.FullName verify /pa /v $File | Out-Null
$hash = (Get-FileHash $File -Algorithm SHA256).Hash
"$hash  $File" | Set-Content -Encoding ascii "$File.sha256"
"Firmado. SHA-256: $hash"
