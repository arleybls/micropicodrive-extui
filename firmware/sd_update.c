// SD-card firmware update (boot-time, from /UPDATE). See sd_update.h.
//
// Single-binary flavor (no flashloader): the UF2 is validated and CRC'd in a
// scan pass, staged into the upper-flash staging region, verified, and — after
// the user confirms — applied by a never-returning RAM routine that rewrites
// the app region with bootrom ROM functions and reboots. Interrupted staging
// is harmless (the old firmware just boots); power loss during the short
// apply window bricks until a BOOTSEL reflash (accepted trade-off).
//
// Called from INIT_SCREEN with the display up and no cartridge loaded, so
// the flash_safe_execute writes are safe (core 0 opted into lockout in main).
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "hardware/regs/addressmap.h"
#include "pico/binary_info/defs.h"
#include "pico/binary_info/structure.h"
#include "ssd1306/ssd1306.h"
#include "pff/pff.h"
#include "UserInterface.h"
#include "flash_layout.h"
#include "sd_update.h"

#define UPD_DIR "/UPDATE"            // 8.3-safe: Petit FatFs has no LFN
#define CONFIRM_TIMEOUT_MS 30000

#ifndef MPD_FW_VERSION_MAJOR
#define MPD_FW_VERSION_MAJOR 1
#endif
#ifndef MPD_FW_VERSION_MINOR
#define MPD_FW_VERSION_MINOR 0
#endif
#ifndef MPD_FW_VERSION_PATCH
#define MPD_FW_VERSION_PATCH 0
#endif

// UF2 block layout (512 bytes) — https://github.com/microsoft/uf2
#define UF2_MAGIC_START0 0x0A324655u
#define UF2_MAGIC_START1 0x9E5D5157u
#define UF2_MAGIC_END    0x0AB16F30u
#define UF2_FLAG_FAMILY  0x00002000u
#define UF2_FAMILY_RP2040 0xE48BFF56u

typedef struct {
    uint32_t magic_start0, magic_start1, flags, target_addr;
    uint32_t payload_size, block_no, num_blocks, family_id;
    uint8_t  data[476];
    uint32_t magic_end;
} uf2_block_t;

// Globals owned by UserInterface.c
extern ssd1306_t disp;
extern FATFS fatfs;
extern bool mdInUse;
extern CARTRIDGE_FORMAT cfInserted;

// Static, not stack: core 1 runs on the 2 KB default stack.
static uf2_block_t s_block;
static uint8_t     s_sector[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));

// --- 64x32 screens: 4 lines of up to 12 chars (5 px pitch, 8 px rows) ---

static void scr4(const char *l1, const char *l2, const char *l3, const char *l4) {
    ssd1306_clear(&disp);
    if (l1) ssd1306_draw_string(&disp, 0, 0, 1, l1);
    if (l2) ssd1306_draw_string(&disp, 0, 8, 1, l2);
    if (l3) ssd1306_draw_string(&disp, 0, 16, 1, l3);
    if (l4) ssd1306_draw_string(&disp, 0, 24, 1, l4);
    ssd1306_show(&disp);
}

static void msg(const char *l2, const char *l3, uint32_t ms) {
    scr4("FW update", l2, l3, NULL);
    if (ms) sleep_ms(ms);
}

static void progress(int pct) {
    char line[13];
    snprintf(line, sizeof(line), "staging %d%%", pct);
    ssd1306_clear(&disp);
    ssd1306_draw_string(&disp, 0, 0, 1, "FW update");
    ssd1306_draw_string(&disp, 0, 8, 1, line);
    ssd1306_draw_empty_square(&disp, 0, 20, 63, 8);
    if (pct > 0)
        ssd1306_draw_square(&disp, 2, 22, (uint32_t)pct * 60u / 100u, 5);
    ssd1306_show(&disp);
}

// SELECT = yes (1), BACK = no (0), 30 s without a press = defer (-1).
static int confirm_update(const char *cur, const char *neu) {
    scr4("FW update?", cur, neu, "SEL=Y BK=N");
    absolute_time_t deadline = make_timeout_time_ms(CONFIRM_TIMEOUT_MS);
    while (!time_reached(deadline)) {
        if (BUTTON_PRESSED(PIN_BTN_SELECT)) { debounce_button(PIN_BTN_SELECT); return 1; }
        if (BUTTON_PRESSED(PIN_BTN_BACK))   { debounce_button(PIN_BTN_BACK);   return 0; }
        sleep_ms(10);
    }
    return -1;
}

