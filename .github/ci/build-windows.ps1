# Build with MSVC (static runtime, static libgit2 from vcpkg, rapport linked
# in), run the tests, and package: a zip and an NSIS installer, each holding
# a self-contained tortilla.exe and the Explorer extension. Leaves them in
# dist\. Expects VCPKG_ROOT (rapport's vcpkg action sets it).
#   .github/ci/build-windows.ps1 [-Version 0.2] [-ConfigureOnly]
# -ConfigureOnly stops once vcpkg has built the ports, so CI can save its
# cache before anything else can fail.
param([string]$Version = '', [switch]$ConfigureOnly)
$ErrorActionPreference = 'Stop'
function Check($what) { if ($LASTEXITCODE) { throw "$what failed ($LASTEXITCODE)" } }

if (-not $Version -and $env:GITHUB_REF_TYPE -eq 'tag') { $Version = $env:GITHUB_REF_NAME -replace '^v', '' }
$appVersion = if ($Version) { $Version -replace '-.*$', '' } else {
    (Select-String -Path CMakeLists.txt -Pattern 'project\(tortilla VERSION ([0-9.]+)').Matches[0].Groups[1].Value
}
$suffix = if ($Version) { "-$Version" } else { '' }

cmake -S . -B build-win -G 'Visual Studio 17 2022' -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static
Check 'configure'
if ($ConfigureOnly) { return }
cmake --build build-win --config Release --parallel `
    --target tortilla tortilla-shell tortilla-tests tortilla-rebase-tests
Check 'build'
$bin = 'build-win/bin/Release'

# The tests make their own repositories: git needs an identity.
git config --global user.name 'Tortilla CI'
git config --global user.email 'ci@tortilla.invalid'
foreach ($t in 'tortilla-tests', 'tortilla-rebase-tests') {
    $scratch = New-Item -ItemType Directory (Join-Path $env:RUNNER_TEMP "$t-$(Get-Random)")
    & "$bin/$t.exe" $scratch.FullName
    Check $t
}

$stage = 'dist/Tortilla'
Remove-Item -Recurse -Force dist -ErrorAction SilentlyContinue
New-Item -ItemType Directory "$stage/bin" | Out-Null
Copy-Item "$bin/tortilla.exe", "$bin/tortilla-shell.dll", data/windows/tortilla.ico "$stage/bin/"
Copy-Item README.md, LICENSE $stage
# Self-contained: nothing but Windows' own DLLs, for the exe and for the
# extension Explorer loads.
python rapport/ci/check-imports.py "$stage/bin/tortilla.exe" "$stage/bin/tortilla-shell.dll"
Check 'the import check'

Compress-Archive -Path $stage -DestinationPath "dist/tortilla$suffix-windows-x86_64.zip"
$makensis = Get-Command makensis -ErrorAction SilentlyContinue
if (-not $makensis) {
    $default = "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
    if (-not (Test-Path $default)) { choco install nsis -y --no-progress | Out-Null }
    $makensis = $default
}
& $makensis /V2 "/DVERSION=$appVersion" "/DSTAGE=$((Resolve-Path $stage).Path)" `
    "/DOUT=$((Resolve-Path dist).Path)\tortilla$suffix-windows-x86_64-setup.exe" data/windows/tortilla.nsi
Check 'makensis'
Remove-Item -Recurse -Force $stage
Get-ChildItem dist
