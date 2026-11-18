# Builds a Deckrunner release from this checkout, exactly as the release workflow does (it calls this script), so a
# local run catches what CI would. For a faithful local check, run it in a fresh clone:
#
#   git clone --recursive https://github.com/londonbrown/Deckrunner.git <temp folder>
#   pwsh <temp folder>/tools/release/ci-build.ps1 -Version 0.0.0-local -Work <another temp folder>
#
# Needs Visual Studio 2022 (C++), CMake, rustup, the .NET 10 SDK and Python 3.11+. Output:
# build/release/Deckrunner-<Version>.zip in the checkout.
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$Work = (Join-Path ([IO.Path]::GetTempPath()) 'deckrunner-ci')
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$Toolchain = '1.98.1'
$WolvenKitUrl = 'https://github.com/WolvenKit/WolvenKit/releases/download/10.0.0-beta.1/WolvenKit.Console-10.0.0-beta.1.zip'
$WolvenKitSha = '292185c2f51d1ca85b95e1a7ca63a2d1bc8a3fc5ee387275e9a42ec78103155e'  # the player's setup pins the same
New-Item -ItemType Directory -Force $Work | Out-Null

# Every git command below (the engine's upstream fetch) checks files out byte for byte, whatever the machine's
# core.autocrlf (the release ships some of those files), and paths past Windows' 260 characters work.
$env:GIT_CONFIG_COUNT = '2'; $env:GIT_CONFIG_KEY_0 = 'core.autocrlf'; $env:GIT_CONFIG_VALUE_0 = 'false'
$env:GIT_CONFIG_KEY_1 = 'core.longpaths'; $env:GIT_CONFIG_VALUE_1 = 'true'

function Step($name) { Write-Host "`n== $name" -ForegroundColor Cyan }

Step 'Rust'
rustup toolchain install $Toolchain --profile minimal
$env:RUSTUP_TOOLCHAIN = $Toolchain

Step "Skate engine sources (deps/deckrunner-engine, upstreams from Deckrunner's forks)"
& "$Repo/deps/deckrunner-engine/tools/fetch-upstreams.ps1"
& "$Repo/deps/deckrunner-engine/tools/prepare-skate-overlay.ps1"

Step 'WolvenKit (wkprobe compiles against it)'
$zip = Join-Path $Work 'wolvenkit.zip'
if (-not (Test-Path $zip)) { Invoke-WebRequest $WolvenKitUrl -OutFile $zip }
$sha = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
if ($sha -ne $WolvenKitSha) { Remove-Item $zip; throw "WolvenKit zip SHA-256 $sha does not match the pin" }
$wk = Join-Path $Work 'wolvenkit'
if (-not (Test-Path (Join-Path $wk 'WolvenKit.RED4.dll'))) { Expand-Archive $zip $wk -Force }
$env:WOLVENKIT_DIR = $wk

Step 'Plugin'
$probe = Join-Path $Work 'probe'
cmake -S "$Repo/stageA/probe" -B $probe -A x64
cmake --build $probe --config Release --target deckrunner_probe

Step 'Runtime'
# Rust embeds source paths in panic messages: remap the checkout and the home folder (Cargo's crate sources).
$env:RUSTFLAGS = "--remap-path-prefix=$Repo=src --remap-path-prefix=$HOME=~ -C link-arg=/PDBALTPATH:%_PDB%"
$env:CARGO_TARGET_DIR = Join-Path $Work 'cargo'
Push-Location "$Repo/stageB/deckrunner-runtime"
try { cargo build --release --locked } finally { Pop-Location }

Step 'Package and scan'
python "$Repo/tools/release/build-release.py" --version $Version `
    --plugin-dll (Join-Path $probe 'Release/deckrunner_probe.dll') `
    --runtime-dll (Join-Path $env:CARGO_TARGET_DIR 'release/deckrunner_runtime.dll')
