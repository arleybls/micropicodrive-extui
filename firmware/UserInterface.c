#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "EventMachine.h"
#include "UserInterface.h"
#include "SharedBuffers.h"
#include "SharedEvents.h"
#include "ssd1306/ssd1306.h"
#include "pff/pff.h"
#include "animations.h"
#include "images.h"
#include "UserInterfaceExtension.h"
#include "sd_update.h"

#define LED_ON(LED) gpio_put(LED, true)
#define LED_OFF(LED) gpio_put(LED, false)
#define IS_UI_DISCONNECTED() gpio_get(PIN_UI_DETECT)
#define IN_FOLDER (fno.fattrib & AM_DIR)

#define PRINT_STR(STR, COL, ROW) ssd1306_draw_string(&disp, COL * 5, ROW * 8, 1, STR)
#define RENDER_SCREEN() ssd1306_show(&disp)
#define CLEAR_SCREEN() ssd1306_clear(&disp)
#define CONCAT(DEST, SOURCE) sprintf(&DEST[strlen(DEST)],"/%s", SOURCE)
#define CONFIG_FILE_SIZE 306  // "FILE=" (5) + path padded to 300 chars + '\n' (1)
USER_INTERFACE_STATE uiState = IDLE;

ssd1306_t disp;
FATFS fatfs;
DIR dir;
FILINFO fno;
char currentPath[PATH_BUFFER_SIZE];
uint8_t currentSector = 0;

bool mdInUse = false;

CARTRIDGE_FORMAT cfInserted = NONE;

// --- Directory menu state ---
#define MAX_DIR_ITEMS 64

typedef struct {
    char  name[13];
    DWORD fsize;
    bool  is_dir;
} DirEntry;

static DirEntry  dir_entries[MAX_DIR_ITEMS];
static char      disp_names[MAX_DIR_ITEMS][15];
static char     *disp_ptrs[MAX_DIR_ITEMS];
static int       dir_count   = 0;
static int       menu_offset = 0;
static DWORD     selected_fsize = 0;
static char      selected_name[13];
static char      configTaggedPath[PATH_BUFFER_SIZE];

uint64_t delayEnd;
USER_INTERFACE_STATE uiNextState;

bool save_mdv_cartridge();
bool save_mpd_cartridge();
static void motor_on(void);
static void motor_off(void);
static bool read_config_file(void);
static bool try_config_autoload(void);
static bool write_config_tag(const char *full_path);
static void show_cart_ready(void);

static int cmp_dir_entry(const void *a, const void *b) {
    const DirEntry *da = a, *db = b;
    if (da->is_dir != db->is_dir) return da->is_dir ? -1 : 1;
    return strcmp(da->name, db->name);
}

//Writes a pair of buffers of a buffer set (a buffer set are four buffers, two header ones and two sector ones)
void write_buffer_set_pair(uint8_t* source, uint8_t* track1Buffer, uint8_t* track2Buffer, bool isHeader)
{
    track2Buffer += 4; //we skip four bits on buffer 2 to respect the skewing done by the ULA

    for(int buc = 0; buc < PREAMBLE_ZERO_BITS; buc++)
    {
        track1Buffer[buc] = 0;
        track2Buffer[buc] = 0;
    } 

    track1Buffer += PREAMBLE_ZERO_BITS;
    track2Buffer += PREAMBLE_ZERO_BITS;

    for(int buc = 0; buc < PREAMBLE_ONE_BITS; buc++)
    {
        track1Buffer[buc] = 1;
        track2Buffer[buc] = 1;
    } 

    track1Buffer += PREAMBLE_ONE_BITS;
    track2Buffer += PREAMBLE_ONE_BITS;

    uint16_t copySize = isHeader ? HEADER_TRACK_DATA_SIZE : SECTOR_TRACK_DATA_SIZE;

    for(int buc = 0; buc < copySize; buc++)
    {
        uint8_t t1b = *source;
        source++;
        uint8_t t2b = *source;
        source++;

        for(int buc = 0; buc < 8; buc++)
        {
            *track1Buffer = (t1b >> buc) & 1;
            *track2Buffer = (t2b >> buc) & 1;
            track1Buffer++;
            track2Buffer++;
        }
    }
}

//Writes a buffer set with cartridge data
void write_buffer_set(uint8_t setNumber, uint8_t sector)
{
    if(setNumber == 0)
    {
        write_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * sector], header_1_track_1, header_1_track_2, true);
        write_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * sector + CARTRIDGE_HEADER_SIZE], sector_1_track_1, sector_1_track_2, false);
        bufferset_1_sector_number = sector;
    }
    else
    {
        write_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * sector], header_2_track_1, header_2_track_2, true);
        write_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * sector + CARTRIDGE_HEADER_SIZE], sector_2_track_1, sector_2_track_2, false);
        bufferset_2_sector_number = sector;
    }
}

