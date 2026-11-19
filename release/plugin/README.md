# Deckrunner: Skate 3 in Cyberpunk 2077

This folder is Deckrunner's plugin (the main file). It needs data that **Deckrunner Setup** builds once from your own
Skate 3 disc and your own Cyberpunk 2077 install: download Deckrunner Setup, extract it anywhere outside the game
folder and run `Deckrunner Setup.cmd`. Setup's README has the details.

- Start and stop skating: **Ctrl+Shift+F10**. You start on foot with the board in hand; **Y** gets on and off it.
  Skate 3's controls are on the gamepad.
- Settings: `deckrunner.ini` in this folder, made from `deckrunner.default.ini` the first time the game starts with
  Deckrunner: the keys under `[keys]`, Skate 3's volume under `[audio]`, its difficulty and preloading under `[skate]`.
  Updates never overwrite it.
- Weapon wheel (optional: needs [TweakXL](https://github.com/psiberx/cp2077-tweak-xl/releases) and
  [ArchiveXL](https://github.com/psiberx/cp2077-archive-xl/releases)): V gets a **Skateboard**. Equip it in a weapon slot
  once (Inventory, Weapons), then pick it in the weapon wheel to start skating. On foot, hold **Y** (Triangle) for the
  wheel: pick the Skateboard to stop, or a weapon to stop and draw it. Without them, Ctrl+Shift+F10 does it all.
- HUD (optional: needs [Codeware](https://github.com/psiberx/cp2077-codeware/releases)): Skate's trick, score, line,
  multiplier, landing and stance, laid out as Skate 3's scoring HUD (a ring for the line's timer) in Cyberpunk's own
  style, bottom right.
  `[hud] enabled=0` in `deckrunner.ini` hides it.
- Removing it by hand: delete `red4ext\plugins\deckrunner\`, `r6\tweaks\deckrunner\`, `r6\scripts\deckrunner\` and
  `archive\pc\mod\deckrunner_items.archive` (and its `.xl`).
- Bug reports: https://github.com/londonbrown/Deckrunner/issues

Deckrunner is an unofficial fan project, unaffiliated with Electronic Arts or CD PROJEKT RED. Licence: Apache-2.0
(`LICENSE`); `NOTICE` and `THIRD-PARTY-NOTICES.txt` list the work it builds on.
