// SD-card firmware update: drop the plain build MicroPicoDrive.uf2 into the
// /UPDATE folder on the card; at boot it is validated, staged to upper flash
// and, after on-screen confirmation (SELECT = yes, BACK = no, 30 s timeout =
// ask again next boot), applied and the module reboots.
//
// Petit FatFs cannot delete the file, so after a successful update it stays
// on the card and is skipped by CRC ("skip-if-same"); an explicit "no" is
// remembered in flash (see mpd_decline_rec_t in flash_layout.h) — only the
// most recently declined image sticks.
#ifndef SD_UPDATE_H
#define SD_UPDATE_H

void sd_update_check_at_boot(void);

#endif // SD_UPDATE_H