//Find the end of a preamble from a track buffer
int8_t find_preamble_end(uint8_t* buffer)
{
    uint8_t zeroCount = 0;
    uint8_t oneCount = 0;

    //We need to find at least 16 zeros followed by 8 ones (0x00, 0x00, 0xFF)
    for(int buc = 0; buc < 100; buc++)
    {
        if(buffer[buc] == 0)
        {
            if(oneCount != 0) //Do we came here form a one?
            {
                //Reset everything
                zeroCount = 1;
                oneCount = 0;
            }
            else
                zeroCount++; //Increment count

        }
        else
        {
            if(zeroCount < 16) //Did we found a one before having eight zeros?
            {
                //Reset everything
                oneCount = 0;
                zeroCount = 0;
            }
            else
                oneCount++; //Increment count
        }

        //Have we found the eight ones?
        if(oneCount == 8)
            return buc + 1;
    }

    //Error! We haven't found the gap end!!
    return -1;

}

bool inFormat = false;
int skip = 0;

//Reads a pair of buffers from a buffer set (a buffer set are four buffers, two header ones and two sector ones)
void read_buffer_set_pair(uint8_t* destination, uint8_t* track1Buffer, uint8_t* track2Buffer, bool isHeader)
{
    uint16_t track1Pos = find_preamble_end(track1Buffer);
    uint16_t track2Pos = find_preamble_end(track2Buffer);

    uint16_t size = isHeader ? HEADER_TRACK_DATA_SIZE : SECTOR_TRACK_DATA_SIZE;

    //Check if we're writting sector 255, if true then this is a format
    if(isHeader && !inFormat)
    {
        uint8_t sectorNumber = track2Buffer[track2Pos] |
            track2Buffer[track2Pos + 1] << 1 |
            track2Buffer[track2Pos + 2] << 2 |
            track2Buffer[track2Pos + 3] << 3 |
            track2Buffer[track2Pos + 4] << 4 |
            track2Buffer[track2Pos + 5] << 5 |
            track2Buffer[track2Pos + 6] << 6 |
            track2Buffer[track2Pos + 7] << 7;

        if(sectorNumber == 255)
        {
            inFormat = 1;
            skip = currentSector;
        }
    }

    for(uint16_t buc = 0; buc < size; buc++)
    {
        *destination = track1Buffer[track1Pos] |
            track1Buffer[track1Pos + 1] << 1 |
            track1Buffer[track1Pos + 2] << 2 |
            track1Buffer[track1Pos + 3] << 3 |
            track1Buffer[track1Pos + 4] << 4 |
            track1Buffer[track1Pos + 5] << 5 |
            track1Buffer[track1Pos + 6] << 6 |
            track1Buffer[track1Pos + 7] << 7;

        track1Pos += 8;
        destination++;

        *destination = track2Buffer[track2Pos] |
            track2Buffer[track2Pos + 1] << 1 |
            track2Buffer[track2Pos + 2] << 2 |
            track2Buffer[track2Pos + 3] << 3 |
            track2Buffer[track2Pos + 4] << 4 |
            track2Buffer[track2Pos + 5] << 5 |
            track2Buffer[track2Pos + 6] << 6 |
            track2Buffer[track2Pos + 7] << 7;

        track2Pos += 8;
        destination++;

    }
}

//Reads a buffer set to the cartridge buffer
void read_buffer_set(uint8_t setNumber)
{
    if(setNumber == 0)
    {
        read_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * bufferset_1_sector_number], header_1_track_1, header_1_track_2, true);
        read_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * bufferset_1_sector_number + CARTRIDGE_HEADER_SIZE], sector_1_track_1, sector_1_track_2, false);
    }
    else
    {
        read_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * bufferset_2_sector_number], header_2_track_1, header_2_track_2, true);
        read_buffer_set_pair(&cartridge_image[CARTRIDGE_SECTOR_SIZE * bufferset_2_sector_number + CARTRIDGE_HEADER_SIZE], sector_2_track_1, sector_2_track_2, false);
    }
}

//Process when a buffer set has been read by the ULA
void process_md_read(uint8_t bufferSet)
{
    uint8_t secNum = cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 1];

    //If we are in the middle of a format and we're going to send sector 254, skip it to make Minerva happy...
    if(inFormat && secNum == 254)
    {
        currentSector++;

        if(currentSector > 253)
            currentSector = 0;
    }

    write_buffer_set(bufferSet, currentSector);

    //If we are in the middle of a format and we're going to send sector 13, damage it to make Minerva happy...
    if(inFormat && secNum == 13)
    {
        cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 13] += 13;
        cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 128] += 13;
    }

    currentSector++;

    if(currentSector == 255)
        currentSector = 0;
}

