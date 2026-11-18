# Deckrunner Setup: Skate 3 in Cyberpunk 2077

![Deckrunner](deckrunner-logo.png)

Deckrunner puts Skate 3's skating into Night City. Skate's own engine runs beside the game: Flick-It controls, board
and skater physics, tricks, grinds, manuals, bails and session markers, on Night City's streets, rails and stairs, with
traffic you can hit and Skate's own wheel sounds. V rides it in third person, with her own head, hair, hat and glasses.

It's an unofficial fan project, unaffiliated with Electronic Arts or CD PROJEKT RED. It ships no Skate 3 or Cyberpunk
2077 code or data: a one-time setup builds everything it needs on your PC from **your own** Skate 3 disc and
**your own** Cyberpunk 2077 install.

## What you need

- **Cyberpunk 2077 2.5** on PC (Steam or GOG).
- **RED4ext**, installed in the game folder ([Nexus mod 2380](https://www.nexusmods.com/cyberpunk2077/mods/2380)).
- **Your own Skate 3 Xbox 360 disc**, as a disc image (`.iso`) or an extracted folder. Tested with title ID 454108E6,
  media ID 5C087C2C, no title update.
- **A gamepad.** Xbox controllers work, and so does a PS5 DualSense on its own, without Steam Input.
- About **3 GB free** during setup (about 1.6 GB stays in the game folder afterwards), and an internet connection for
  the setup's downloads (under 100 MB, see below).

## Install

Deckrunner comes as two downloads: **Deckrunner** (the plugin, the main file) and **Deckrunner Setup** (this one).

1. Install **Deckrunner** like any mod: with Vortex or MO2, or extract it into the game folder (it holds
   `red4ext\plugins\deckrunner\`).
2. Extract **Deckrunner Setup** anywhere (not inside the game folder).
3. Run **`Deckrunner Setup.cmd`**. You only need to run it once; plugin updates don't need it again.
   - It finds Cyberpunk 2077 through Steam or GOG (or asks you), then asks for your Skate 3 disc image or folder.
   - It needs Python 3.11 or newer with numpy and Pillow. If you don't have them, it offers to download a private copy
     (about 31 MB from python.org and PyPI, each file checked against its published SHA-256). Nothing is installed
     system-wide.
   - It downloads the programs it runs, each checked against its published hash, because Nexus doesn't allow programs
     in mod files: Deckrunner's own setup tools (0.3 MB, from [Deckrunner's GitHub release](https://github.com/londonbrown/Deckrunner/releases/tag/setup-tools-1)),
     two small SK8V programs that read your Skate 3 disc; WolvenKit's command-line release (45 MB, GitHub), to read and
     write the game's files; and Microsoft's .NET 10 runtime (37 MB) if you don't have it.
   - It takes a few minutes. If it stops, run it again: it picks up where it stopped.
   - It builds Deckrunner's data into `red4ext\plugins\deckrunner\data\` and two archives into `archive\pc\mod\`.

## Play

- Load a save, stand somewhere open, and press **Ctrl+Shift+F10** to start skating. Press it again to stop.
- Skate 3's controls are on the gamepad.
- **Session markers:** LB + d-pad down sets one; hold LB + d-pad up to return to it.
- **Ctrl+Shift+F12** saves the last few seconds of a ride for a bug report (see below).
- Settings are in `red4ext\plugins\deckrunner\deckrunner.ini`: the two keys (`[keys] skate=`, `capture=`) and Skate 3's
  volume (`[audio] volume=`; 1.0 is the default, 0 turns it off).

## Uninstall

1. Run **`Deckrunner Uninstall.cmd`**. It removes what setup built: the data and the two archives.
2. Remove the **Deckrunner** plugin with your mod manager, or delete `red4ext\plugins\deckrunner\` if you installed it
   by hand.

## Bug reports

Please include:
- the newest `deckrunner_probe-*.log` from the game's `red4ext\logs\` folder;
- if you pressed Ctrl+Shift+F12, the newest files in `%LOCALAPPDATA%\Deckrunner\probe\` and `%LOCALAPPDATA%\Deckrunner\stageb\`.

Those captures hold collision and pose data built from your own games. They're fine to send to the developers for a bug
report, but please don't post them publicly.

## Licences and credits

Deckrunner is licensed under the Apache License 2.0 (`LICENSE`). It builds on SK8V, the Skate 3 engine projects behind
it, RED4ext and WolvenKit; `NOTICE` and `THIRD-PARTY-NOTICES.txt` list them and their licences. The setup's WolvenKit
helper (`setup\wkprobe`) is GPL-3.0, with its source beside it.
