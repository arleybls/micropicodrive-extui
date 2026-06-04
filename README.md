# MicroPicoDrive — Extended(Enhanced) UI Firmware

An extended firmware for the [MicroPicoDrive](https://github.com/gusmanb/micropicodrive) project by gusmanb, adding a richer file browser experience, auto-load configuration, and tactile feedback on top of the original MicroDrive emulation core.

---

## Vision

The original MicroPicoDrive project delivers excellent MicroDrive emulation for the Sinclair QL on a Raspberry Pi Pico. This firmware builds on that foundation without touching the real-time emulation core — all enhancements are purely software changes confined to the UI layer, keeping the timing-critical MicroDrive behaviour intact and the hardware requirements identical to the original project.

The goal is a more polished day-to-day experience: pick up the device, have your favourite image load automatically, browse only the files that matter, tag a new autoload with a single long press, and feel confirmation through an optional vibration motor — all without needing a computer.

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

## Button Reference

| Button | Short press | Long press (hold 500 ms) |
|---|---|---|
| **BACK** | Scroll list up / go to parent folder | Scroll list fast |
| **NEXT** | Scroll list down | Scroll list fast |
| **SELECT** | Load selected file | Tag / untag file as autoload |
| **SELECT** *(in Ready state)* | Save image to SD card | — |
| **BACK** *(in Ready state)* | Eject and return to browser | — |

> **BACK at the top of the root directory is ignored** — prevents accidentally leaving the browser.

---

## SD Card Setup

1. Format the SD card as FAT32
2. Copy `CONFIG.CFG` from this repository to the **root** of the SD card
3. Copy your `.MDV` and `.MPD` image files (subdirectories are supported)
4. Insert and power on

To set an autoload image from the device: browse to the file and long-press SELECT until the `*` appears.

---

## Building

Uses the standard Raspberry Pi Pico SDK (1.5.1 or 2.x). Open in VS Code with the Raspberry Pi Pico extension, set Build Type to **Release**, and build.

---

## Credits

MicroDrive emulation core by [gusmanb](https://github.com/gusmanb/micropicodrive).  