// flash_safe_execute wrappers (multicore lockout — core 0 opted in at boot).
// Return 0 on success, mirroring "if (op()) fail" call sites.
typedef struct { uint32_t off; const uint8_t *data; uint32_t len; } flash_op_t;

static void cb_erase(void *param) {
    const flash_op_t *op = (const flash_op_t *)param;
    flash_range_erase(op->off, op->len);
}

static void cb_program(void *param) {
    const flash_op_t *op = (const flash_op_t *)param;
    flash_range_program(op->off, op->data, op->len);
}

static int flash_safe_execute_erase(uint32_t off, uint32_t len) {
    flash_op_t op = { off, NULL, len };
    return flash_safe_execute(cb_erase, &op, 100) != PICO_OK;
}

static int flash_safe_execute_program(uint32_t off, const uint8_t *data, uint32_t len) {
    flash_op_t op = { off, data, len };
    return flash_safe_execute(cb_program, &op, 100) != PICO_OK;
}

// One scan/stage pass over the UF2. Strict: every block must be a 256-byte
// app payload, contiguous from XIP_BASE, with matching block numbering — a
// merged/foreign UF2 (e.g. an image linked at a different address) is
// rejected outright rather than skipped, since applying it here would brick.
// Returns the image size in bytes (0 on error, message already shown) and
// the CRC in *crc. When `stage` is set, payloads are streamed into the
// staging region (which must already be erased).
static uint32_t uf2_pass(uint32_t *crc, bool stage, uint32_t total_size) {
    uint32_t size = 0;
    uint32_t sect_fill = 0;
    uint32_t last_nblk = 0;
    int last_pct = -1;
    *crc = 0xFFFFFFFFu;

    for (;;) {
        UINT n = 0;
        if (pf_read(&s_block, sizeof(s_block), &n) != FR_OK) {
            msg("read error", "aborted", 2500);
            return 0;
        }
        if (n == 0) break;                       // EOF
        if (n != sizeof(s_block) ||
            s_block.magic_start0 != UF2_MAGIC_START0 ||
            s_block.magic_start1 != UF2_MAGIC_START1 ||
            s_block.magic_end != UF2_MAGIC_END) {
            msg("not a UF2", "", 2500);
            return 0;
        }
        if (!(s_block.flags & UF2_FLAG_FAMILY) ||
            s_block.family_id != UF2_FAMILY_RP2040) {
            msg("wrong board", "not RP2040", 2500);
            return 0;
        }
        if (s_block.payload_size != 256u ||
            s_block.target_addr != XIP_BASE + MPD_APP_OFFSET + size ||
            s_block.block_no != size / 256u ||
            size + 256u > MPD_APP_MAX_SIZE || size + 256u > MPD_STAGE_MAX_SIZE) {
            msg("bad UF2", "", 2500);
            return 0;
        }
        last_nblk = s_block.num_blocks;

        *crc = fl_crc32_update(*crc, s_block.data, 256u);
        size += 256u;

        if (stage) {
            memcpy(&s_sector[sect_fill], s_block.data, 256u);
            sect_fill += 256u;
            if (sect_fill == FLASH_SECTOR_SIZE) {
                if (flash_safe_execute_program(MPD_STAGE_IMG_OFFSET + size - sect_fill,
                                               s_sector, FLASH_SECTOR_SIZE)) {
                    msg("write error", "aborted", 2500);
                    return 0;
                }
                sect_fill = 0;
            }
            int pct = (int)((uint64_t)size * 100 / total_size);
            if (pct != last_pct) {
                last_pct = pct;
                progress(pct);
            }
        }
    }

    if (stage && sect_fill) {                    // final partial sector
        memset(&s_sector[sect_fill], 0xFF, FLASH_SECTOR_SIZE - sect_fill);
        if (flash_safe_execute_program(MPD_STAGE_IMG_OFFSET + size - sect_fill,
                                       s_sector, FLASH_SECTOR_SIZE)) {
            msg("write error", "aborted", 2500);
            return 0;
        }
    }

    if (size < 256u) {
        msg("empty file", "", 2500);
        return 0;
    }
    // Truncation defense: a file cut at a block boundary would otherwise
    // self-verify (the CRC is computed from what we read). The per-block
    // numbering check above made blocks 0..N-1 contiguous; EOF must have
    // consumed exactly num_blocks of them.
    if (size / 256u != last_nblk) {
        msg("truncated", "", 2500);
        return 0;
    }
    *crc = ~*crc;
    return size;
}

