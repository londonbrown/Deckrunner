# Building Deckrunner

Windows, x64. The source expects two checkouts **beside** this repository:

| Folder | What |
|---|---|
| `deckrunner-sk8v` | [SK8V](https://github.com/becaring/SK8V) at tag **1.1.10** (commit 6a1a6e2), with its upstreams fetched and its patch overlay built (`tools\fetch-upstreams.ps1`, then `tools\build-rust.ps1`). The runtime links its crates; setup ships its scripts. |
| `cp-RED4ext.SDK` | [RED4ext.SDK](https://github.com/WopsS/RED4ext.SDK) at commit `a34f0ad0a9e4f37cbe1fe001e4ff68c2949d0665` (defines the 2.5 runtime). |

Toolchains: Visual Studio 2022 (C++), CMake 3.24+, Rust (stable), .NET 10 SDK, Python 3.11+ with numpy and Pillow.

## The plugin (`stageA/probe`)

```
cmake -S stageA/probe -B <build folder> -A x64
cmake --build <build folder> --config Release --target deckrunner_probe
```

## The runtime (`stageB/deckrunner-runtime`)

```
cd stageB/deckrunner-runtime
cargo build --release
```

For a release, build it with your local paths remapped out of the binary (Rust embeds source paths in panic messages):
`RUSTFLAGS="--remap-path-prefix=<your home>=~ --remap-path-prefix=<your source root>=src -C link-arg=/PDBALTPATH:%_PDB%"`,
into `%LOCALAPPDATA%\Deckrunner\cargo-release`.

## wkprobe (`tools/wkprobe`, GPL-3.0)

It links WolvenKit's libraries, from a WolvenKit 10.0.0-beta.1 folder (the console release is enough):

```
dotnet build tools/wkprobe -c Release -p:WolvenKitDir=<WolvenKit folder>
```

At run time it reads `DKR_WOLVENKIT` (the WolvenKit folder) and `DKR_GAME` (the Cyberpunk 2077 folder).

## A release

```
python tools/release/build-tools.py --number <n>          # the setup tools zip for a GitHub release (rarely)
python tools/release/build-release.py --version <version> # the Nexus zip, from an allowlist
```

`build-release.py` refuses anything game-derived, any program (Nexus Mods quarantines them) and this machine's user
name and home folder (`tools/release/scan.py`; add other private words in `DECKRUNNER_PRIVATE_WORDS`, separated by `;`).
Set `WOLVENKIT_DIR` for its wkprobe publish.

The player-facing setup is `release/setup/prepare.py`; see `release/README.md`.

## What's never in this repository

Skate 3 or Cyberpunk 2077 data of any kind (the setup builds it on each player's PC), and the developer's lab: logs,
captures and test copies of the games.