//Process when a buffer set has been written by the ULA
void process_md_write(uint8_t bufferSet)
{
    read_buffer_set(bufferSet);

    uint8_t secNum = cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 1];

    //If we are in the middle of a format and we're going to send sector 254, skip it to make Minerva happy...
    if(inFormat && secNum == 254)
    {
        currentSector++;

        if(currentSector > 253)
            currentSector = 0;
    }

    write_buffer_set(bufferSet, currentSector);

    //If we are in the middle of a format and we're going to send sector 13, damage it to make Minerva happy...
    if(inFormat && secNum == 13)
    {
        cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 13] += 13;
        cartridge_image[CARTRIDGE_SECTOR_SIZE * currentSector + 128] += 13;
    }

    currentSector++;
    if(currentSector == 255)
        currentSector = 0;
}

//Process events from the MD control
void process_md_to_ui_event(void* event)
{
    mtuevent_t* evt = (mtuevent_t*)event;

    switch(evt->event)
    {
        case MTU_MD_DESELECTED:

            mdInUse = false;
            inFormat = false;
            LED_OFF(PIN_LED_SELECT);
            LED_OFF(PIN_LED_READ);
            LED_OFF(PIN_LED_WRITE);
            break;

        case MTU_MD_SELECTED:

            mdInUse = true;
            LED_ON(PIN_LED_SELECT);
            break;

        case MTU_MD_READING:
            LED_ON(PIN_LED_READ);
            LED_OFF(PIN_LED_WRITE);
            break;

        case MTU_MD_WRITTING:
            LED_ON(PIN_LED_WRITE);
            LED_OFF(PIN_LED_READ);
            break;

        case MTU_BUFFERSET_READ:

            process_md_read(evt->arg);
            break;

        case MTU_BUFFERSET_WRITTEN:

            process_md_write(evt->arg);
            break;
    }
}

//Initialize the I2C screen
bool init_screen()
{
    disp.external_vcc=false;
    
    if(!ssd1306_init(&disp, 64, 32, 0x3C, I2C_PORT))
        return false;

    CLEAR_SCREEN();
    RENDER_SCREEN();

    return true;
}

//Show the current file name to the screen

//Remove from the path buffer the last entry
void rewind_path()
{
    if(strlen(currentPath) == 0)
        return;

    char* lastPos = strrchr(currentPath, '/');

    if(lastPos == NULL)
        return;

    memset(lastPos, 0, (size_t)(PATH_BUFFER_SIZE - (lastPos - currentPath)));
}

//Debounce a button press
void debounce_button(uint button)
{
    while(BUTTON_PRESSED(button))
    {
        sleep_ms(20);
    }
    sleep_ms(200);
}

void fix_cartridge_checksums()
{
    SECTOR_t* sector = (SECTOR_t*)cartridge_image;

    for(int buc = 0; buc < 255; buc++)
    {
        uint16_t computedChecksum = 0;

        for(int hBuc = 0; hBuc < 14; hBuc++)
            computedChecksum += sector->Header.HeaderData[hBuc];

        computedChecksum += 0x0f0f;
        sector->Header.Checksum = computedChecksum;

        computedChecksum = 0;

        for(int hrBuc = 0; hrBuc < 2; hrBuc++)
            computedChecksum += sector->Record.HeaderData[hrBuc];

        computedChecksum += 0x0f0f;
        sector->Record.HeaderChecksum = computedChecksum;

        computedChecksum = 0;

        for(int hdBuc = 0; hdBuc < 512; hdBuc++)
            computedChecksum += sector->Record.Data[hdBuc];

        computedChecksum += 0x0f0f;
        sector->Record.DataChecksum = computedChecksum;

        for (int bExtra = 0; bExtra < 84; bExtra++)
                sector->Record.ExtraBytes[bExtra] = bExtra % 2 == 0 ? 0xAA : 0x55;

        if (sector->Record.ExtraBytesChecksum != 0x3b19)
            sector->Record.ExtraBytesChecksum = 0x3b19;

        sector++;
    }
}