// Translate a runtime app address into the staged copy of the image.
static const void *staged_ptr(uint32_t addr, uint32_t size) {
    if (addr < XIP_BASE + MPD_APP_OFFSET || addr >= XIP_BASE + MPD_APP_OFFSET + size)
        return NULL;
    return (const void *)(XIP_BASE + MPD_STAGE_IMG_OFFSET + (addr - XIP_BASE - MPD_APP_OFFSET));
}

// The pico_set_program_version string embedded in the staged image's binary
// info (what picotool shows as "version"). NULL when absent.
static const char *staged_version_string(uint32_t size) {
    const uint32_t *img = (const uint32_t *)(XIP_BASE + MPD_STAGE_IMG_OFFSET);
    uint32_t search_words = (size < 1024u ? size : 1024u) / 4u;
    // Marker block sits early in .text, just after the vector table.
    for (uint32_t i = 0; i + 4 < search_words; i++) {
        if (img[i] != BINARY_INFO_MARKER_START || img[i + 4] != BINARY_INFO_MARKER_END)
            continue;
        const uint32_t *entry     = staged_ptr(img[i + 1], size);
        const uint32_t *entry_end = staged_ptr(img[i + 2], size);
        if (!entry || !entry_end) return NULL;
        for (; entry < entry_end; entry++) {
            const binary_info_id_and_string_t *bi = staged_ptr(*entry, size);
            if (bi && bi->core.type == BINARY_INFO_TYPE_ID_AND_STRING &&
                bi->core.tag == BINARY_INFO_TAG_RASPBERRY_PI &&
                bi->id == BINARY_INFO_ID_RP_PROGRAM_VERSION_STRING)
                return (const char *)staged_ptr((uint32_t)(uintptr_t)bi->value, size);
        }
        return NULL;
    }
    return NULL;
}

// --- Apply: rewrite the app region from the staged copy and reboot ---
//
// The running code lives in the region being erased, so this must be one
// never-returning RAM routine that calls bootrom ROM functions only (the
// SDK's flash_range_erase/program and anything else flash-resident is gone
// after the first sectors). Entered via flash_safe_execute: core 0 is parked
// in its RAM lockout handler, IRQs on this core are off. Between the ROM
// erase/program calls XIP is re-entered so the next staged sector can be
// read. A watchdog armed beforehand (and re-fed per sector from RAM) covers
// a hang; the final reboot is a direct register write — watchdog_reboot()
// itself is flash-resident and by then flash holds the new image.

typedef struct {
    rom_connect_internal_flash_fn connect;
    rom_flash_exit_xip_fn         exit_xip;
    rom_flash_range_erase_fn      range_erase;
    rom_flash_range_program_fn    range_program;
    rom_flash_flush_cache_fn      flush_cache;
    rom_flash_enter_cmd_xip_fn    enter_cmd_xip;
    uint32_t size;
} apply_args_t;

static void __no_inline_not_in_flash_func(apply_cb)(void *param) {
    const apply_args_t *a = (const apply_args_t *)param;
    uint32_t nsect = (a->size + FLASH_SECTOR_SIZE - 1u) / FLASH_SECTOR_SIZE;
    for (uint32_t s = 0; s < nsect; s++) {
        uint32_t off = s * FLASH_SECTOR_SIZE;
        // Buffer the staged sector in RAM (word copy — no library memcpy):
        // flash is unreadable during the ROM erase/program calls below.
        const uint32_t *src = (const uint32_t *)(XIP_BASE + MPD_STAGE_IMG_OFFSET + off);
        uint32_t *dst = (uint32_t *)s_sector;
        for (uint32_t i = 0; i < FLASH_SECTOR_SIZE / 4u; i++)
            dst[i] = src[i];
        watchdog_hw->load = 0x7FFFFFu;           // re-feed: ~4 s per sector allowed
        a->connect();
        a->exit_xip();
        a->range_erase(MPD_APP_OFFSET + off, FLASH_SECTOR_SIZE, 1u << 16, 0xD8);
        a->range_program(MPD_APP_OFFSET + off, s_sector, FLASH_SECTOR_SIZE);
        a->flush_cache();
        a->enter_cmd_xip();
    }
    watchdog_hw->ctrl = WATCHDOG_CTRL_TRIGGER_BITS;
    while (true) { }
}

