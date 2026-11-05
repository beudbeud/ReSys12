<h1 align="center">ReSys12</h1>

<p align="center">
  <b>Namco System 12 and System 11 arcade emulator, as a libretro core</b><br>
  pcsx_rearmed based &middot; R3000A dynarecs (aarch64, ARMv7, x86_64) &middot; NEON GPU rasterizer &middot; C352 sound<br>
  <a href="https://www.patreon.com/Reyanon">Support on Patreon</a> or <a href="https://ko-fi.com/reyanon">Ko-Fi</a>
</p>

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Highlights](#2-highlights)
3. [Supported Games](#3-supported-games)
4. [Installation](#4-installation)
5. [Usage](#5-usage)
6. [Building from Source](#6-building-from-source)
7. [Project Layout](#7-project-layout)
8. [Accuracy and Testing](#8-accuracy-and-testing)
9. [License and Credits](#9-license-and-credits)
10. [Support](#10-support)

---

## 1. Introduction

ReSys12 is an emulator for the Namco System 12 arcade board, the PlayStation-based hardware behind
Tekken Tag Tournament, Tekken 3, Soul Calibur, Ehrgeiz, Point Blank 2 and others, and for the earlier System 11 board
(Tekken 2, Soul Edge, Xevious 3D/G, ...). The CPU, GTE and GPU come from
[pcsx_rearmed](https://github.com/notaz/pcsx_rearmed); the boards themselves (ROM banking, protection, H8/3002 and C76
sound / I/O CPUs, C352 sound, EEPROM, JVS I/O) are ported from [MAME](https://www.mamedev.org/).

System 12 is a PlayStation with twice the CPU clock (R3000A at 48 MHz), up to 4 MB of RAM, banked ROM
instead of a CD drive, and a separate H8/3002 with a 32-voice C352 for sound. System 11 keeps the PlayStation clock and
drives its C352 from a Mitsubishi C76 (M37702) microcontroller.

ReSys12 is a [libretro](https://www.libretro.com/) core for Linux (x86_64, and ARM such as the Raspberry Pi 5). Recalbox
ships it as the `namcos12` and `namcos11` systems (core `resys12`). Board accuracy is kept where the player can see or
hear it; timing details that do not change what is on screen are simplified when that buys speed.

## 2. Highlights

- R3000A dynamic recompilers: ari64 from pcsx_rearmed on ARMv7 and aarch64, lightrec on x86_64. Both are checked
  against the interpreter; lightrec gives its pictures exactly on every set.
- NEON GTE and the gpu_neon software rasterizer, fed by an asynchronous command queue.
- Idle loop skipping for the main CPU and the H8/3002 (polling, timeout and wait loops), so waiting CPUs do not
  burn host time.
- H8/3002 recompiler on 32-bit ARM: runs of its program ROM are translated to ARM code once (registers, flags and
  cycle counts kept exact; checked against the interpreter).
- KC0xx protection and ROM DMA ported from MAME, with the DMA source fixes needed by Tekken Tag Tournament.
- System 12 CD-XA board (SH-2, ATAPI drive from a CHD image, CD audio) for Um Jammer Lammy NOW! and Truck Kyosokyoku.
- System 11: main board, KEYCUS protection, ROM banks, gun board, and the C76 (M37702 interpreter) with the C352.
- Lightguns through the RetroArch lightgun device, wheels and levers on the analog stick, two-cabinet Kart Duel over
  UDP or the Recalbox link-lobby.

## 3. Supported Games

Set names are the MAME set names. ROMs must match the MAME romset (see [ROM sets](#rom-sets)); clone sets read missing
files from the parent zip.

**Status**: on a Raspberry Pi 5 with the ari64 recompiler. *Playable*: a game was played through; *Boots*: start-up,
attract mode and menus work, gameplay not reached in the test runs yet; *Partial*: runs, with the known problem in the
notes; *Not working* / *Untested* as written. Every set runs well above full speed there (9000 frames: tektagt 12.7 s,
kartduel 13.4 s, ujlnow 56.9 s; 150 s of play each).

### System 12

System 12 has no BIOS.

| Game | Sets (parent first) | Year | Status | Notes |
|---|---|---|---|---|
| Tekken Tag Tournament | `tektagt`, `tektagtc1a`, `tektagtub`, `tektagtuc1`, `tektagtjc1` | 1999 | Playable |  |
| Tekken 3 | `tekken3`, `tekken3a`, `tekken3b`, `tekken3c`, `tekken3d`, `tekken3ua`, `tekken3ud`, `tekken3ja`, `tekken3jd`, `tekken3je1` | 1997 | Playable |  |
| Soul Calibur | `soulclbr`, `soulclbrab`, `soulclbrac`, `soulclbrub`, `soulclbruc`, `soulclbrja2`, `soulclbrjb`, `soulclbrjc` | 1998 | Playable |  |
| Ehrgeiz | `ehrgeiz`, `ehrgeizua`, `ehrgeizja` | 1998 | Playable |  |
| Fighting Layer | `fgtlayer`, `fgtlayerja` | 1998 | Playable |  |
| Shin Nihon Pro Wrestling Toukon Retsuden 3 Arcade Edition | `toukon3` | 1997 | Playable |  |
| Libero Grande | `lbgrande` | 1997 | Playable | the game draws 30 pictures a second |
| Super World Stadium '98 | `sws98` | 1998 | Boots | the test runs reach a match (first innings) |
| Super World Stadium '99 | `sws99` | 1999 | Boots | as `sws98` |
| Mr. Driller | `mrdrillr`, `mrdrillrja2` | 1999 | Playable |  |
| Aqua Rush | `aquarush` | 1999 | Playable |  |
| Tenkomori Shooting | `tenkomor`, `tenkomorja1` | 1998 | Playable | vertical screen (the core sets the rotation) |
| Paca Paca Passion | `pacapp` | 1998 | Playable |  |
| Paca Paca Passion 2 | `pacapp2` | 1999 | Playable |  |
| Paca Paca Passion Special | `pacappsp` | 1999 | Playable |  |
| Derby Quiz My Dream Horse | `mdhorse` | 1998 | Playable | the game draws about 40 pictures a second |
| Kaiun Quiz | `kaiunqz` | 1999 | Playable | the game draws about 35 pictures a second |
| Point Blank 2 / Gunbarl | `ptblank2`, `gunbarl` | 1999 | Playable | lightgun (RetroArch lightgun device, or the left stick) |
| Ghoul Panic / Oh! Bakyuuun | `ghlpanic`, `ohbakyuun` | 1999 | Playable | lightgun |
| Golgo 13 | `golgo13` | 1999 | Playable | lightgun; the board tests its ROMs for about 50 s after power-on |
| Golgo 13 Kiseki no Dandou | `g13knd` | 2000 | Playable | as `golgo13` |
| Techno Drive | `technodr` | 1998 | Playable | needs `namco_emio102.zip`; wheel on the left stick, R2 gas, L2 brake |
| Attack Pla Rail | `aplarail` | 1998 | Playable | lever on the left stick |
| Kart Duel | `kartduel`, `kartduelja` | 2000 | Playable | two cabinets race over UDP, on the LAN or through the Recalbox link-lobby (see [Kart Duel link](#kart-duel-link)) |
| Tekken Tag Tournament (Japan) | `tektagtja3`, `tektagtjb` | 1999 | Not working | bad dumps, also not working in MAME |
| Super World Stadium 2000 / 2001 | `sws2000`, `sws2001` | 2000 | Boots | the test runs reach a match (first innings); not working in MAME |
| Um Jammer Lammy NOW! | `ujlnow` | 1999 | Playable | CD-XA board: disc `ujlnow/ul1-a.chd` next to the zip, CD audio |
| Truck Kyosokyoku | `truckk` | 2000 | Playable | disc `truckk/tkk2-a.chd`; 2 coins, then the horn (button 3, RetroPad B) starts a game; R2 gas, L2 brake |
| Quest for Fame | `questff` | 1999 | Untested | CD-XA board; no dump here |

### System 11

The core picks System 11 from the set name. The C76 BIOS `namcoc76.zip` (`c76.bin`) must sit next to the sets.
Statuses: played on a Raspberry Pi 5 (Recalbox, ari64), plus headless runs (3600 frames, coin, start and buttons) on
x86_64 and on the Pi 5.

| Set | Status |
|---|---|
| `tekken2`, `souledgea`, `dunkmnia`, `primglex`, `starswep`, `xevi3dg`, `pocketrc`, `danceyes`, `myangel3` | Playable |
| `ptblank2a`, `ptblank2b`, `ptblank2c`, `ptblank2ua`, `gunbarla` | Playable in the headless runs (gun at the screen centre); gun as on System 12 (RetroArch lightgun, or the left stick) |
| `tekken` | Boots; textures and HUD wrong (COH-100 board, CXD8538Q GPU) |
| `fambowl` | Not working: waits on "put the ball back" (the bowling lane's sensors are not emulated, nor in MAME) |
| other clones in `src/romload/s11_games.h` | Untested |

The gun sets are clones of the System 12 `ptblank2`: they read the files they share with it from `ptblank2.zip`.

## 4. Installation

Build the core (see [Building from Source](#6-building-from-source)) and load `resys12_libretro.so` in RetroArch, or
use the `namcos12` / `namcos11` systems of Recalbox, which ship it.

```bash
retroarch -L libretro/resys12_libretro.so roms/tektagt.zip
```

**No ROMs or game artwork are included or provided.** You must supply ROM images of games you own.

### ROM sets

- **Required MAME version: romsets from MAME 0.260 or newer.** 0.260 is the version we test with. The supported set
  list comes from a current MAME source (September 2026), and the few sets whose files or names changed after 0.260
  are recognized in both forms. Romsets older than 0.260 are not tested and may not load.
- Keep each set as one zip named after the set, for example `tektagt.zip`. Do not rename or extract them.
  Files inside the zip are matched by CRC32, so their names do not matter.
- Clone sets (for example `tektagtub`) read missing files from the parent zip (`tektagt.zip`), so keep the
  parent in the same folder, or use non-merged sets.
- System 12 has no BIOS. System 11 needs `namcoc76.zip` next to the sets.
- The CD-XA sets (`truckk`, `ujlnow`, `questff`) read their disc as MAME does: `<romdir>/<set>/<disc>.chd` next to the
  zip. Techno Drive also needs `namco_emio102.zip` (its I/O board ROM) in the same folder.

## 5. Usage

**RetroPad**: Y X B A L R L2 R2 = buttons 1-8, Start, Select = coin, L3 = test, R3 = service. With no stick to click
(arcade panel, wheel), the core options `resys12_service_mode` (`start_hold`) and `resys12_service1` (`start_b_hold`)
move them onto Start held alone for 5 s (Test, the operator menu) and Start + B held together for 5 s (Service), as
the blueflash core does; the press fires once when the 5 s are reached, and Start and B still reach the game
meanwhile. The EEPROM is the core's save RAM (`.srm`); save states go through RetroArch.

Gun games read the RetroArch lightgun device (or the left stick aims). Driving games: wheel on the left stick, R2 gas,
L2 brake; Attack Pla Rail's lever is on the left stick.

### Kart Duel link

Kart Duel links two cabinets: core option `resys12_link` sets the UDP port, `resys12_link_id` the PCB ID (1 or 2,
one per cabinet; restart the content after changing it).

Over the internet, the cabinets meet through the Recalbox [link-lobby](https://gitlab.com/recalbox/packages/recalbox-cores/link-lobby),
as the System 23 core does: with `resys12_link` on, a `sys12_comm.cfg` next to the saves (`lobby = host[:port]`,
`room = name`, `nick = name`; or `RS12_LOBBY` / `RS12_ROOM` / `RS12_NICK`) sends the cabinet to that lobby. It holds
its boot until the lobby pairs it (60 s at most, `RS12_LINK_HOLD`), then boots with the PCB ID the lobby gives (1 for
the master, 2 for the other), so `resys12_link_id` is not needed. The cabinet identity is kept in `sys12_guid.txt`.

## 6. Building from Source

Builds are done on Linux (Ubuntu 24.04 is used for development).

```bash
sudo apt install -y build-essential git python3 zlib1g-dev
make -C libretro                          # native (x86_64: lightrec recompiler; LIGHTREC=0 for the interpreter)
make -C libretro CC=aarch64-linux-gnu-gcc # Raspberry Pi 5, 64-bit OS (ari64 recompiler); needs an aarch64 zlib,
                                          # e.g. LDFLAGS=-L<dir with libz.so>
```

Headless regression runs (RetroArch null drivers, scripted input, picture hashes): `libretro/tests/headless.py`
(`--update` records the expected hashes; romset dirs in `RS12_ROMS` and `RS11_ROMS`, default `~/system12` and
`~/system11`).

## 7. Project Layout

```
src/psx/       pcsx_rearmed subset: R3000A, ari64 recompiler, lightrec glue, GTE, gpulib, gpu_neon, System 12 shim
src/s12/       System 12 board: memory map, KC0xx protection, ROM DMA, EEPROM, RTC, JVS I/O, CD-XA board, save states
src/s11/       System 11 board: shared RAM, KEYCUS protection, EEPROM, ROM banks, gun board, C76 interface
src/m37710/    M37702 / M37710 interpreter (System 11 C76 sound / I/O MCU)
src/h8/        H8/3002 sound / I/O CPU
src/sh2/       SH-2 (System 12 CD-XA board)
src/sound/     C352
src/romload/   MAME set tables (System 12, System 11), zip reader, ROM cache
src/sys/       executable memory for the recompilers
deps/          lightrec + GNU lightning (x86_64 recompiler), libchdr (CHD discs)
libretro/      libretro core (Linux x86_64 / ARM / aarch64, e.g. Raspberry Pi 5) and its headless tests
tools/         table generators (System 11 sets from MAME namcos11.cpp)
```

## 8. Accuracy and Testing

- `libretro/tests/headless.py` runs every System 12 set and the System 11 sets with a hash file through RetroArch
  (null drivers, scripted input) and compares the pictures with the recorded hashes.
- The recompilers are checked against the interpreter: on x86_64 lightrec gives the interpreter's pictures exactly on
  every set; on the Pi 5, ari64 runs are compared with the interpreter's screens, and a lockstep interpreter /
  recompiler compare build exists for divergence hunts.
- Changes that alter the hashes on purpose (idle-loop skipping, relaxed synchronization) are checked against reference
  screenshots, so that the game reaches the same scenes with the same picture and sound.

## 9. License and Credits

**License.** ReSys12 is distributed under the [GNU General Public License, version 3](LICENSE).
- The pcsx_rearmed code and the ReSys12 code are GPL-2.0-or-later (pcsx_rearmed's license text is in `src/psx/COPYING`).
- The MAME parts (System 12 and System 11 boards, H8/3002, M37710, C352) are BSD-3-Clause.
- lightrec is LGPL-2.1-or-later, GNU lightning LGPL-3.0-or-later, libchdr BSD-3-Clause.

The original copyright headers are kept in all imported sources.

**Credits**
- [pcsx_rearmed](https://github.com/notaz/pcsx_rearmed) (notaz, Ari64 and contributors): R3000A, ari64 recompiler,
  GTE, gpulib and gpu_neon.
- [MAME](https://www.mamedev.org/) developers: System 12 and System 11 boards, H8/3002, M37710, C352, AT28C16, ROM
  definitions and hardware research.
- [lightrec](https://github.com/pcercuei/lightrec) (Paul Cercueil) and [GNU lightning](https://www.gnu.org/software/lightning/):
  the x86_64 recompiler. [libchdr](https://github.com/rtissera/libchdr): CHD disc images.
- [JUnzip](https://github.com/jokkebk/JUnzip), [zlib](https://zlib.net/).

**Trademarks.** Namco, System 11, System 12 and all game titles are trademarks of their respective owners. This project
is not affiliated with or endorsed by Bandai Namco or any rights holder. No ROM images, game graphics or logos are
included in this repository.

## 10. Support

ReSys12 is free software. If you want to support its development, you can do so on
[Patreon](https://www.patreon.com/Reyanon) or [Ko-Fi](https://ko-fi.com/reyanon). Support is for the emulator work only.

**ROMs are never provided, to anyone, including supporters.** Do not ask for them.