//Save the cartridge to a mdv image
bool save_mdv_cartridge()
{
    if(pf_open(currentPath))
        return false;

    UINT writeSize;
    uint8_t tmpByte = 0;
    int bufferPos = 0;

    uint8_t padBuffer[MDV_PAD_SIZE];
    memset(padBuffer, 'Z', MDV_PAD_SIZE);

    for(int buc = 0; buc < 255; buc++)
    {
        tmpByte = 0;

        for(int zeros = 0; zeros < PREAMBLE_ZERO_BYTES; zeros++)
        {
            if(pf_write(&tmpByte, 1, &writeSize))
            {
                pf_write(0, 0, &writeSize);
                return false;
            }

            if(writeSize != 1)
            {
                pf_write(0, 0, &writeSize);
                return false;
            }
        }

        tmpByte = 0xff;

        for(int ones = 0; ones < PREAMBLE_ONE_BYTES; ones++)
        {
            if(pf_write(&tmpByte, 1, &writeSize))
            {
                pf_write(0, 0, &writeSize);
                return false;
            }

            if(writeSize != 1)
            {
                pf_write(0, 0, &writeSize);
                return false;
            }
        }

        if(pf_write(&cartridge_image[bufferPos], CARTRIDGE_HEADER_SIZE, &writeSize))
        {
            pf_write(0, 0, &writeSize);
            return false;
        }

        if(writeSize != CARTRIDGE_HEADER_SIZE)
        {
            pf_write(0, 0, &writeSize);
            return false;
        }

        bufferPos += CARTRIDGE_HEADER_SIZE;

        tmpByte = 0;

        for(int zeros = 0; zeros < PREAMBLE_ZERO_BYTES; zeros++)
        {
            if(pf_write(&tmpByte, 1, &writeSize))
            {
                pf_write(0, 0, &writeSize);
                return false;
            }

            if(writeSize != 1)
            {
                pf_write(0, 0, &writeSize);
                return false;
            }
        }

        tmpByte = 0xff;

        for(int ones = 0; ones < PREAMBLE_ONE_BYTES; ones++)
        {
            if(pf_write(&tmpByte, 1, &writeSize))
            {
                pf_write(0, 0, &writeSize);
                return false;
            }

            if(writeSize != 1)
            {
                pf_write(0, 0, &writeSize);
                return false;
            }
        }

        if(pf_write(&cartridge_image[bufferPos], CARTRIDGE_DATA_SIZE, &writeSize))
        {
            pf_write(0, 0, &writeSize);
            return false;
        }

        if(writeSize != CARTRIDGE_DATA_SIZE)
            return false;

        bufferPos += CARTRIDGE_DATA_SIZE;

        if(pf_write(padBuffer, MDV_PAD_SIZE, &writeSize))
        {
            pf_write(0, 0, &writeSize);
            return false;
        }

        if(writeSize != MDV_PAD_SIZE)
            return false;
    }

    pf_write(0, 0, &writeSize);

    return true;
}

//Save the cartridge to a mpd image
bool save_mpd_cartridge()
{
    if(pf_open(currentPath))
        return false;

    UINT writeSize;

    if(pf_write(cartridge_image, CART_SIZE, &writeSize))
    {
        pf_write(0, 0, &writeSize);
        return false;
    }

    if(writeSize != CART_SIZE)
    {
        pf_write(0, 0, &writeSize);
        return false;
    }

    pf_write(0, 0, &writeSize);

    return true;
}

//Load a MDV image to the cartridge buffer
bool load_mdv_cartridge()
{
    if(pf_open(currentPath))
        return false;

    UINT readSize = 0;

    int bufferPos = 0;
    int filePos = 0;

    for(int buc = 0; buc < 255; buc++)
    {
        filePos = buc * MDV_SECTOR_SIZE + MDV_PREAMBLE_SIZE; //skip preamble

        if(pf_lseek(filePos))
            return false;

        if(pf_read(&cartridge_image[bufferPos], MDV_HEADER_SIZE, &readSize))
            return false;

        if(readSize != MDV_HEADER_SIZE)
            return false;

        filePos += MDV_HEADER_SIZE + MDV_PREAMBLE_SIZE;
        bufferPos += MPD_HEADER_SIZE;

        if(pf_lseek(filePos))
            return false;

        if(pf_read(&cartridge_image[bufferPos], MPD_DATA_SIZE, &readSize))
            return false;

        if(readSize != MPD_DATA_SIZE)
            return false;

        bufferPos += MPD_DATA_SIZE;
    }

    return true;
}

//Load a MPD image to the cartridge buffer
bool load_mpd_cartridge()
{
    if(pf_open(currentPath))
        return false;

    UINT readSize = 0;

    if(pf_read(cartridge_image, CART_MPD_SIZE, &readSize))
        return false;

    if(readSize != CART_MPD_SIZE)
        return false;

    return true;

}

//Check if cancel was requested
void check_cancel()
{
    if(BUTTON_PRESSED(PIN_BTN_BACK) || BUTTON_PRESSED(PIN_BTN_NEXT))
    {
        debounce_button(BUTTON_PRESSED(PIN_BTN_BACK) ? PIN_BTN_BACK : PIN_BTN_NEXT);
        rewind_path();
        uiState = OPEN_FOLDER;
        cfInserted = NONE;
        utmevent_t removeEvt;
        currentSector = 0;
        removeEvt.event = UTM_CARTRIDGE_REMOVED;
        event_push(&uiToMdEventQueue, &removeEvt);
    }
}

void program_delay(uint64_t ms_delay, USER_INTERFACE_STATE nextState)
{
    delayEnd = time_us_64() + (ms_delay * 1000);
    uiNextState = nextState;
    uiState = DELAY;
}

void check_delay()
{
    uint64_t currentTime = time_us_64();

    if(currentTime >= delayEnd)
        uiState = uiNextState;
}