void sd_update_check_at_boot(void) {
    // Hard rule: no flash writes with a cartridge mounted or the drive in
    // use. Power-on always arrives here ejected, but the UI-reconnect path
    // (CARTRIDGE_READY -> IDLE -> INIT_SCREEN on a PIN_UI_DETECT glitch)
    // re-enters WITH the cartridge loaded — a lockout or reboot then would
    // corrupt the live QL session. The latch keeps reconnects prompt-free.
    if (cfInserted != NONE || mdInUse)
        return;
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;

    if (pf_mount(&fatfs) != FR_OK) return;       // no card — WAITING_SD_CARD re-mounts later

    // First *.UF2 in /UPDATE (SFN uppercase, 8.3)
    DIR udir;
    FILINFO fi;
    char path[24] = {0};
    if (pf_opendir(&udir, UPD_DIR) != FR_OK) return;   // no UPDATE folder
    gpio_put(PIN_MOTOR, 1);
    while (pf_readdir(&udir, &fi) == FR_OK && fi.fname[0]) {
        size_t l = strlen(fi.fname);
        if (!(fi.fattrib & AM_DIR) && l > 4 &&
            strcasecmp(fi.fname + l - 4, ".UF2") == 0) {
            snprintf(path, sizeof(path), UPD_DIR "/%s", fi.fname);
            break;
        }
    }
    if (!path[0]) { gpio_put(PIN_MOTOR, 0); return; }

    if (pf_open(path) != FR_OK) {
        gpio_put(PIN_MOTOR, 0);
        msg("open error", "", 2000);
        return;
    }

    // Scan pass: validate + size + CRC, no writes. Takes a second or two —
    // say so instead of leaving the fresh display blank.
    msg("checking..", "", 0);
    uint32_t crc = 0;
    uint32_t size = uf2_pass(&crc, false, 0);
    gpio_put(PIN_MOTOR, 0);
    if (!size) return;

    // Identical to the running app (typically right after an update, with
    // the file still on the card — Petit FatFs cannot delete it): inert.
    if (crc == fl_crc32((const uint8_t *)(XIP_BASE + MPD_APP_OFFSET), size))
        return;

    // Previously declined: also inert (only the latest decline sticks).
    const mpd_decline_rec_t *dr = (const mpd_decline_rec_t *)(XIP_BASE + MPD_STAGE_HDR_OFFSET);
    if (dr->magic == MPD_DECLINE_MAGIC && dr->inv_crc32 == ~dr->crc32 &&
        dr->size == size && dr->crc32 == crc)
        return;

    // Stage BEFORE asking: having the image in flash lets the confirm screen
    // show its embedded version, and a timed-out prompt (file left on card)
    // reuses the staged copy next boot — near-instant re-offer, no wear.
    if (fl_crc32((const uint8_t *)(XIP_BASE + MPD_STAGE_IMG_OFFSET), size) != crc) {
        // Erase header + image sectors, one sector per lockout so core 0's
        // blackout stays bounded. (The header sector may hold an older
        // decline record for a different image — superseded by design.)
        uint32_t img_sectors = (size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
        msg("erasing..", "", 0);
        for (uint32_t s = 0; s <= img_sectors; s++) {
            if (flash_safe_execute_erase(MPD_STAGE_HDR_OFFSET + s * FLASH_SECTOR_SIZE,
                                         FLASH_SECTOR_SIZE)) {
                msg("erase error", "aborted", 2500);
                return;
            }
        }

        // Stage pass: stream payloads into the staging region. Note: the
        // md-to-ui event queue (depth 8) is not drained during this — fine,
        // the guard above guarantees no cartridge is generating events.
        if (pf_lseek(0) != FR_OK) { msg("read error", "aborted", 2500); return; }
        gpio_put(PIN_MOTOR, 1);
        uint32_t crc2 = 0;
        uint32_t size2 = uf2_pass(&crc2, true, size);
        gpio_put(PIN_MOTOR, 0);
        if (!size2) return;

        // Verify what actually landed in flash before offering it.
        if (size2 != size || crc2 != crc ||
            fl_crc32((const uint8_t *)(XIP_BASE + MPD_STAGE_IMG_OFFSET), size) != crc) {
            msg("verify fail", "aborted", 2500);
            return;
        }
    }

    // Sanity: a real app image has its vector table (initial SP in SRAM)
    // right after boot2, at +0x100.
    uint32_t staged_msp = *(const uint32_t *)(XIP_BASE + MPD_STAGE_IMG_OFFSET + 0x100u);
    if ((staged_msp >> 28) != 0x2u) {
        msg("bad image", "aborted", 2500);
        return;
    }

    // Ask (default: defer). "old" instead of "new" marks a downgrade.
    const char *newver = staged_version_string(size);
    char cur[13], neu[13];
    snprintf(cur, sizeof(cur), "cur v%u.%u.%u",
             MPD_FW_VERSION_MAJOR, MPD_FW_VERSION_MINOR, MPD_FW_VERSION_PATCH);
    unsigned nmaj, nmin, npat;
    bool older = false;
    if (newver && sscanf(newver, "%u.%u.%u", &nmaj, &nmin, &npat) == 3) {
        uint32_t cur_key = ((uint32_t)MPD_FW_VERSION_MAJOR << 20) |
                           ((uint32_t)MPD_FW_VERSION_MINOR << 10) |
                            (uint32_t)MPD_FW_VERSION_PATCH;
        uint32_t new_key = (nmaj << 20) | (nmin << 10) | npat;
        older = new_key < cur_key;
    }
    if (newver) snprintf(neu, sizeof(neu), "%s v%.6s", older ? "old" : "new", newver);
    else        snprintf(neu, sizeof(neu), "new v?");

    int ans = confirm_update(cur, neu);
    if (ans == 0) {
        // Declining sticks: remember this image's CRC in the header sector.
        mpd_decline_rec_t rec = { MPD_DECLINE_MAGIC, size, crc, ~crc };
        memset(s_sector, 0xFF, FLASH_PAGE_SIZE);
        memcpy(s_sector, &rec, sizeof(rec));
        if (flash_safe_execute_erase(MPD_STAGE_HDR_OFFSET, FLASH_SECTOR_SIZE) ||
            flash_safe_execute_program(MPD_STAGE_HDR_OFFSET, s_sector, FLASH_PAGE_SIZE)) {
            msg("flash error", "", 2000);
            return;
        }
        msg("declined", "", 2000);
        return;
    }
    if (ans != 1)
        return;   // timeout: not a decline — offer again next boot

    apply_args_t args = {
        .connect       = (rom_connect_internal_flash_fn)rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH),
        .exit_xip      = (rom_flash_exit_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP),
        .range_erase   = (rom_flash_range_erase_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE),
        .range_program = (rom_flash_range_program_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_PROGRAM),
        .flush_cache   = (rom_flash_flush_cache_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE),
        .enter_cmd_xip = (rom_flash_enter_cmd_xip_fn)rom_func_lookup_inline(ROM_FUNC_FLASH_ENTER_CMD_XIP),
        .size = size,
    };
    if (!args.connect || !args.exit_xip || !args.range_erase ||
        !args.range_program || !args.flush_cache || !args.enter_cmd_xip) {
        msg("ROM error", "aborted", 2500);
        return;
    }

    scr4("FW update", "flashing..", "do not", "power off");
    // Safety net: if the apply hangs (or the lockout below fails), this
    // reboots — into the old app if nothing was erased yet. Also configures
    // the watchdog for a full system reset, which the RAM routine triggers
    // directly when done.
    watchdog_reboot(0, 0, 4000);
    flash_safe_execute(apply_cb, &args, 500);
    while (true) tight_loop_contents();          // unreachable; watchdog covers it
}
