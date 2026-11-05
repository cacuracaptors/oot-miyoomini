# The Legend of Zelda: Ocarina of Time - Miyoo Mini Plus / OnionOS Port

A native port of [**zeldaret/oot**](https://github.com/zeldaret/oot) - the Ocarina of Time decompilation - to the
**Miyoo Mini Plus** handheld running **OnionOS**.

The Miyoo Mini Plus has no 3D GPU, so this port draws the game with a software renderer written for it:
NEON-accelerated and spread over both CPU cores, with a dynamic resolution up to the full 640x480 screen. It plays
the **USA 1.1 (Rev A)** version of Ocarina of Time, built from your own ROM, and can also add German and French
from your European ROM.

**[Download the latest release](../../releases)**

## OnionOS Exclusive Features

### Graphics and performance

- Native 640x480 with dynamic resolution: the 3D picture gets sharper or softer by itself to keep the game smooth,
  with a target frame time you can choose. It can go from 640 down to 320 pixels wide (as on the N64), but in
  practice it rarely drops much: on the Mini Plus (1800 MHz) it stays at or near the full 640 most of the time, and
  only goes down a little in the busiest areas, like the Kokiri and Kakariko villages. On the models limited to 1600 MHz (Mini, Mini v4, Mini Flip) it
  drops a little bit more often
- 30 FPS: the game's 20 FPS are interpolated to 30 frames per second (in-between frames for everything that
  moves: actors, the camera, effects, scrolling textures, skinned models like Epona), with optional steps between
  30 and 20 FPS in the heaviest scenes
- A software renderer made for the Miyoo Mini Plus, using both CPU cores, with a smooth texture filter and color
  dithering
- The HUD, the dialog boxes and the title logo are drawn by the chip's 2D engine at full 640x480 (sharp text)
- A new HD title logo
- Adjustable brightness (gamma) without any cost: Select+Up/Down during the game
- CPU clock picked by device (1800 MHz on the Mini Plus, 1600 MHz on the Mini, Mini v4 and Mini Flip), kept
  there during the game

### Controls

- A manual jump button, with an adjustable jump strength and an optional automatic jump at ledges
- Smooth D-pad turning (Link turns in smooth curves, like with an analog stick)
- Natural aiming (up looks up) and smooth aiming, for first person, the slingshot, the bow and the hookshot
- Sprint: keep holding A after a roll
- Ocarina on the D-pad (C buttons) and the shoulder buttons (Z and R bend the note)
- Real vibration (Rumble Pak), Stone of Agony included
- Select+Left/Right switches tunics and boots without opening the pause menu

### Languages

- English, Japanese (the original Japanese text, which is also in the US cartridge) and Brazilian Portuguese (the
  Zelda 64 BR fan translation by Hyllian, Odin, Fserve and Solid_One, with translated graphics)
- German and French, taken from your own European ROM: put it in the game folder (zipped or not) and the game
  extracts them once - all the text, the item and place names, the area and boss title cards and the pause menu
- The in-game options menu follows the chosen language (all five)

### In-game options menu (Menu+Start)

- Every option of the port, with a description of each, in tabs: Save, Load, General, Controls, Enhancements,
  Fixes, Cheats, Graphics
- 10 save state slots with pictures, plus the quick state (Menu+R2 saves, Menu+L2 loads)
- The game and the sound pause while the menu is open; options that need a restart ask before restarting, and the
  game goes on from where it was
- Restore defaults, and quit with an autosave
- Two color themes
- Everything is also in `options.txt`, in the game folder

### Enhancements (from Ship of Harkinian, all optional and off by default)

- **Speed-ups:** faster text (and hold B to fast-forward), Start skips cutscenes (not the ones where you learn a
  song), fast chests, fast ocarina playback, instant item putaway, faster heavy block lift, faster climbing,
  block pushing, crawling and King Zora, skip the breath animation, skip the save confirmation, faster pause menu,
  bottle emptying and rupee counter, faster Shadow Temple boat, skip pickup messages, skip Navi's and NPCs'
  forced talks, skip the warp song cutscenes, the castle stealth section, the tower escape and feeding Jabu-Jabu,
  instant scarecrow, faster Farore's Wind