//Process the user interface state machine
void process_user_interface()
{
    switch(uiState)
    {
        case IDLE:

            if(!IS_UI_DISCONNECTED())
                program_delay(2000, INIT_SCREEN);
            
            break;

        case DELAY:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
                check_delay();

            break;

        case INIT_SCREEN:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                if(init_screen())
                {
                    sd_update_check_at_boot();
                    uiState = WELCOME;
                }
            }
            break;

        case WELCOME:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                play_animation(&disp,
                    ANIM_BOOT_FRAMES, ANIM_BOOT_COUNT,
                    ANIM_BOOT_W, ANIM_BOOT_H, ANIM_BOOT_DELAYS,
                    0, 0.7f,
                    "MicroPico", ANIM_TEXT_MIDDLE);

                if(cfInserted == NONE)
                    memset(currentPath, 0, PATH_BUFFER_SIZE);

                uiState = SHOW_WAITING_SD_CARD;
            }
            break;

        case SHOW_WAITING_SD_CARD:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
                uiState = WAITING_SD_CARD;
            break;

        case WAITING_SD_CARD:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                play_animation(&disp,
                    ANIM_WAIT_FRAMES, ANIM_WAIT_COUNT,
                    ANIM_WAIT_W, ANIM_WAIT_H, ANIM_WAIT_DELAYS,
                    0, 0.7f,
                    "Waiting SD", ANIM_TEXT_BOTTOM);

                if(!pf_mount(&fatfs))
                {
                    if(cfInserted == NONE)
                    {
                        if(!try_config_autoload())
                            uiState = OPEN_FOLDER;
                    }
                    else
                    {
                        uiState = CARTRIDGE_READY;
                        show_cart_ready();
                    }
                }
            }

            break;

        case OPEN_FOLDER:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                motor_on();
                if(pf_opendir(&dir, currentPath))
                {
                    motor_off();
                    show_error(&disp, "Open error");
                    sleep_ms(2000);
                    uiState = WAITING_SD_CARD;
                }
                else
                {
                    dir_count = 0;
                    while(dir_count < MAX_DIR_ITEMS)
                    {
                        if(pf_readdir(&dir, &fno))
                        {
                            motor_off();
                            show_error(&disp, "Read error");
                            sleep_ms(2000);
                            uiState = WAITING_SD_CARD;
                            break;
                        }
                        if(fno.fname[0] == 0) break;
                        if(fno.fattrib & AM_SYS) continue;
                        if((fno.fattrib & AM_DIR) && currentPath[0] == 0 &&
                           strcmp(fno.fname, "UPDATE") == 0) continue;
                        if(!(fno.fattrib & AM_DIR))
                        {
                            const char *ext = strrchr(fno.fname, '.');
                            if(!ext ||
                               (strcmp(ext, ".MDV") != 0 && strcmp(ext, ".MPD") != 0))
                                continue;
                        }
                        strncpy(dir_entries[dir_count].name, fno.fname, 12);
                        dir_entries[dir_count].name[12] = '\0';
                        dir_entries[dir_count].fsize  = fno.fsize;
                        dir_entries[dir_count].is_dir = (fno.fattrib & AM_DIR) != 0;
                        dir_count++;
                    }

                    if(uiState != WAITING_SD_CARD)
                    {
                        motor_off();
                        qsort(dir_entries, dir_count, sizeof(DirEntry), cmp_dir_entry);

                        bool not_at_root = (strlen(currentPath) > 0);

                        strncpy(disp_names[0], not_at_root ? "[..]" : "[/]", 13);
                        disp_ptrs[0] = disp_names[0];

                        for(int i = 0; i < dir_count; i++)
                        {
                            char fullpath[PATH_BUFFER_SIZE];
                            snprintf(fullpath, PATH_BUFFER_SIZE, "%s/%s",
                                     currentPath, dir_entries[i].name);

                            if (configTaggedPath[0] != '\0' &&
                                strcmp(fullpath, configTaggedPath) == 0)
                            {
                                disp_names[i + 1][0] = '*';
                                strncpy(disp_names[i + 1] + 1, dir_entries[i].name, 12);
                                disp_names[i + 1][13] = '\0';
                            }
                            else
                            {
                                strncpy(disp_names[i + 1], dir_entries[i].name, 13);
                                disp_names[i + 1][13] = '\0';
                            }
                            disp_ptrs[i + 1] = disp_names[i + 1];
                        }

                        menu_offset = 0;
                        uiState = BROWSE_FOLDER;
                    }
                }
            }

            break;

        case BROWSE_FOLDER:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                bool not_at_root = (strlen(currentPath) > 0);
                int  total_count = dir_count + 1;  // slot 0 is always [/] or [Back]

                if(dir_count == 0)
                {
                    show_error(&disp, "Empty dir");
                    sleep_ms(2000);
                    if(not_at_root)
                    {
                        rewind_path();
                        menu_offset = 0;
                        uiState = OPEN_FOLDER;
                    }
                    else
                        uiState = WAITING_SD_CARD;
                }
                else
                {
                    uiext_draw_menu(disp_ptrs, total_count, &disp, menu_offset);
                    int sel = uiext_run_menu(disp_ptrs, total_count, &disp, &menu_offset);

                    if(sel == UIEXT_LONG_SELECT)
                    {
                        // Long-press SELECT: toggle auto-load tag on highlighted file
                        int entry_idx = menu_offset;
                        if(!dir_entries[entry_idx].is_dir)
                        {
                            char fullpath[PATH_BUFFER_SIZE];
                            snprintf(fullpath, PATH_BUFFER_SIZE, "%s/%s",
                                     currentPath, dir_entries[entry_idx].name);

                            bool already_tagged = (configTaggedPath[0] != '\0' &&
                                                   strcmp(fullpath, configTaggedPath) == 0);
                            if(already_tagged)
                            {
                                // Clear: write empty FILE= and remove *
                                if(write_config_tag(""))
                                {
                                    strncpy(disp_names[entry_idx + 1],
                                            dir_entries[entry_idx].name, 13);
                                    disp_names[entry_idx + 1][13] = '\0';
                                    uiext_draw_menu(disp_ptrs, total_count, &disp, menu_offset);
                                }
                            }
                            else
                            {
                                // Tag: write this file and update * in listing
                                if(write_config_tag(fullpath))
                                {
                                    for(int i = 0; i < dir_count; i++)
                                    {
                                        if(i == entry_idx)
                                        {
                                            disp_names[i + 1][0] = '*';
                                            strncpy(disp_names[i + 1] + 1,
                                                    dir_entries[i].name, 12);
                                            disp_names[i + 1][13] = '\0';
                                        }
                                        else if(disp_names[i + 1][0] == '*')
                                        {
                                            strncpy(disp_names[i + 1],
                                                    dir_entries[i].name, 13);
                                            disp_names[i + 1][13] = '\0';
                                        }
                                    }
                                    uiext_draw_menu(disp_ptrs, total_count, &disp, menu_offset);
                                }
                            }
                        }
                        // stay in BROWSE_FOLDER — no uiState change
                    }
                    else if(sel == 0)
                    {
                        if(not_at_root)
                        {
                            rewind_path();
                            menu_offset = 0;
                            uiState = OPEN_FOLDER;
                        }
                        // at root: ignore BACK at top of list
                    }
                    else
                    {
                        // sel = offset+1; items[sel] = dir_entries[sel-1]
                        int entry_idx = sel - 1;
                        if(dir_entries[entry_idx].is_dir)
                        {
                            CONCAT(currentPath, dir_entries[entry_idx].name);
                            menu_offset = 0;
                            uiState = OPEN_FOLDER;
                        }
                        else
                        {
                            selected_fsize = dir_entries[entry_idx].fsize;
                            strncpy(selected_name, dir_entries[entry_idx].name, 12);
                            selected_name[12] = '\0';
                            uiState = FILE_SELECTED;
                        }
                    }
                }
            }

            break;

        case FILE_SELECTED:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                switch(selected_fsize)
                {

                    case CART_MDV_SIZE:

                        play_animation(&disp,
                            ANIM_LOAD_FRAMES, ANIM_LOAD_COUNT,
                            ANIM_LOAD_W, ANIM_LOAD_H, ANIM_LOAD_DELAYS,
                            0, 0.7f,
                            "Load MDV", ANIM_TEXT_BOTTOM);
                        cfInserted = MDV;
                        uiState = FILE_LOAD;
                        break;

                    case CART_MPD_SIZE:

                        play_animation(&disp,
                            ANIM_LOAD_FRAMES, ANIM_LOAD_COUNT,
                            ANIM_LOAD_W, ANIM_LOAD_H, ANIM_LOAD_DELAYS,
                            0, 0.7f,
                            "Load MPD", ANIM_TEXT_BOTTOM);
                        cfInserted = MPD;
                        uiState = FILE_LOAD;
                        break;

                    default:

                        show_error(&disp, "Bad format");
                        sleep_ms(4000);
                        uiState = BROWSE_FOLDER;
                        break;

                }
            }

            break;

        case FILE_LOAD:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                CONCAT(currentPath, selected_name);

                bool res = false;

                motor_on();
                switch(cfInserted)
                {
                    case MDV:
                        res = load_mdv_cartridge();
                        break;
                    case MPD:
                        res = load_mpd_cartridge();
                        break;
                }
                motor_off();

                if(!res)
                {
                    show_error(&disp, "Load error");
                    rewind_path();
                    sleep_ms(4000);
                    cfInserted = NONE;
                    uiState = OPEN_FOLDER;
                }
                else
                {

                    play_animation(&disp,
                        ANIM_LOAD_FRAMES, ANIM_LOAD_COUNT,
                        ANIM_LOAD_W, ANIM_LOAD_H, ANIM_LOAD_DELAYS,
                        0, 0.7f,
                        "Validating", ANIM_TEXT_BOTTOM);

                    fix_cartridge_checksums();

                    write_buffer_set(0, 0);
                    write_buffer_set(1, 1);
                    currentSector = 2;
                    uiState = CARTRIDGE_READY;
                    utmevent_t insertEvt;
                    insertEvt.event = UTM_CARTRIDGE_INSERTED;
                    event_push(&uiToMdEventQueue, &insertEvt);
                    show_cart_ready();
                }
            }

            break;

        case CARTRIDGE_READY:

            if(IS_UI_DISCONNECTED())
                uiState = IDLE;
            else
            {
                if(BUTTON_PRESSED(PIN_BTN_BACK) || BUTTON_PRESSED(PIN_BTN_NEXT))
                {
                    debounce_button(BUTTON_PRESSED(PIN_BTN_BACK) ? PIN_BTN_BACK : PIN_BTN_NEXT);
                    rewind_path();
                    uiState = OPEN_FOLDER;
                    cfInserted = NONE;
                    utmevent_t removeEvt;
                    removeEvt.event = UTM_CARTRIDGE_REMOVED;
                    event_push(&uiToMdEventQueue, &removeEvt);
                }
                else if(BUTTON_PRESSED(PIN_BTN_SELECT))
                {
                    play_animation(&disp,
                        ANIM_WRITE_FRAMES, ANIM_WRITE_COUNT,
                        ANIM_WRITE_W, ANIM_WRITE_H, ANIM_WRITE_DELAYS,
                        0, 0.7f,
                        NULL, ANIM_TEXT_NONE);

                    bool res = false;

                    motor_on();
                    switch(cfInserted)
                    {
                        case MDV:
                            res = save_mdv_cartridge();
                            break;
                        case MPD:
                            res = save_mpd_cartridge();
                            break;
                    }
                    motor_off();

                    if(res)
                    {
                        show_sdcard(&disp, "Saved");
                        sleep_ms(2000);
                        show_cart_ready();
                    }
                    else
                    {
                        show_error(&disp, "Save error");
                        sleep_ms(2000);
                        show_cart_ready();
                    }
                }
            }

            break;

    }
}

