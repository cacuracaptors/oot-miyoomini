The Legend of Zelda: Ocarina of Time - Miyoo Mini Plus / OnionOS Port
=====================================================================
https://github.com/cacuracaptors/oot-miyoomini

A native port of the Ocarina of Time decompilation by the zeldaret team
(https://github.com/zeldaret/oot) to the Miyoo Mini Plus running OnionOS.
It plays the USA 1.1 (Rev A) version, built from your own ROM, and can also
add German and French from your European ROM. No game data is included:
you need your own copy of the game.

INSTALLATION
------------
1. Extract the release zip to the root of your OnionOS SD card.
2. Copy your ROM of "The Legend of Zelda: Ocarina of Time (USA) (Rev A)"
   (the 1.1 version) into this folder (Roms/PORTS/Games/ZeldaOoT/).
   .z64, .n64 or .v64 all work, also inside a .zip; the file name doesn't
   matter.
3. Optional, for German and French: also copy your "The Legend of Zelda:
   Ocarina of Time (Europe) (En,Fr,De) (Rev A)" ROM into this folder.
4. Open "Zelda - Ocarina of Time" from the Ports list (use "Refresh roms"
   at the bottom of the list if it doesn't show up).

The first start builds the game data from the ROM (oot.cart, about 73 MB).
It takes a few minutes, shows a progress screen and only happens once; then
the European ROM, if it is there, gives the German and French files. After
that the ROMs can be removed. Choose the language in Menu+Start > General >
Language.
To update the port to a new version, just replace the files: the game data
stays on the SD card.

CONTROLS
--------
  D-pad ....................... move (analog stick)
  A ........................... A
  B ........................... jump (and B in menus and dialogs)
  Y ........................... sword (the N64's B)
  L2 / X / R2 ................. C-Left / C-Down / C-Right
  R2 (hold) ................... walk slowly (a quick tap is still C-Right)
  Select (tap) ................ C-Up (first person, Navi)
  L1 .......................... Z (target)
  R1 .......................... R (shield)
  Start ....................... Start
  Select+L1 ................... minimap on/off (the N64's L)
  Select+Up / Select+Down ..... brightness
  Select+Left / Select+Right .. switch tunic / boots
  Menu+Start .................. options menu
  Menu+R2 / Menu+L2 ........... save / load the quick state
  Menu ........................ Game Switcher / back to OnionOS / previous
                                game, as set in OnionOS Tweaks (the state
                                is saved first)

With the manual jump turned off (Menu+Start > Controls) the buttons are the
original ones: B is the sword, Y C-Left, X C-Down, R2 C-Right, L2 C-Up.
With "Jump on B" off, X jumps and B is the sword.
PLAYING THE OCARINA
-------------------
While the ocarina is out, the buttons change so that every note is easy to
reach:
  A ........................... A note
  D-pad Up / Down ............. C-Up / C-Down notes
  D-pad Left / Right .......... C-Left / C-Right notes
  L1 or L2 (with a note) ...... Z: the note goes a half step down
  R1 or R2 (with a note) ...... R: the note goes a half step up
  B or Start .................. put the ocarina away
Link doesn't move while playing, and X, Y and a tap on Select do nothing.
Select+D-pad keeps its usual functions (brightness, tunic and boots).

FEATURES
--------
- Native 640x480 with dynamic resolution (the 3D picture gets sharper or
  softer by itself to keep the game smooth)
- 30 FPS: the game's 20 FPS are interpolated to 30 frames per second
- A software renderer made for the Miyoo Mini Plus, using both CPU cores;
  the HUD, dialog boxes and title logo drawn sharp at full 640x480
- A new HD title logo; adjustable brightness (Select+Up/Down)
- Optional retro filter (CRT look): scanlines, or scanlines with an RGB
  mask, with adjustable strength; the game is then drawn at the N64's
  320x240, so every game line matches the filter's lines
- CPU clock picked by device (1800 MHz on the Mini Plus, 1600 MHz on the
  Mini, Mini v4 and Mini Flip)
- Manual jump button, mid-air steering, smooth D-pad turning, natural and
  smooth aiming, sprint (keep holding A after a roll), ocarina on the
  D-pad, real vibration, tunic/boots switching without the pause menu
- English, Japanese, Brazilian Portuguese (the Zelda 64 BR fan translation),
  Italian (the Zelda 64 ITA fan translation) and, from your European ROM,
  German and French
- In-game options menu (Menu+Start) in all six languages, with 10 save
  state slots with pictures
- Many optional enhancements from Ship of Harkinian (off by default), fixes
  (on by default) and cheats (off by default); see the README on GitHub
  for the full list
- OnionOS integration: save states and the Game Switcher, save on power-off
  with automatic resume, autosave every few minutes and when you quit

TIPS
----
If after changing a setting you notice stuttering or uneven frame pacing,
lower the dynamic resolution target (Menu+Start > Graphics > Dynamic
resolution target, 30 ms by default): the resolution then steps down a bit
sooner and the game stays smooth.

FILES
-----
  options.txt ......... all the options of the port (also in Menu+Start)
  saves/ .............. the game's save, save states
  idiomas/ ............ language packs
  log.txt ............. the game's log - please attach it to bug reports

CREDITS
-------
- Ocarina of Time decompilation by the zeldaret team
  https://github.com/zeldaret/oot
- Ship of Harkinian by HarbourMasters (the enhancements, fixes and cheats
  follow its code) - https://github.com/HarbourMasters/Shipwright
- Zelda 64 BR (Brazilian Portuguese translation, v1.8) by Hyllian, Odin,
  Fserve and Solid_One, with graphics from Patcher64+
- Zelda 64 ITA (Italian translation, v1.00, 2001) by XXcentury
- Audio microcode in C based on mupen64plus's HLE
- Miyoo Mini toolchain by Shaun Inman
- Options menu font: DejaVu Sans
- Original The Legend of Zelda: Ocarina of Time (1998) by Nintendo
- Miyoo Mini Plus port by cacuracaptors

The Legend of Zelda is a trademark of Nintendo.