- **Quality of life:** remember the save location, item counters in messages (Gold Skulltulas, Pieces of Heart,
  Heart Containers), enemy health bar, no camera wiggle at low health, better owl, quit fishing at the door, the
  Zora's Domain waterfall opens once (or by itself), Biggoron's forge time, door chains on both sides, remember the
  minimap, dynamic wallet icon, visual Stone of Agony, time flows on the file select screen, no dropped ocarina
  notes, persistent masks, Deku Nuts explode bombs, no first-person bombchus, better bombchu shopping, skip the
  magic arrow animation, instant boomerang recall, better roll, better Farore's Wind, rebottle blue fire, still
  hearts, no area title cards, Dampé all night, night Gold Skulltulas by day, shops open all hours, leave the
  Market at night, arrow cycle (R1 while aiming), bow and slingshot reticle, green hookshot reticle on
  hookshotable surfaces, time travel with the Song of Time, warp songs from the pause menu, mask select,
  Majora's Mask Bunny Hood, ask to equip new items, minimal UI, low health beep on/off
- **Minigames:** frogs' note timer, a customizable Lost Woods ocarina game, fishing (fish always bite, never
  escape, prize weights)
- **Restorations:** red Ganon blood, pulsating boss icon, colored Temple of Time medallions

### Fixes (on by default)

Gravedigging tour, raised floor switches, Zora hint dialogue, falling from vine edges, enemies near water, Deku
Nut upgrade, broken Giant's Knife, camera drift, camera swing, camera hop when surfacing, ledge camera swing, pause
menu L/R color, dungeon entrance icons, two-handed idle pose, Gerudo warrior clothes, Link's eyes while sleeping,
silver rupee jingle, Darunia's dance speed, vanishing paths.

### Cheats (all off by default)

Infinite money, health, ammo, magic, Nayru's Love and Epona boost, freeze the time of day, no item restrictions,
no sandstorm, moon jump, bomb fuse and hookshot reach, no ReDead freeze, fireproof Deku Shield, unbreakable (and
burning) Deku Sticks, hookshot everything, super tunic, shield with two-handed weapons, drops/fish/bugs never
disappear, targetable Gold Skulltulas, Keese and Guay don't attack, Dampé always gives the best prize.

### OnionOS integration

- Easy installation: copy your ROM (`.z64`, `.n64`, `.v64` or zipped) into the game folder; the game data is
  built from it on the first start, with a progress screen
- Save states and the Game Switcher (with a picture of where you left off), the Menu key actions set in Tweaks,
  save on power-off with automatic resume
- Autosave every few minutes (as "Save" in the pause menu) and when you quit
- Full sound: the N64 music and sound effects, mixed in software

## Installation

1. Download the release zip and extract it to the root of your OnionOS SD card. This places the `ZeldaOoT` folder
   in `Roms/PORTS/Games/`, and the shortcut and its image in `Roms/PORTS/Shortcuts/` and `Roms/PORTS/Imgs/`.
2. You need your own legitimate copy of **The Legend of Zelda: Ocarina of Time (USA) (Rev A)** - the 1.1 version -
   as a ROM file. Copy it into `Roms/PORTS/Games/ZeldaOoT/`. Any of `.z64`, `.n64` or `.v64` works, also inside a
   `.zip`; the file name doesn't matter.
3. Optional, for German and French: also copy your **The Legend of Zelda: Ocarina of Time (Europe) (En,Fr,De)
   (Rev A)** ROM into the same folder.
