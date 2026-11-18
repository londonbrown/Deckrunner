# Building Deckrunner

Windows, x64. Everything comes from this repository and its submodules:

| Folder | What |
|---|---|
| `deps/deckrunner-engine` | Deckrunner's Skate engine: [londonbrown/deckrunner-engine](https://github.com/londonbrown/deckrunner-engine), a fork of [SK8V](https://github.com/becaring/SK8V) 1.1.10 by becaring (branch `deckrunner`). Its pinned upstreams are Deckrunner's forks too: [2010-rust-rewrite-mashup](https://github.com/londonbrown/2010-rust-rewrite-mashup) (the Skate engine crates) and [skate-3-rust-engine](https://github.com/londonbrown/skate-3-rust-engine) (the setup's asset pipeline). |
| `deps/RED4ext.SDK` | [RED4ext.SDK](https://github.com/WopsS/RED4ext.SDK) at the commit that defines the 2.5 runtime. |
| `stageA/probe/third_party` | NVIDIA PhysX 3.4 headers (BSD-3-Clause). |

Toolchains: Visual Studio 2022 (C++), CMake 3.24+, Rust 1.98.1, .NET 10 SDK, Python 3.11+.

```
git clone --recursive https://github.com/londonbrown/Deckrunner.git
cd Deckrunner
pwsh deps/deckrunner-engine/tools/fetch-upstreams.ps1
pwsh deps/deckrunner-engine/tools/prepare-skate-overlay.ps1
```

The last two fetch the engine's pinned upstreams from Deckrunner's forks and apply the engine's patches to the Skate
crates (`deps/deckrunner-engine/build/overlay`).

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

For a release, remap your local paths out of the binary (Rust embeds source paths in panic messages):
`RUSTFLAGS="--remap-path-prefix=<your source root>=src -C link-arg=/PDBALTPATH:%_PDB%"`.

## wkprobe (`tools/wkprobe`, GPL-3.0)

It links WolvenKit's libraries, from a WolvenKit 10.0.0-beta.1 folder (the console release is enough):

```
dotnet build tools/wkprobe -c Release -p:WolvenKitDir=<WolvenKit folder>
```

At run time it reads `DKR_WOLVENKIT` (the WolvenKit folder) and `DKR_GAME` (the Cyberpunk 2077 folder).

## A release

Push a tag `v<version>` with `release/notes/<version>.md` committed: `.github/workflows/release.yml` builds everything
above on GitHub and publishes the zip as a GitHub release. Run the workflow by hand to build without publishing.

Locally:

```
python tools/release/build-release.py --version <version> --plugin-dll <deckrunner_probe.dll> --runtime-dll <deckrunner_runtime.dll>
python tools/release/build-tools.py --number <n>   # the setup tools zip (rarely)
```

`build-release.py` refuses anything game-derived, any program or script-host file (Nexus Mods quarantines them) and
this machine's user name and home folder (`tools/release/scan.py`; add other private words in
`DECKRUNNER_PRIVATE_WORDS`, separated by `;`). Set `WOLVENKIT_DIR` for its wkprobe publish.

## What's never in this repository

Skate 3 or Cyberpunk 2077 data of any kind (the setup builds it on each player's PC), and the developer's lab: logs,
captures and test copies of the games.
