// Flash map for the SD-card firmware update (sd_update.c).
//
//   0x000000 - 0x100000  app (boot2 + firmware, linked at XIP_BASE by the
//                        default linker script — the plain build UF2)
//   0x100000 - 0x101000  header sector: decline record (page 0)
//   0x101000 - 0x200000  staging area: verified update image, inert until
//                        applied by the RAM apply routine in sd_update.c
#ifndef FLASH_LAYOUT_H
#define FLASH_LAYOUT_H

#include <stdint.h>

#define MPD_APP_OFFSET       0x000000u
#define MPD_APP_MAX_SIZE     0x100000u

#define MPD_STAGE_HDR_OFFSET 0x100000u            // one 4 KB sector
#define MPD_STAGE_IMG_OFFSET 0x101000u
#define MPD_STAGE_MAX_SIZE   (0x200000u - MPD_STAGE_IMG_OFFSET)

#define MPD_DECLINE_MAGIC    0x5844504Du          // "MPDX"

_Static_assert(MPD_APP_OFFSET + MPD_APP_MAX_SIZE == MPD_STAGE_HDR_OFFSET,
               "app region must end where the staging header starts");
_Static_assert(MPD_STAGE_HDR_OFFSET + 0x1000u == MPD_STAGE_IMG_OFFSET,
               "staging header is exactly one 4 KB sector");
#ifdef PICO_FLASH_SIZE_BYTES
_Static_assert(MPD_STAGE_IMG_OFFSET + MPD_STAGE_MAX_SIZE == PICO_FLASH_SIZE_BYTES,
               "flash map assumes a 2 MB part");
#endif

// Decline record, page 0 of the header sector. Petit FatFs cannot delete the
// dropped file, so "No" at the confirm prompt is remembered here instead: a
// later boot that finds a file with this exact CRC skips it silently. Only
// the most recently declined image is remembered (erase-then-write of a
// single page). Erased flash (0xFF) fails the magic check = no record.
typedef struct {
    uint32_t magic;
    uint32_t size;       // declined image bytes (multiple of 256)
    uint32_t crc32;      // fl_crc32 over the image payload
    uint32_t inv_crc32;  // ~crc32 — guards against a torn page write
} mpd_decline_rec_t;

// Small bitwise CRC-32 (IEEE, reflected) — same algorithm on the file, the
// staged copy and the app region, so the compares can never disagree.
// ~8 cycles/bit is fine for <1 MB images.
static inline uint32_t fl_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static inline uint32_t fl_crc32(const uint8_t *p, uint32_t n) {
    return ~fl_crc32_update(0xFFFFFFFFu, p, n);
}

#endif // FLASH_LAYOUT_H