static void motor_on(void) {
    gpio_put(PIN_MOTOR, 1);
}

static void motor_off(void) {
    sleep_ms(500);
    gpio_put(PIN_MOTOR, 0);
}

static void init_motor(void) {
    gpio_init(PIN_MOTOR);
    gpio_set_dir(PIN_MOTOR, true);
    gpio_put(PIN_MOTOR, 0);
}

//Initialize UI leds
void init_leds()
{
    gpio_init(PIN_LED_ON);
    gpio_init(PIN_LED_SELECT);
    gpio_init(PIN_LED_READ);
    gpio_init(PIN_LED_WRITE);

    gpio_set_dir(PIN_LED_ON, true);
    gpio_set_dir(PIN_LED_SELECT, true);
    gpio_set_dir(PIN_LED_READ, true);
    gpio_set_dir(PIN_LED_WRITE, true);

    gpio_put(PIN_LED_ON, 1);
}

//Initialize UI buttons
void init_buttons()
{
    gpio_init(PIN_BTN_BACK);
    gpio_init(PIN_BTN_NEXT);
    gpio_init(PIN_BTN_SELECT);
    gpio_init(PIN_UI_DETECT);

    gpio_set_dir(PIN_BTN_BACK, false);
    gpio_set_dir(PIN_BTN_NEXT, false);
    gpio_set_dir(PIN_BTN_SELECT, false);
    gpio_set_dir(PIN_UI_DETECT, false);

    gpio_pull_up(PIN_BTN_BACK);
    gpio_pull_up(PIN_BTN_NEXT);
    gpio_pull_up(PIN_BTN_SELECT);
    gpio_pull_up(PIN_UI_DETECT);
}

