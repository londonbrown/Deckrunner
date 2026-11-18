# Stage 0: offline validation (no game involvement)

Stage 0 runs SK8V's unchanged Skate session headlessly on synthetic collision supplied by a host, then retargets
its output onto Cyberpunk 2077's player rigs. Nothing here touches the Cyberpunk install, its saves, or the original
repositories.

## Layout

| Path | What |
|---|---|
| `deckrunner-stub/` | Rust host stub: synthetic Z-up course → `rails::find_metres` → `skate_host::bridge::Session` → per-tick trace and pose |
| `retarget/retarget.py` | RigProfile prototype: Skate pose → `woman_base` / `man_base` rig, with metrics, stills and a GIF |
| `../investigation/probes/cr2w_rig.py` | decodes a Cyberpunk `.rig` (hierarchy and reference pose) from the game archive, read-only |
| `../lab/` | retail-derived data and outputs; **never commit or share** |
| `..\deckrunner-sk8v` (beside this repository) | separate SK8V clone at `6a1a6e2` with pinned upstreams and the 49-patch overlay |
| `%LOCALAPPDATA%\Deckrunner\cargo-target*` | Rust build output (C:, because I: is nearly full) |

## Reproduce

1. Build the runtime in the SK8V clone (fetches pinned sources, applies the patches, builds with Rust 1.98.1):

   ```powershell
   cd ..\deckrunner-sk8v; .\tools\fetch-upstreams.ps1; $env:CARGO_TARGET_DIR="$env:LOCALAPPDATA\Deckrunner\cargo-target"; .\tools\build-rust.ps1
   ```

2. Extract the disc files, then convert them. Run this from the SK8V clone's `tools`:

   ```bash
   python xiso.py "<Skate 3 iso>" lab/skate-disc
   ```

   Then run `convert-skate-data.py --xex …/default.xex --out …/lab/skate-data`, plus the Skate-only stages
   (`prepare-skate-audio.py`, `prepare-audio-tuning.py`, `prepare-hom-hud.py` ×3, `prepare-hom-xray.py`,
   `prepare-board.py`).

3. Decode the rigs:

   ```bash
   python investigation/probes/cr2w_rig.py "<CP>/archive/pc/content/basegame_4_appearance.archive" "<CP>/bin/x64/oo2ext_7_win64.dll" base/characters/base_entities/woman_base/woman_base.rig --json lab/cp-rigs/woman_base.json
   ```

4. Run the stub. Arguments are assets, output, seconds, steer sign and line x:

   ```bash
   deckrunner-stub.exe lab/skate-data/assets lab/stage0-line 30 1 0
   ```

5. Retarget:

   ```bash
   python stage0/retarget/retarget.py lab/stage0-line lab/skate-data/assets/private/skater.glb lab/cp-rigs/woman_base.json lab/stage0-line/retarget-woman_base
   ```
