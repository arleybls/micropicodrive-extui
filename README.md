# MicroPicoDrive — Extended(Enhanced) UI Firmware

An extended firmware for the [MicroPicoDrive](https://github.com/gusmanb/micropicodrive) project by gusmanb, adding a richer file browser experience, auto-load configuration, and tactile feedback on top of the original MicroDrive emulation core.

---

## Vision

The original MicroPicoDrive project delivers excellent MicroDrive emulation for the Sinclair QL on a Raspberry Pi Pico. This firmware builds on that foundation without touching the real-time emulation core — all enhancements are purely software changes confined to the UI layer, keeping the timing-critical MicroDrive behaviour intact and the hardware requirements identical to the original project.

The goal is a more polished day-to-day experience.

---

## New Features

---

### 1. Auto-Load via `CONFIG.CFG`

On power-up, the firmware reads a `CONFIG.CFG` file from the root of the SD card. If a valid `FILE=` entry is present, the named image is loaded automatically and the device goes straight to the **Ready** state — no browsing required.

> **Compatibility:** The `CONFIG.CFG` auto-load feature is compatible with the [Silveriomrs fork](https://github.com/Silveriomrs/micropicodrive) of MicroPicoDrive. A `CONFIG.CFG` file created or tagged by either firmware will be recognised by the other.

**CONFIG.CFG format:**
```
FILE=/GAMES/CHESS.MDV
```

- The path is relative to the SD card root and must start with `/`
- Subdirectory paths are supported (e.g. `FILE=/SUBDIR/IMAGE.MDV`)
- If the file is absent, or `FILE=` is empty, the browser opens normally
- Supported formats: `.MDV` and `.MPD`

> **Note:** `CONFIG.CFG` must be pre-created on the SD card with a minimum size of **306 bytes** before the long-press tag feature can write to it. A ready-made blank `CONFIG.CFG` is included in this repository — copy it to the root of your SD card.

<!-- PICTURE: Boot sequence showing auto-load jumping straight to Ready screen -->

---

### 2. Filtered File Browser

The file browser only shows `.MDV` and `.MPD` image files. All other file types are hidden, keeping the list clean regardless of what else is on the SD card. Directories are shown normally for navigation.

<!-- PICTURE: File browser showing only MDV/MPD files -->

---

### 3. Long-Press to Tag / Untag Autoload File

While browsing, **hold SELECT** on any image file for 500 ms. The asterisk (`*`) appears on the filename as visual confirmation that the threshold has been reached — release the button at that point.

**Tagging** writes the file path to `CONFIG.CFG` so it auto-loads on next boot. The `*` prefix appears next to the filename in the browser immediately.

**Untagging** — long-pressing the same already-tagged file (`*` visible) clears the `CONFIG.CFG` entry. The `*` disappears instantly.

| Action | Result |
|---|---|
| Long-press untagged file | `*` appears → release → file tagged as autoload |
| Long-press tagged (`*`) file | `*` disappears → release → autoload cleared |
| Short-press any file | Normal load (tag status unaffected) |
| Long-press a directory | No action |

> **Requirement:** `CONFIG.CFG` must exist on the SD card. If absent, a "No CONFIG" error is shown briefly.

<!-- PICTURE: Browser showing * prefix on tagged file -->
<!-- PICTURE: Long-press mid-hold showing * appearing on screen -->

---

### 4. Image Name on Ready Screen

When a cartridge image is loaded successfully, the OLED shows the image filename instead of the generic "Ready" text. The extension is omitted, and names longer than 10 characters are truncated.

Examples:
- `ABACUS` — short name, shown as-is
- `LONGFILENA..` — truncated

<!-- PICTURE: Ready screen showing image filename -->

---

### 5. Vibration Motor Feedback *(Optional)*

If a [breakout vibration motor module](https://www.amazon.com/s?k=keyes+vibration+motor+module) (or equivalent ERM coin motor breakout with onboard transistor driver) is connected to **GP28**, the motor runs during all SD card activity and stops 500 ms after the activity ends, giving tactile confirmation of reads and writes.

Motor activity is triggered by:
- Directory listing (browsing)
- Loading an MDV or MPD image
- Saving a modified image
- Reading or writing `CONFIG.CFG`

The motor pin is defined as `PIN_MOTOR 28` in `UserInterface.h`. If no motor is connected, the firmware operates normally — driving an unconnected output pin is harmless.

**Wiring:**

| Module pin | Connect to |
|---|---|
| IN | GP28 |
| VCC | VSYS (pin 39) for full voltage, or 3.3V |
| GND | GND |

<!-- PICTURE: Motor module wired to Pico -->

---

### 6. Smooth Vertical Scroll Animation

The file browser uses a pixel-by-pixel vertical scroll animation when navigating the list. Items slide smoothly up or down over 10 frames at 20 ms per frame (200 ms total). A horizontal scroll animation activates on long filenames when the browser is idle.

<!-- PICTURE: Browser scrolling animation (video or multi-frame) -->

---

### 7. Firmware Update from SD Card

Drop a new firmware build (the plain `MicroPicoDrive.uf2` produced by the build — no renaming or repacking needed) into an `UPDATE` folder at the root of the SD card. On the next power-up the device validates the file and shows a confirmation screen:

```
FW update?
cur v1.0.0
new v1.0.1
SEL=Y BK=N
```

| Action | Result |
|---|---|
| **SELECT** | Install and reboot into the new firmware |
| **BACK** | Decline — this exact build is remembered and never offered again |
| No press for 30 s | Nothing happens; the update is offered again on the next power-up |

While installing, the screen shows a progress bar, then **"do not power off"** during the final flash write.

Notes:

- The file is fully verified (CRC-32, board family, integrity) **before** anything is written over the running firmware. Corrupt, truncated or wrong-board files are rejected harmlessly.
- After a successful update the `.uf2` stays on the card and is ignored automatically — it matches the running firmware. You can delete it from a PC at leisure.
- Downgrades are allowed; the prompt marks them as `old vX.Y.Z` instead of `new`.
- The `UPDATE` folder is hidden from the file browser.
- The check runs only at power-on, and never while a cartridge is mounted.

> **Warning:** if power is lost during the short *"do not power off"* phase (a couple of seconds), the device will not boot and must be reflashed once over USB (hold BOOTSEL while plugging in, copy the `.uf2`). Losing power at any other point of the update is harmless.

<!-- PICTURE: Update confirmation screen -->

---

## Button Reference

| Button | Short press | Long press (hold 500 ms) |
|---|---|---|
| **BACK** | Scroll list up / go to parent folder | Scroll list fast |
| **NEXT** | Scroll list down | Scroll list fast |
| **SELECT** | Load selected file | Tag / untag file as autoload |
| **SELECT** *(in Ready state)* | Save image to SD card | — |
| **BACK / NEXT** *(in Ready state)* | Eject and return to browser | — |

> **BACK at the top of the root directory is ignored** — prevents accidentally leaving the browser.

---

## SD Card Setup

1. Format the SD card as FAT32
2. Copy `CONFIG.CFG` from this repository to the **root** of the SD card
3. Copy your `.MDV` and `.MPD` image files (subdirectories are supported)
4. Insert and power on

To set an autoload image from the device: browse to the file and long-press SELECT until the `*` appears.

To update the firmware: create an `UPDATE` folder at the root of the SD card and copy the new `MicroPicoDrive.uf2` into it (see [Firmware Update from SD Card](#7-firmware-update-from-sd-card)).

---

## Building

Uses the standard Raspberry Pi Pico SDK (1.5.1 or 2.x). Open in VS Code with the Raspberry Pi Pico extension, set Build Type to **Release**, and build.

---

## Roadmap / TODO

Candidates identified by comparing this firmware against [micropicodrive-ng-firmware](https://github.com/arleybls/micropicodrive-ng-firmware). Line references are to this repo unless prefixed with `NG:`.

### Bugs / safety (small diffs, do these first)

- [ ] **`event_push` blocks inside IRQs.** `EventMachine.c:13` uses `queue_add_blocking`; all producers are interrupt handlers (`MicroDriveControl.c:401,417,697,714` plus the DMA IRQs). A full queue hangs in interrupt context. Fix: `queue_try_add` + sticky `overflow` flag (NG: `src/drive/EventMachine.c:14-18`).
- [ ] **Raise queue depths** from 16/8/8 (`MicroDriveControl.c:1111-1112`, `UserInterface.c:1342`) to NG's 32/16/16.
- [ ] **"Event lost" state.** Surface the overflow flag: with a cartridge mounted the RAM image is untrusted, so force a screen where eject is the only live key and save is unreachable (NG: `src/ui/UserInterface.c:2016-2028`).
- [ ] **Eject fires mid-transfer.** `check_cancel()` (`UserInterface.c:644-657`, called at `:1357`) ejects while the QL is actively using the drive. Delete it and freeze buttons while `mdInUse` (NG: `:2032-2037`).
- [ ] **Bogus DMA abort.** `MicroDriveControl.c:821-828` passes a transfer *count* to `dma_channel_abort()` as a channel index, aborting an arbitrary channel on every header read. Dead debug code — delete.
- [ ] **`event_clear` races.** `EventMachine.c:28-32` writes `queue.wptr`/`rptr` directly, bypassing the SDK spinlock. Drain with `queue_try_remove`. Currently unused, but it's a landmine.

### Data-loss guards

- [ ] **Dirty flag + eject confirm.** Track QL writes and ask before ejecting a modified cartridge. Today one BACK/NEXT press in `CARTRIDGE_READY` (`UserInterface.c:1062-1071`) discards everything silently. (NG: `:630`, `:1561`, `:1659`, `:1587-1596`)
- [ ] **Save-failure messaging.** "Save failed / Retry save!" and keep the dirty flag, instead of the current bare "Save error" (`:1101-1105`).
- [ ] **Card-swap save guard** — refuse SAVE if the card changed since load (NG: `:1614-1631`). Blocked on full FatFs (Petit FatFs has no `f_getlabel`).

### Browser polish

- [ ] **Empty-root infinite loop.** `:858-870` cycles Empty dir → remount → Empty dir forever. Add a persistent no-files screen (NG: `:1359-1362`).
- [ ] **Truncation marker.** Both firmwares cap listings at 64 entries; append a non-selectable `...` row when entries were dropped (NG: `:1049-1056`, `:1303-1322`).
- [ ] **Hidden-entry filtering.** Skip `AM_HID|AM_SYS` and all dot-prefixed names (NG: `:1288-1289`) instead of the hard-coded `UPDATE` special case (`:793-795`). Rename the update folder to `.update` to match.
- [ ] **Case-insensitive extension filter** — `strcasecmp` (NG: `:1294`) vs the current `strcmp` (`:798-801`).

### Larger projects

- [ ] **Petit FatFs → full FatFs + carlk3 no-OS-FatFS.** Unblocks: creating `CONFIG.CFG` on demand (no more pre-created 306-byte file), deleting the declined `.uf2` (drops the flash decline record in `flash_layout.h:36-41`), long filenames, volume serial, and any future save-as. Wiring is identical to NG (`pff/pffconf.h:62-67` matches NG `src/storage/sd_hw_config.c`), and RAM fits — about 175 KB used of 264 KB, while NG's Lite build runs full FatFs at 217 KB.
- [ ] **Flashloader-based update.** Replace the in-place rewrite (`sd_update.c:273-294`, the "do not power off" window) with stage → commit header → reboot → idempotent first-stage apply (NG: `src/flashloader/flashloader.c`). Costs a second binary, a custom linker script, and a one-time BOOTSEL migration for existing users.
  - [ ] Cheap subset, no flashloader needed: stale-stage invalidation after an external reflash (NG: `sd_update_lite.c:232-239`) and ignoring out-of-app-range UF2 blocks (NG: `:116-123`).

### Nice to have

- [ ] **System Info screen** (NG: `src/ui/sys_info.c`) — version, chip, clock, temperature, RAM/flash use. There is currently no way to see the running firmware version.
- [ ] **SD Check** (NG: `src/storage/sd_check.c`) — card info plus a write/read/verify pass.
- [ ] **SD error-burst detector** — treat N consecutive failures as a surprise card removal (NG: `:63`, `:290-291`).
- [ ] **Persistent settings in flash** (NG: `:1940-1989`) — the pattern to reuse if any user-facing option is added.
- [ ] Both tools above are reached by a **3 s long press** rather than a dedicated button — this board has three buttons where NG has a fourth (K4). The gesture must stay distinct from the existing 500 ms long-press tag on SELECT.

---

## Credits

MicroDrive emulation core by [gusmanb](https://github.com/gusmanb/micropicodrive).  
