![Deckrunner](media/deckrunner-header.png)

# Deckrunner: Skate 3 in Cyberpunk 2077

Deckrunner puts Skate 3's skating into Night City: Skate's own engine runs beside Cyberpunk 2077, with Flick-It
controls, tricks, grinds, manuals, bails and session markers on Night City's streets, and V riding in third person.

**Download it from Nexus Mods:** https://www.nexusmods.com/cyberpunk2077/mods/38864

It's an unofficial fan project, unaffiliated with Electronic Arts or CD PROJEKT RED. It ships no Skate 3 or Cyberpunk
2077 code or data: a one-time setup builds everything from your own Skate 3 disc and your own game install.

## What's here

| Folder | What |
|---|---|
| `stageA/probe` | The RED4ext plugin (C++): V, the camera, her head, the board, Skate's controls and sound in the game. |
| `stageB/deckrunner-runtime` | The runtime (Rust): Skate's engine from [SK8V](https://github.com/becaring/SK8V), fed Night City's collision. |
| `release` | The player-facing setup (`setup/prepare.py`) and the release's README, licence and notice. |
| `tools/wkprobe` | The setup's WolvenKit helper (C#, GPL-3.0): V's rigs, her animation graph, the board's game files. |
| `tools/board`, `tools/release` | The board's mesh export, and the release packaging and scans. |
| `tools/analysis`, `tools/crash`, `stage0` | Development tools: ride analysis, crash dumps, the first offline test. |
| `media` | The logo and its generator. |

Building: see [`BUILDING.md`](BUILDING.md).

## Setup tools release

The [setup-tools-1 release](https://github.com/londonbrown/Deckrunner/releases/tag/setup-tools-1) holds the two small
programs the setup runs to read your Skate 3 disc (`skate-xma.exe` and `xex_image.exe`, built unchanged from SK8V
1.1.10). Nexus Mods doesn't allow programs in mod files, so the setup downloads them from there and checks them against
their SHA-256. `skate-xma` is LGPL-2.1-or-later; its complete source is inside the zip.

## Licence

Apache License 2.0 (`LICENSE`), except `tools/wkprobe` (GPL-3.0, it links WolvenKit) and the NVIDIA PhysX 3.4 headers in
`stageA/probe/third_party` (BSD-3-Clause, their licence beside them). `NOTICE` lists the work Deckrunner builds on.