4. On OnionOS, open **Zelda - Ocarina of Time** from the Ports list (use "Refresh roms" at the bottom of the list if
   it doesn't show up).

The first start builds the game data from the ROM (`oot.cart`, about 73 MB). It takes a few minutes, shows a
progress screen, and only happens once; then the European ROM, if it is there, gives the German and French files.
After that the ROMs can be removed from the folder. Choose the language in Menu+Start > General > Language.

When you update the port to a new version, just replace the files: the game data stays on the SD card.

## Controls

| Button | Action |
|---|---|
| D-pad | Move (analog stick) |
| A | A |
| B | Jump (and B in menus and dialogs) |
| Y | Sword (the N64's B) |
| L2 / X / R2 | C-Left / C-Down / C-Right |
| Select (tap) | C-Up (first person, Navi) |
| L1 | Z (target) |
| R1 | R (shield) |
| Start | Start |
| Select+L1 | Minimap on/off (the N64's L) |
| Select+Up / Select+Down | Brightness |
| Select+Left / Select+Right | Switch tunic / boots |
| Menu+Start | Options menu |
| Menu+R2 / Menu+L2 | Save / load the quick state |
| Menu | Game Switcher / back to OnionOS / previous game, as set in OnionOS Tweaks (the state is saved first) |

With the manual jump turned off (Menu+Start > Controls), the buttons are the original ones: B is the sword,
Y C-Left, X C-Down, R2 C-Right, L2 C-Up. With "Jump on B" off, X jumps and B is the sword.
### Playing the ocarina

While the ocarina is out, the buttons change so that every note is easy to reach:

| Button | Action |
|---|---|
| A | A note |
| D-pad Up / Down / Left / Right | C-Up / C-Down / C-Left / C-Right notes |
| L1 or L2 (hold with a note) | Z: the note goes a half step down |
| R1 or R2 (hold with a note) | R: the note goes a half step up |
| B or Start | Put the ocarina away |

Link doesn't move while playing, and X, Y and a tap on Select do nothing. Select+D-pad keeps its usual functions
(brightness, tunic and boots).

## Known issues

- A few cosmetic texts of the file select options screen stay in English with German/French (their size differs
  in the European version).
- The game's log is in `log.txt`, in `Roms/PORTS/Games/ZeldaOoT/` - please attach it to your reports.

## Building from source

This port cross-compiles for ARMv7 hard-float on Windows + WSL2, with the
[union-miyoomini-toolchain](https://github.com/shauninman/union-miyoomini-toolchain) (the same glibc as OnionOS).

1. Build the [zeldaret/oot](https://github.com/zeldaret/oot) decompilation for `ntsc-1.1` with your own ROM, in
   `~/fallout-miyoo/oot`.
2. Copy the toolchain out of its Docker container to `~/fallout-miyoo/miyoomini-toolchain`.
3. `sh tools/oot-build.sh` - patches a copy of the decompilation (`tools/oot-patch.py`, `tools/oot-patch-soh.py`),
   compiles it with the port (`port/`, `platform/`) and links `~/fallout-miyoo/oot-arm/oot_headless`.
4. `sh tools/oot-release.sh 1.0.0` - the release zip, without any game data: the data that comes from the ROM is
   taken out of the executable and rebuilt on the device from the player's own cartridge file, following the
   recipe `oot_dados.rcp`, which holds no game data (`tools/oot-romdata.c`, `port/romdata.c`).

## Credits

- Ocarina of Time decompilation by the zeldaret team - https://github.com/zeldaret/oot
- Ship of Harkinian by HarbourMasters - https://github.com/HarbourMasters/Shipwright (the enhancements, fixes and
  cheats follow its code)
- Zelda 64 BR (Brazilian Portuguese translation, v1.8) by Hyllian, Odin, Fserve and Solid_One, with graphics from
  Patcher64+
- Audio microcode in C based on mupen64plus's HLE
- Miyoo Mini toolchain by Shaun Inman - https://github.com/shauninman/union-miyoomini-toolchain
- Options menu font: DejaVu Sans - https://dejavu-fonts.github.io
- Original The Legend of Zelda: Ocarina of Time (1998) by Nintendo
- Miyoo Mini Plus port by [cacuracaptors](https://github.com/cacuracaptors)

## License

The decompilation this port is based on comes with its own terms; the code added for the Miyoo Mini Plus follows
the same terms. The Legend of Zelda is a trademark of Nintendo; no game data is included - you need your own copy
of the game.

## AI-Assisted

Claude Opus 5.5 was used to write parts of the code and to speed up debugging and optimization.
