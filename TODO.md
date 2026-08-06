# Port TODO — technical notes

Detailed backing for the checklist in [README.md](README.md#roadmap--todo).
Candidates identified by comparing this firmware against
[micropicodrive-ng-firmware](https://github.com/arleybls/micropicodrive-ng-firmware) (NG).

Line references are to this repo's `firmware/` unless prefixed with `NG:`,
which refers to NG's `src/`.

## Background

Both firmwares descend from gusmanb's original. The microdrive emulation core is
still byte-identical between them — `MicroDriveControl.c`, `PIO_machines.pio`,
`SharedBuffers.*` and `SharedEvents.h` differ only by three deliberate NG
changes (listed below). NG then diverged on hardware: ST7735 colour panel, full
FatFs, RP2350/Pico 2 W target, BLE.

That splits the port work three ways: shared-core fixes (free), display-agnostic
logic (cheap), and features that need NG's hardware (not portable).

One fact that makes several items easier than expected: **the SD wiring is
identical**. `pff/pffconf.h:62-67` (SPI0, MISO 16 / CS 17 / SCK 18 / MOSI 19)
matches NG's `storage/sd_hw_config.c` exactly.

---

## Bugs / safety

### Non-blocking `event_push`

`EventMachine.c:13` uses `queue_add_blocking`. Every producer is an interrupt
handler — `MicroDriveControl.c:401` and `:417` (alarms), `:697` (`status_irq`),
`:714` (`shifter_irq`), plus the DMA IRQs at `:665` and `:677`. A full queue
means spinning forever in interrupt context.

NG uses `queue_try_add` and raises a sticky `overflow` flag instead
(NG: `drive/EventMachine.c:14-18`, `drive/EventMachine.h:17-19`). Dropping an
event is recoverable; hanging in an IRQ is not.

### Raise event queue depths

Current: `mdEventQueue` 16 and `uiToMdEventQueue` 8 (`MicroDriveControl.c:1111-1112`),
`mdToUiEventQueue` 8 (`UserInterface.c:1342`).
NG: 32 / 16 / 16. Cheap headroom, and it pairs with the overflow flag above.

### "Event lost" state

NG's UI loop checks all three overflow flags each pass
(NG: `ui/UserInterface.c:2016-2028`). Behaviour depends on whether a cartridge
is mounted:

- No cartridge — the QL polls its drives regardless, so the flags are cleared
  silently.
- Cartridge mounted — the RAM image can no longer be trusted. NG forces a screen
  where eject is the only live key and **save is deliberately unreachable**
  (`EVENT_LOST` state, NG: `:1695-1715`).

Without this, a dropped event silently corrupts the image and the user then
saves it over a good file. Needs the overflow flag from the first item.

### Ignore buttons while the drive is in use

`check_cancel()` (`UserInterface.c:644-657`, called from the main loop at
`:1357`) runs *while* `mdInUse` is true and ejects on BACK/NEXT — in the middle
of a QL read or write, discarding everything written since load.

NG deleted `check_cancel` outright and gates the whole UI on `!mdInUse`
(NG: `:2032-2037`). Eject then happens normally in `CARTRIDGE_READY` once the QL
deselects the drive. Their comment calls out this exact failure.

### Delete the bogus `dma_channel_abort`

`MicroDriveControl.c:821-828`:

```c
if(activeStatus == MDA_READ_HEADER)
{
    int32_t transferA = dma_channel_hw_addr(track1DMA)->transfer_count;
    dma_channel_abort(transferA);          // count passed as a channel index
    int32_t transferB = dma_channel_hw_addr(track2DMA)->transfer_count;
    dma_channel_abort(transferB);
    int32_t final = transferA - transferB; // computed, never used
}
```

`dma_channel_abort(ch)` writes `1u << ch` to `dma_hw->abort`. A transfer count of
e.g. 306 shifts out of range — undefined behaviour that masks to channel 18 — so
an arbitrary DMA channel is aborted on every header read. The block is dead
debug code (`final` is unused, and the real aborts follow immediately at
`:830-839`). NG deleted it.

### Fix `event_clear`

`EventMachine.c:28-32` writes `queue.wptr` and `queue.rptr` directly, bypassing
the SDK's spinlock — a race against any IRQ producer. NG drains with
`queue_try_remove` in a loop (NG: `drive/EventMachine.c:32-37`).

Currently unused in this firmware, so it is not an active bug — but NG needed a
working `event_clear` as soon as it added `clear_event_overflows()`.

---

## Data-loss guards

### Dirty flag and eject confirmation

NG sets `cartDirty = true` in `process_md_write` (NG: `:630`) and clears it on
load (NG: `:1561`) and successful save (NG: `:1659`). On eject it shows a Yes/No
confirm when the flag is set (NG: `:1587-1596`); a clean cartridge still ejects
instantly.

Here, any BACK/NEXT press in `CARTRIDGE_READY` (`UserInterface.c:1062-1071`)
ejects silently and discards every QL write since load. Self-contained change —
the only new UI is a two-line confirm on the 64×32 OLED.

### Save-failure message that says to retry

`:1101-1105` shows a bare "Save error" and returns to the ready screen, giving no
hint that the on-card file may now be partially written. NG shows
"Save failed / Retry save!" and keeps the dirty flag set (NG: `:1668-1676`).

The risk here is lower than NG's — `pf_write` updates in place, where NG's
`FA_CREATE_ALWAYS` truncates first — but a failure part-way through still leaves
a half-updated image on the card.

### Card-swap save guard

NG records the FAT volume serial at load time (NG: `:1562-1564`) and re-checks it
against a fresh mount before saving, refusing on mismatch with
"Different card / Save blocked" while keeping the RAM image and its dirty flag
(NG: `:1614-1631`).

**Not portable as-is.** Petit FatFs has no `f_getlabel` and never re-inits the
card, so a swapped card would be written through a stale `FATFS` — the failure
this guard exists to prevent. Implementable by reading the volume ID out of the
boot sector via `disk_readp`, but it belongs with the full-FatFs migration.

---

## Browser polish

### Persistent no-files screen

`UserInterface.c:858-870`: with `dir_count == 0` at root it shows "Empty dir",
sleeps 2 s, goes to `WAITING_SD_CARD`, remounts, returns to `OPEN_FOLDER`, finds
it empty again — forever. NG added dedicated `SHOW_NO_FILES` / `NO_FILES` states
that hold a wait screen instead of looping (NG: `:1359-1362`).

### Truncation marker

Both firmwares cap listings at 64 entries (`MAX_DIR_ITEMS`, `:41`). NG peeks past
the cap for one more storable entry and, if found, appends a non-selectable
`...` row so the truncation is visible (NG: `:1049-1056` and `:1303-1322`).
Entries are dropped in FAT order — before the alphabetical sort — so *which* ones
vanish is arbitrary, which is exactly why it needs to be signalled.

### Hidden-entry filtering

`:793-795` skips `AM_SYS` only, and hides the update folder by matching the
literal name `UPDATE` at root. NG skips `AM_HID | AM_SYS` and every dot-prefixed
name (NG: `:1288-1289`) — which is why its update folder is `.update`, hidden by
the general rule instead of a special case.

Renaming the folder means updating `UPD_DIR` in `sd_update.c:30` and the
instructions in the README.

### Case-insensitive extension filter

`:798-801` compares with `strcmp` against `".MDV"` / `".MPD"`; NG uses
`strcasecmp` (NG: `:1294`). Low impact while Petit FatFs returns uppercase 8.3
names, but it becomes a real bug the moment long filenames arrive.

---

## Larger projects

### Petit FatFs → full FatFs + carlk3 no-OS-FatFS

Petit FatFs cannot create, delete, extend or truncate files. Most of the
awkwardness in the current README traces straight back to that:

- `CONFIG.CFG` must be pre-created at ≥306 bytes and is shipped in this repo
  (README "Auto-Load" note). NG creates it on demand with `FA_CREATE_ALWAYS`.
- The declined-update record has to live in a flash sector
  (`mpd_decline_rec_t`, `flash_layout.h:36-41`) purely because the `.uf2` cannot
  be deleted. NG calls `f_unlink` (NG: `update/sd_update_lite.c:357`).
- 8.3 uppercase names only, against NG's 64-char LFN (`MAX_NAME_LEN`).
- No volume serial, so no card-swap guard.
- Saving can only overwrite in place — no save-as, no new image creation.

Feasibility:

- **Wiring** — identical, see Background. The driver drops in with no hardware
  change.
- **RAM** — statics here are roughly 175 KB of 264 KB (`cartridge_image` 160,140
  + track buffers ~6.2 KB + the updater's 4 KB sector buffer + the rest). NG's
  Lite build carries full FatFs on the same RP2040 at 217,100 bytes. It fits.
- **Caveat** — FatFs wants a heap for long filenames; NG's Lite build reserves
  its spare RAM for exactly that.

Largest item on the list: a vendored library swap plus rewriting every `pf_*`
call site. It unblocks the card-swap guard and most of the README's caveats at
once.

### Flashloader-based update

`sd_update.c:273-294` rewrites the *running* app region from a never-returning
RAM routine, which is why the README carries a "do not power off" warning —
power loss in that window bricks the device until a BOOTSEL reflash.

NG Lite instead stages the image, writes the header page **last** as the commit
marker, reboots, and lets a 16 KB first-stage loader
(NG: `flashloader/flashloader.c`) copy staging over the app. The copy is
idempotent, so an interrupted apply simply re-runs on the next boot, and if no
plausible app is found the loader drops to BOOTSEL (NG: `flashloader.c:69-70`) —
a bad image cannot strand the board.

Costs: a second binary, a custom linker script (NG: `boards/memmap_app.ld`), the
merged-UF2 build step (NG: `tools/build_lite.ps1`, `tools/make_stage_clear.ps1`),
and relocating the app above 16 KB — which means every existing user needs one
BOOTSEL migration, since the current flat UF2 stops being directly flashable.

#### Subset without the flashloader

Two wins from NG's updater that need none of the above:

- **Stale-stage invalidation** (NG: `sd_update_lite.c:232-239`) — if a valid
  staged image differs from the running app, the app was reflashed externally
  (BOOTSEL rollback) or the apply is failing. Either way the stage is stale and
  must be invalidated, or it gets re-applied over the rollback.
- **Ignore out-of-app-range UF2 blocks** (NG: `sd_update_lite.c:116-123`) — lets
  a merged image work as an SD drop instead of being rejected outright.

---

## Nice to have

### System Info screen

NG: `ui/sys_info.c`. Firmware version, chip, clock, on-die temperature
(ADC channel 4), RAM and flash use from the linker-provided extents, board ID.
Rendered as scrollable report lines, so it ports to 64×32 without the colour
panel. Worth noting there is currently **no way at all** to see which firmware
version is running.

### SD Check

NG: `storage/sd_check.c`. Mounts the card, reports card and filesystem info, and
runs a timed 32 KB write/read/verify pass. Needs write support, so it lands with
the full-FatFs migration.

### SD error-burst detector

NG treats `SD_ERR_BURST_LIMIT` consecutive FatFs failures as a probable surprise
removal (NG: `:63`, `:290-291`) and re-evaluates presence rather than looping on
errors. Any successful operation resets the streak; a failed *mount* deliberately
does not count, since an empty slot fails the same way and is a normal waiting
state.

### Persistent settings in flash

NG: `:1940-1989`. A magic-guarded record written with `flash_safe_execute` and
gated on the cartridge being ejected (a flash write stalls XIP, which would
corrupt a live QL session). Nothing to persist here today — this is the pattern
to reuse if any user-facing option is added.

### 3 s long press to reach the tools

NG puts System Tools on a dedicated fourth button (K4). This board has three, so
the two tools above are reached by a 3 s long press instead. The gesture must
stay distinguishable from the existing 500 ms long-press autoload tag on SELECT
(`UserInterfaceExtension.h:9`) — both would live on the same button, so the tag
action needs to fire on release before the 3 s threshold, not at 500 ms.

---

## Licensing

Decide this **before** copying code, not after — it constrains which items can be
lifted from NG directly.

### Where things stand

| Project | Licence | Why |
|---|---|---|
| gusmanb/micropicodrive (upstream) | MIT | original project |
| This repo | *none — no LICENSE file* | never added |
| NG | GPL-3.0 | forced by its vendored ST7735 driver |

NG is GPL-3.0 because its display driver traces back through
[bablokb/pico-st7735](https://github.com/bablokb/pico-st7735) to
gavinlyonsrepo's work, which is GPL-3.0. Copyleft on a linked component makes
the whole combined work GPL-3.0. NG records the chain in its
`src/lib-st7735/LICENSE`.

MIT code may be incorporated into a GPL-3.0 work with the original notice
retained, which is how NG absorbed the upstream emulation core.

### What that means per item

**Copying NG's own code makes this repo a GPL-3.0 derivative** and a `LICENSE`
file becomes mandatory. That covers everything NG authored under `src/ui/`,
`src/update/`, `src/drive/` and `src/storage/` — i.e. most of the Bugs / safety,
Data-loss guards and Browser polish items, and the flashloader.

Note the ST7735 driver itself is never coming across (no colour panel here), so
the *reason* NG is GPL does not apply to this firmware. The obligation would come
from NG's own source files, not from the display driver.

**The full-FatFs migration is licence-neutral.** Its vendored components are
separately licensed and impose no copyleft:

| Component | Licence |
|---|---|
| carlk3 no-OS-FatFS-SD | Apache 2.0 |
| FatFs (ChaN) | own permissive licence, see its `LICENSE.txt` |

Only NG's thin glue around them (`storage/sd_hw_config.c`, the `sd_fs_mount`
wrapper) is NG-authored — and both are short enough to reimplement from the
datasheet and the library's own examples if staying non-copyleft matters.

### Options

1. **Adopt GPL-3.0.** Add a `LICENSE`, port freely from NG. Simplest, and
   consistent with NG being the sibling project.
2. **Stay permissive.** Take the FatFs migration and reimplement the NG-derived
   logic independently. Most of the small fixes are a few lines each and follow
   from the bug rather than from NG's phrasing — but "reimplemented, not copied"
   has to be true in fact, not just claimed.

Either way, **add a `LICENSE` file**. An unlicensed public repo grants no rights
to anyone, including people who want to contribute back.