//Initialize the I2C bus
void init_i2c()
{
    // I2C Initialisation. Using it at 400Khz.
    i2c_init(I2C_PORT, 400*1000);
    
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
}

// Shows the loaded image name (no extension, truncated with .. if over 10 chars).
static void show_cart_ready(void) {
    char base[13];
    strncpy(base, selected_name, 12);
    base[12] = '\0';
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    int len = (int)strlen(base);
    char label[11];
    if (len <= 10) {
        strcpy(label, base);
    } else {
        strncpy(label, base, 8);
        label[8] = '\0';
        strcat(label, "..");
    }
    show_sdcard(&disp, label);
}

// Reads CONFIG.CFG from SD root, populates configTaggedPath with the FILE= value.
// Returns true if a non-empty FILE= value was found, false otherwise.
// On success the motor is left running for the load that follows; every
// failure path stops it here.
static bool read_config_file(void) {
    char buf[CONFIG_FILE_SIZE + 1];
    UINT br = 0;

    configTaggedPath[0] = '\0';

    motor_on();

    if (pf_open("CONFIG.CFG")) {
        motor_off();
        return false;
    }

    if (pf_read(buf, CONFIG_FILE_SIZE, &br)) {
        motor_off();
        return false;
    }

    buf[br] = '\0';

    char *p = strstr(buf, "FILE=");
    if (!p) {
        motor_off();
        return false;
    }

    p += 5;

    char *end = p;
    while (*end && *end != '\n' && *end != '\r')
        end++;
    while (end > p && *(end - 1) == ' ')
        end--;

    int len = (int)(end - p);
    if (len <= 0) {
        motor_off();
        return false;
    }

    int copy = (len < PATH_BUFFER_SIZE - 1) ? len : PATH_BUFFER_SIZE - 1;
    strncpy(configTaggedPath, p, copy);
    configTaggedPath[copy] = '\0';
    return true;
}

