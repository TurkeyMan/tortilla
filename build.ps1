# Build from an ordinary PowerShell; use Visual Studio's CMake, not PATH's.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Get-Command git -CommandType Application -ErrorAction SilentlyContinue)) {
    throw 'Git was not found. Install Git for Windows, then open PowerShell again.'
}
if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'rapport/CMakeLists.txt'))) {
    throw 'The rapport submodule is not initialized. Run git submodule update --init --recursive from the repository folder.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio was not found. Install Visual Studio with Desktop development with C++ and C++ CMake tools for Windows.'
}
$query = @('-latest', '-products', '*', '-version', '[17.0,)', '-format', 'json', '-utf8')
$installations = (& $vswhere @query | Out-String) | ConvertFrom-Json
if ($LASTEXITCODE -or -not $installations) {
    throw 'Visual Studio 2022 or newer was not found. Install it with Desktop development with C++ and C++ CMake tools for Windows.'
}
$installations = (& $vswhere @query -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 | Out-String) | ConvertFrom-Json
if ($LASTEXITCODE -or -not $installations) {
    throw 'The Visual Studio C++ tools were not found. In Visual Studio Installer, add Desktop development with C++.'
}
$visualStudio = @($installations)[0]
$cmake = Join-Path $visualStudio.installationPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) {
    throw "Visual Studio's CMake was not found at $cmake. In Visual Studio Installer, add C++ CMake tools for Windows."
}

# Match this installation's major version against the generators its bundled
# CMake actually supports, including VS 2026 and future releases. Bind the
# instance too, so another installation cannot be chosen by the generator.
$capabilities = (& $cmake -E capabilities | Out-String) | ConvertFrom-Json
if ($LASTEXITCODE) { throw "Could not query Visual Studio's CMake: $cmake" }
if ([version]($capabilities.version.string -replace '-.*$', '') -lt [version]'3.25') {
    throw "Visual Studio's bundled CMake $($capabilities.version.string) at $cmake is too old. Update Visual Studio's C++ CMake tools for Windows to CMake 3.25 or newer."
}
$major = ([version]$visualStudio.installationVersion).Major
$generator = @($capabilities.generators | Where-Object { $_.name -like "Visual Studio $major *" })
if ($generator.Count -ne 1) {
    throw "Visual Studio's CMake does not support its Visual Studio $major generator. Update C++ CMake tools for Windows in Visual Studio Installer."
}

$build = Join-Path $PSScriptRoot 'build'
$configure = @('-S', $PSScriptRoot, '-B', $build, '-G', $generator[0].name, '-A', 'x64',
    "-DCMAKE_GENERATOR_INSTANCE=$($visualStudio.installationPath)")
$toolchain = if ($env:VCPKG_ROOT) { Join-Path $env:VCPKG_ROOT 'scripts/buildsystems/vcpkg.cmake' } else { $null }
$useVcpkg = $toolchain -and (Test-Path -LiteralPath $toolchain) -and
    (Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'ports') -PathType Container) -and
    (Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'versions/baseline.json'))
if ($useVcpkg -and -not (Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) {
    throw "VCPKG_ROOT names a full checkout without vcpkg.exe. Run $env:VCPKG_ROOT\bootstrap-vcpkg.bat first, or unset VCPKG_ROOT to use the pinned libgit2 download."
}
if ($useVcpkg) { $toolchain = (Resolve-Path -LiteralPath $toolchain).Path }

# Keep dependency installations, downloads and caches in this build directory.
# These overrides affect only commands below and are restored even on failure;
# TEMP and TMP are never changed. In particular, an inherited installed prefix
# must not turn this manifest install into modifications of a shared prefix.
$environment = @{}
$overrides = @{}
if ($useVcpkg) {
    $vcpkgBuild = Join-Path $build 'vcpkg'
    foreach ($directory in 'downloads', 'binary-cache', 'registries', 'buildtrees', 'packages') {
        New-Item -ItemType Directory -Force (Join-Path $vcpkgBuild $directory) | Out-Null
    }
    $overrides = @{
        VCPKG_DOWNLOADS = (Join-Path $vcpkgBuild 'downloads')
        VCPKG_DEFAULT_BINARY_CACHE = (Join-Path $vcpkgBuild 'binary-cache')
        VCPKG_BINARY_SOURCES = 'clear;default,readwrite'
        X_VCPKG_REGISTRIES_CACHE = (Join-Path $vcpkgBuild 'registries')
        X_VCPKG_ASSET_SOURCES = 'clear'
        VCPKG_DISABLE_METRICS = '1'
        VCPKG_VISUAL_STUDIO_PATH = $visualStudio.installationPath
    }
    $installOptions = "--x-buildtrees-root=$(Join-Path $vcpkgBuild 'buildtrees');--x-packages-root=$(Join-Path $vcpkgBuild 'packages')"
    $configure += @("-DCMAKE_TOOLCHAIN_FILE=$toolchain", '-DVCPKG_MANIFEST_MODE=ON',
        '-DVCPKG_TARGET_TRIPLET=x64-windows-static', '-DVCPKG_HOST_TRIPLET=x64-windows',
        "-DVCPKG_INSTALLED_DIR=$(Join-Path $vcpkgBuild 'installed')",
        "-DVCPKG_INSTALL_OPTIONS=$installOptions")
    Write-Host "Using the full vcpkg checkout at $env:VCPKG_ROOT (private build/vcpkg installation)."
} else {
    if ($env:VCPKG_ROOT) {
        Write-Host 'VCPKG_ROOT is not a full vcpkg checkout; using the pinned libgit2 source download instead.'
    }
    $configure += @('-DCMAKE_TOOLCHAIN_FILE=', '-DVCPKG_MANIFEST_MODE=OFF', '-DTORTILLA_FETCH_LIBGIT2=ON')
}

function Invoke-CMake([string[]]$Arguments) {
    # Native stderr is printed normally; only the program's exit code decides
    # whether configure/build failed (also on Windows PowerShell 5.1).
    $ErrorActionPreference = 'Continue'
    & $cmake @Arguments | Out-Host
    if ($LASTEXITCODE) { throw "CMake failed with exit code $LASTEXITCODE. See the output above." }
}

Write-Host "Using $($visualStudio.displayName) $($visualStudio.installationVersion)"
Write-Host "CMake $($capabilities.version.string): $cmake"
try {
    foreach ($name in $overrides.Keys) {
        $environment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $overrides[$name], 'Process')
    }
    Invoke-CMake $configure
    Invoke-CMake @('--build', $build, '--config', 'Release')
} finally {
    foreach ($name in $environment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $environment[$name], 'Process')
    }
}
Write-Host "Built Tortilla: $(Join-Path $build 'bin/Release/tortilla.exe')"
