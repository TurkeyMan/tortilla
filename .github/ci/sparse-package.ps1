# Tortilla's sparse package (data/windows/AppxManifest.xml), packed and
# signed: the identity that puts its Explorer entry on Windows 11's own menu.
# build-windows.ps1 runs it; it can be run alone, needing only the Windows
# SDK (makeappx, signtool) and this tree.
#   .github/ci/sparse-package.ps1 -Version 0.1.0.0 -Out dist/tortilla.msix [-Certificate tortilla-test.cer]
# It's signed with the test certificate (CN=Tortilla Test) from the user's
# own store, made there the first time; -Certificate saves its public half,
# for trusting it by hand (an admin's Import-Certificate into
# LocalMachine\TrustedPeople). Until it's trusted, Windows won't add the
# package, and Setup skips it.
param([Parameter(Mandatory)][string]$Version, [Parameter(Mandatory)][string]$Out,
      [string]$Certificate = '')
$ErrorActionPreference = 'Stop'
$publisher = 'CN=Tortilla Test'

# The newest SDK's tools.
function Sdk-Tool($name)
{
    $found = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\$name" -ErrorAction SilentlyContinue |
        Sort-Object { [version]$_.Directory.Parent.Name } | Select-Object -Last 1
    if (-not $found) { throw "$name not found: install the Windows 10/11 SDK" }
    $found.FullName
}

$root = Resolve-Path "$PSScriptRoot/../.."
# Without its braces, as the manifest has it.
$clsid = (Select-String -Path "$root/CMakeLists.txt" -Pattern 'WINDOWS_COMMAND_CLSID \{([0-9A-Fa-f-]+)\}').Matches[0].Groups[1].Value
if (-not $clsid) { throw "CMakeLists.txt has no WINDOWS_COMMAND_CLSID" }

# The package's own files: the manifest and its logos.
$dir = Join-Path ([IO.Path]::GetTempPath()) "tortilla-package-$(Get-Random)"
New-Item -ItemType Directory "$dir/Assets" | Out-Null
try
{
    (Get-Content -Raw "$root/data/windows/AppxManifest.xml").
        Replace('@VERSION@', $Version).Replace('@COMMAND_CLSID@', $clsid) |
        Set-Content -NoNewline -Encoding utf8 "$dir/AppxManifest.xml"
    $icons = "$root/data/icons/hicolor"
    Copy-Item "$icons/48x48/apps/tortilla.png" "$dir/Assets/Square44x44Logo.png"
    Copy-Item "$icons/256x256/apps/tortilla.png" "$dir/Assets/Square150x150Logo.png"
    Copy-Item "$icons/64x64/apps/tortilla.png" "$dir/Assets/StoreLogo.png"

    $Out = [IO.Path]::GetFullPath($Out)
    New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
    # /nv: a sparse package's executable is outside it.
    & (Sdk-Tool makeappx.exe) pack /o /nv /d $dir /p $Out | Out-Host
    if ($LASTEXITCODE) { throw "makeappx failed ($LASTEXITCODE)" }
}
finally
{
    Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
}

# The test certificate: code signing, its subject the manifest's publisher.
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
    Where-Object { $_.Subject -eq $publisher -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(1) } |
    Sort-Object NotAfter | Select-Object -Last 1
if (-not $cert)
{
    $cert = New-SelfSignedCertificate -Type Custom -Subject $publisher -KeyUsage DigitalSignature `
        -FriendlyName 'Tortilla test signing' -CertStoreLocation Cert:\CurrentUser\My `
        -NotAfter (Get-Date).AddYears(5) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
}
& (Sdk-Tool signtool.exe) sign /q /fd SHA256 /sha1 $cert.Thumbprint /s My $Out | Out-Host
if ($LASTEXITCODE) { throw "signtool failed ($LASTEXITCODE)" }
if ($Certificate) { Export-Certificate -Cert $cert -FilePath $Certificate | Out-Null }