// Reads CONFIG.CFG and, if a valid FILE= path is found, loads the image and
// transitions to CARTRIDGE_READY. Returns true on successful auto-load.
static bool try_config_autoload(void) {
    if (!read_config_file())
        return false;

    // read_config_file() left the motor running for the load below, so the
    // paths that bail out before it has to stop it themselves.
    const char *dot = strrchr(configTaggedPath, '.');
    if (!dot) {
        motor_off();
        return false;
    }

    CARTRIDGE_FORMAT fmt;
    if (strcmp(dot, ".MDV") == 0 || strcmp(dot, ".mdv") == 0)
        fmt = MDV;
    else if (strcmp(dot, ".MPD") == 0 || strcmp(dot, ".mpd") == 0)
        fmt = MPD;
    else {
        motor_off();
        return false;
    }

    cfInserted = fmt;
    strncpy(currentPath, configTaggedPath, PATH_BUFFER_SIZE - 1);
    currentPath[PATH_BUFFER_SIZE - 1] = '\0';
    const char *slash = strrchr(configTaggedPath, '/');
    const char *base_name = slash ? slash + 1 : configTaggedPath;
    strncpy(selected_name, base_name, 12);
    selected_name[12] = '\0';

    bool res = (fmt == MDV) ? load_mdv_cartridge() : load_mpd_cartridge();
    motor_off();
    if (!res) {
        show_error(&disp, "Load fail");
        rewind_path();
        cfInserted = NONE;
        sleep_ms(2000);
        return false;
    }

    fix_cartridge_checksums();
    write_buffer_set(0, 0);
    write_buffer_set(1, 1);
    currentSector = 2;
    uiState = CARTRIDGE_READY;

    utmevent_t evt;
    evt.event = UTM_CARTRIDGE_INSERTED;
    event_push(&uiToMdEventQueue, &evt);

    show_cart_ready();
    return true;
}

// Writes the given full_path as FILE= into CONFIG.CFG (fixed-size, in-place).
// CONFIG.CFG must already exist on the SD card with at least CONFIG_FILE_SIZE bytes.
static bool write_config_tag(const char *full_path) {
    char buf[CONFIG_FILE_SIZE];
    UINT bw = 0;

    memset(buf, ' ', CONFIG_FILE_SIZE);
    buf[0] = 'F'; buf[1] = 'I'; buf[2] = 'L'; buf[3] = 'E'; buf[4] = '=';
    int plen = (int)strlen(full_path);
    if (plen > 300) plen = 300;
    memcpy(buf + 5, full_path, (size_t)plen);
    buf[CONFIG_FILE_SIZE - 1] = '\n';

    motor_on();

    if (pf_open("CONFIG.CFG")) {
        motor_off();
        show_error(&disp, "No CONFIG");
        sleep_ms(2000);
        return false;
    }

    if (pf_write(buf, CONFIG_FILE_SIZE, &bw) || bw != CONFIG_FILE_SIZE) {
        pf_write(0, 0, &bw);
        motor_off();
        show_error(&disp, "Write failed");
        sleep_ms(2000);
        return false;
    }

    pf_write(0, 0, &bw);
    motor_off();

    strncpy(configTaggedPath, full_path, PATH_BUFFER_SIZE - 1);
    configTaggedPath[PATH_BUFFER_SIZE - 1] = '\0';
    return true;
}

//Main user interface loop
void RunUserInterface()
{
    event_machine_init(&mdToUiEventQueue, &process_md_to_ui_event, sizeof(mtuevent_t), 8);
    mtuevent_t mtuevtBuffer;

    init_leds();
    init_buttons();
    init_motor();
    init_i2c();

    while(true)
    {
        event_process_queue(&mdToUiEventQueue, &mtuevtBuffer, 16);

        if(!mdInUse)
            process_user_interface();
        else
            check_cancel();

    }
}
