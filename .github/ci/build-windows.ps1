# Build with MSVC (static runtime, static libgit2 from vcpkg, rapport linked
# in), run the tests, and package: a zip and an NSIS installer, each holding
# a self-contained tortilla.exe and the Explorer extension. Leaves them in
# dist\. Expects VCPKG_ROOT (rapport's vcpkg action sets it).
#   .github/ci/build-windows.ps1 [-Version 0.2] [-ConfigureOnly]
# -ConfigureOnly stops once vcpkg has built the ports, so CI can save its
# cache before anything else can fail.
param([string]$Version = '', [switch]$ConfigureOnly)
$ErrorActionPreference = 'Stop'
# Runs a command, showing its output; if it fails, its last lines also go
# up as a GitHub error annotation, readable without the full log.
function Run($what, [scriptblock]$command)
{
    # A program's stderr (git's warnings, say) isn't an error of ours.
    $ErrorActionPreference = 'Continue'
    $output = & $command 2>&1 | ForEach-Object { "$_" } | Tee-Object -Variable lines
    $output | Out-Host
    if ($LASTEXITCODE)
    {
        if ($env:GITHUB_ACTIONS) {
            $tail = ($lines | Select-Object -Last 40) -join '%0A'
            Write-Host "::error title=$what failed ($LASTEXITCODE)::$tail"
        }
        throw "$what failed ($LASTEXITCODE)"
    }
}

if (-not $Version -and $env:GITHUB_REF_TYPE -eq 'tag') { $Version = $env:GITHUB_REF_NAME -replace '^v', '' }
$appVersion = if ($Version) { $Version -replace '-.*$', '' } else {
    (Select-String -Path CMakeLists.txt -Pattern 'project\(tortilla VERSION ([0-9.]+)').Matches[0].Groups[1].Value
}
$suffix = if ($Version) { "-$Version" } else { '' }

Run 'configure' {
    cmake -S . -B build-win -G 'Visual Studio 17 2022' -A x64 `
        "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
        -DVCPKG_TARGET_TRIPLET=x64-windows-static
}
if ($ConfigureOnly) { return }
Run 'build' {
    cmake --build build-win --config Release --parallel `
        --target tortilla tortilla-shell tortilla-tests tortilla-rebase-tests
}
$bin = 'build-win/bin/Release'

# The tests make their own repositories: git needs an identity. Set for
# this process only, so that a local run leaves the user's own alone.
$env:GIT_AUTHOR_NAME = $env:GIT_COMMITTER_NAME = 'Tortilla CI'
$env:GIT_AUTHOR_EMAIL = $env:GIT_COMMITTER_EMAIL = 'ci@tortilla.invalid'
foreach ($t in 'tortilla-tests', 'tortilla-rebase-tests') {
    $scratch = New-Item -ItemType Directory (Join-Path $env:RUNNER_TEMP "$t-$(Get-Random)")
    Run $t { & "$bin/$t.exe" $scratch.FullName }
}

$stage = 'dist/Tortilla'
Remove-Item -Recurse -Force dist -ErrorAction SilentlyContinue
New-Item -ItemType Directory "$stage/bin" | Out-Null
Copy-Item "$bin/tortilla.exe", "$bin/tortilla-shell.dll", data/windows/tortilla.ico "$stage/bin/"
Copy-Item -Recurse data/windows/menu "$stage/bin/menu"
Copy-Item -Recurse data/windows/badges "$stage/bin/badges"
# Translations beside bin\ (tortilla looks in ..\translations).
New-Item -ItemType Directory "$stage/translations" | Out-Null
Get-ChildItem data/translations/*.po, rapport/i18n/*.po -ErrorAction SilentlyContinue |
    Copy-Item -Destination "$stage/translations/"
Copy-Item README.md, LICENSE, NOTICES.md $stage
# Self-contained: nothing but Windows' own DLLs, for the exe and for the
# extension Explorer loads.
Run 'the import check' { python rapport/ci/check-imports.py "$stage/bin/tortilla.exe" "$stage/bin/tortilla-shell.dll" }

Compress-Archive -Path $stage -DestinationPath "dist/tortilla$suffix-windows-x86_64.zip"
$makensis = Get-Command makensis -ErrorAction SilentlyContinue
if (-not $makensis) {
    $default = "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
    if (-not (Test-Path $default)) { choco install nsis -y --no-progress | Out-Null }
    $makensis = $default
}
Run 'makensis' {
    & $makensis /V2 "/DVERSION=$appVersion" "/DSTAGE=$((Resolve-Path $stage).Path)" `
        "/DOUT=$((Resolve-Path dist).Path)\tortilla$suffix-windows-x86_64-setup.exe" (Resolve-Path data/windows/tortilla.nsi).Path
}
Remove-Item -Recurse -Force $stage
Get-ChildItem dist
