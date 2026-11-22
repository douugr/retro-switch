# Retro Switch: MD16, SNES16 and PSX32

The three retro emulators from the iPad ([MD16](https://github.com/douugr/md16), [SNES16](https://github.com/douugr/snes16) and [PSX32](https://github.com/douugr/psx32)) as **homebrew for a modded Nintendo Switch**: same names, same look as on the iPad, same features, and the **same cloud saves**, so you can save on the iPad and continue on the Switch.

| App | System | Core (libretro, built in) |
|-----|--------|---------------------------|
| **MD16** | Mega Drive / Genesis, Master System, Game Gear, SG-1000 | [Genesis Plus GX](https://github.com/libretro/Genesis-Plus-GX) |
| **SNES16** | Super Nintendo | [Snes9x 2005](https://github.com/libretro/snes9x2005) |
| **PSX32** | PlayStation | [PCSX ReARMed](https://github.com/libretro/pcsx_rearmed), with its ARM64 dynamic recompiler |

Tested on a Switch OLED with Atmosphère (emuMMC), launched from the [Sphaira](https://github.com/ITotalJustice/sphaira) homebrew menu.

## Install

Download the `.nro` files from the [latest release](../../releases/latest) and copy them to the SD card as `/switch/MD16/MD16.nro`, `/switch/SNES16/SNES16.nro` and `/switch/PSX32/PSX32.nro`. Open them from the homebrew menu.

Games go in `/md16/roms`, `/snes16/roms` and `/psx32/games` on the SD card. You can copy them there, or send them from a browser with **Upload ROMs** / **Upload games** in each app's side menu.

No games or BIOS files are included: use dumps of cartridges, discs and consoles you own.

## Features

- **The iPad look on the TV:** MD16 has the black grid, chrome italic logo and red/orange/yellow stripes of the 16-bit boxes. SNES16 has the light gray console shell with ridges and the purple molded logo. PSX32 has the CD era: jewel cases, black discs with the rainbow shine and a silver logo. Text uses distance-field fonts, so it stays sharp at any size and docked.
- **Side menu (Y):** Games, Upload, Cloud saves, Achievements and the default image filter.
- **Game menu (−):** a panel that slides in from the left with Continue, Save state, Load state, Achievements, Image, Reset and Quit, plus the PSX32 extras.
- **Save states:** three slots plus an autosave when you leave the game, each with a **picture of the moment**. "Continue where you left off?" when a game opens.
- **Image filters:** sharp pixels, smooth, **CRT TV** and **CRT TV with a curved screen**. This is the same shader as on the iPad: scanlines that widen on bright colors, a phosphor mask and composite-video blending. The Image page previews each filter live behind the menu.
- **Cloud saves:** in-game saves (cartridge saves and PS1 memory cards) sync with a **private GitHub repository** of yours. It is the same repository and the same file paths as the iPad apps, matched by the ROM's fingerprint (the RetroAchievements hash), so file names can differ between devices. Save states in slots 1 to 3 sync as well, compressed, and load on either machine: a state saved on the iPad continues on the Switch and the other way around.
  - A save is downloaded when a game opens and uploaded after you leave it.
  - If a save changed on both sides, the app asks which one to keep. Every version stays in the repository history.
  - Setup: create a private repository and a fine-grained token with *Contents: Read and write* for it. Paste both in the "Cloud saves" section of the upload page (the setup is shared by the three apps), or copy `repo=` / `token=` to `/retrocloud/config.ini`.
- **RetroAchievements:** sign in from the side menu with the Switch keyboard. Your password is sent only to RetroAchievements; the Switch keeps a sign-in token.
  - A notice slides down with the badge when you unlock an achievement. There are no notices while you are only making progress.
  - The game menu lists every achievement of the game with its badge and progress.
  - Save states keep the achievement progress.
  - For now unlocks count in Casual mode only.
- **Upload from a browser:** the upload screen shows an address and a **QR code** for phones. The address ends in a random code that changes every time the screen opens, so nobody else on the network can reach the page. Files up to 4 GB.
- **IPS/BPS patches:** give the patch the same name as the ROM (`Golden Axe.md` + `Golden Axe.bps`). It is applied to a copy when the game starts, and the original ROM is not changed.
- **PSX32:**
  - Every game gets its own memory card, or you can use the shared card. Saves are written to the SD card a moment after the game saves.
  - The **Memory cards** screen shows both cards side by side with the animated save icons. X copies a save to the other card, Y deletes it and R swaps the card in the slot.
  - **Change disc** for `.m3u` multi-disc games, **DualShock** with both analog sticks, and the console boot animation when you have a BIOS.
  - Disc formats: `.cue` + `.bin`, `.chd`, `.pbp`, `.iso`, `.ccd` and `.m3u`.
- **6-button pad (MD16):** ZR is the MODE button.

## Build

You need a Mac or Linux machine with [devkitPro](https://devkitpro.org/) (devkitA64, libnx) and these packages: `switch-sdl2 switch-mesa switch-glad switch-curl switch-mbedtls switch-zlib switch-libpng`.

```bash
cd MD16 && ./build.sh      # or SNES16 / PSX32
```

The first build downloads the core at a pinned commit, plus rcheevos, the fonts, the root certificates, stb and the QR code generator, and builds them. Later builds only compile the app. Send a build to the Switch with `nxlink -s -a <switch-ip> MD16.nro` while the homebrew menu is open.

The code is organized like this:

- `common/` holds what the three apps share: graphics (OpenGL ES 2 through SDL2 + Mesa), the screens and menus, save states, cloud saves, RetroAchievements, the upload server and patches.
- `MD16/patches/` and `SNES16/patches/` make Genesis Plus GX and Snes9x 2005 write their save states in the same layout as the 32-bit iPad build (a few structs change size with 64-bit pointers). `layout.py` generates the conversion tables from the struct layouts clang reports for both targets. Old Switch states still load.
- Each app's `source/main.cpp` only describes its core and its look.

## Licenses

- The code in this repository is released under the **MIT License** (see [LICENSE](LICENSE)). The icons are original, made for the projects.
- The cores are not included. Each one is downloaded at build time (commit pinned in each `build.sh`) and linked statically, so **each `.nro` is also subject to its core's license**:
  - Genesis Plus GX and Snes9x 2005 are free for non-commercial use (see their repositories).
  - PCSX ReARMed is licensed under the **GNU GPL v2**, so PSX32 as a whole is distributed under the GPL v2. This repository plus that commit is its complete source.
- Other parts downloaded at build time:
  - [rcheevos](https://github.com/RetroAchievements/rcheevos) (MIT)
  - [stb](https://github.com/nothings/stb) (public domain / MIT)
  - [QR Code generator](https://github.com/nayuki/QR-Code-generator) by Project Nayuki (MIT)
  - the Barlow and Saira Condensed fonts (SIL Open Font License, included in the app with their license)
  - the Mozilla root certificates from the curl project (MPL 2.0)
- Mega Drive, Genesis, Master System and Game Gear are trademarks of SEGA. Super Nintendo and Nintendo Switch are trademarks of Nintendo. PlayStation is a trademark of Sony Interactive Entertainment. This project is not affiliated with or endorsed by any of them.
