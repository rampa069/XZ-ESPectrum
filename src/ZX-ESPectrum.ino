// -------------------------------------------------------------------
//  ESPectrum Emulator
//  Ramón Martínez & Jorge fuertes 2019
//  Though from ESP32 Spectrum Emulator (KOGEL esp32) Pete Todd 2017
//  You are not allowed to distribute this software commercially
//  lease, notify me, if you make any changes to this file
// -------------------------------------------------------------------

#include "Emulator/Keyboard/PS2Kbd.h"
#include "Emulator/msg.h"
#include "Emulator/z80emu/z80emu.h"
#include "Emulator/z80user.h"
//#include "FS.h"
//#include "SPIFFS.h"
#include "machinedefs.h"
#include <ESP32Lib.h>
#include <Ressources/Font6x8.h>
#include <esp_bt.h>
#include <esp_task_wdt.h>

//
// types

// EXTERN VARS
extern boolean writeScreen;
extern boolean cfg_mode_sna;
extern boolean cfg_slog_on;
extern boolean cfg_debug_on;
extern String cfg_ram_file;
extern String cfg_rom_file;
extern CONTEXT _zxContext;
extern Z80_STATE _zxCpu;
extern int _total;
extern int _next_total;

void load_rom(String);
void load_ram(String);
volatile byte keymap[256];
volatile byte oldKeymap[256];

// EXTERN METHODS

void zx_setup(void); /* Reset registers to the initial values */
void zx_loop();
void zx_reset();
void setup_cpuspeed();
void config_read();
void do_OSD();
void errorHalt(String);
void mount_spiffs();

// GLOBALS
volatile byte *bank0;
volatile byte z80ports_in[128];
byte borderTemp = 7;
byte soundTemp = 0;
unsigned int flashing = 0;
byte lastAudio = 0;
boolean xULAStop = false;
boolean xULAStopped = false;
boolean writeScreen = false;
byte tick;

// SETUP *************************************

#ifdef COLOUR_8
VGA3Bit vga;
#endif

#ifdef COLOUR_16
VGA14Bit vga;
#endif

void setup() {
    // Turn off peripherals to gain memory (?do they release properly)
    esp_bt_controller_deinit();
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    // esp_wifi_set_mode(WIFI_MODE_NULL);

    mount_spiffs();
    config_read();

    if (cfg_slog_on) {
        Serial.println(MSG_CHIP_SETUP);
        Serial.println(MSG_VGA_INIT);
    }

#ifdef COLOUR_8
    vga.init(vga.MODE360x200, redPin, greenPin, bluePin, hsyncPin, vsyncPin);
#endif

#ifdef COLOUR_16
    vga.init(vga.MODE360x200, redPins, greenPins, bluePins, hsyncPin, vsyncPin);
#endif

    Serial.printf("VGA RGB: %x\n", vga.RGBA(0xff, 0xff, 0xff, 0xff));

    pinMode(SOUND_PIN, OUTPUT);
    digitalWrite(SOUND_PIN, LOW);

    kb_begin();

    // ALLOCATE MEMORY
    //
    bank0 = (byte *)malloc(49152);
    if (bank0 == NULL)
        errorHalt((String)ERR_BANK_FAIL + "0");

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    Serial.printf("%s bank %u: %ub\n", MSG_FREE_HEAP_AFTER, 0, system_get_free_heap_size());
#pragma GCC diagnostic warning "-Wall"

    setup_cpuspeed();

    // START Z80
    if (cfg_slog_on)
        Serial.println(MSG_Z80_RESET);
    zx_setup();

    // make sure keyboard ports are FF
    for (int t = 0; t < 32; t++) {
        z80ports_in[t] = 0x1f;
    }

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    Serial.printf("%s %u\n", MSG_EXEC_ON_CORE, xPortGetCoreID());
    Serial.printf("%s Z80 RESET: %ub\n", MSG_FREE_HEAP_AFTER, system_get_free_heap_size());
#pragma GCC diagnostic warning "-Wall"

    xTaskCreatePinnedToCore(videoTask,   /* Function to implement the task */
                            "videoTask", /* Name of the task */
                            1024,        /* Stack size in words */
                            NULL,        /* Task input parameter */
                            20,          /* Priority of the task */
                            NULL,        /* Task handle. */
                            0);          /* Core where the task should run */

    load_rom(cfg_rom_file);
    if (cfg_mode_sna)
        load_ram(cfg_ram_file);
}

// VIDEO core 0 *************************************

void videoTask(void *parameter) {
    unsigned int ff, i, byte_offset;
    unsigned char color_attrib, pixel_map, flash, bright;
    unsigned int zx_vidcalc, calc_y;
    unsigned int old_border;
    unsigned int ts1, ts2;
    word zx_fore_color, zx_back_color, tmp_color;

    while (1) {
        while (xULAStop) {
            xULAStopped = true;
            delay(5);
        }
        xULAStopped = false;

        ts1 = millis();

        if (flashing++ > 32)
            flashing = 0;

        for (unsigned int vga_lin = 0; vga_lin < 200; vga_lin++) {
            tick = 0;
            if (vga_lin < 4 || vga_lin > 194) {
                for (int bor = 32; bor < 328; bor++)
                    vga.dotFast(bor, vga_lin, zxcolor(borderTemp, 0));
            } else {
                for (int bor = 32; bor < 52; bor++) {
                    vga.dotFast(bor, vga_lin, zxcolor(borderTemp, 0));
                    vga.dotFast(bor + 276, vga_lin, zxcolor(borderTemp, 0));
                }

                for (ff = 0; ff < 32; ff++) // foreach byte in line
                {

                    byte_offset = (vga_lin - 3) * 32 + ff; //*2+1;

                    color_attrib = bank0[0x1800 + (calcY(byte_offset) / 8) * 32 + ff]; // get 1 of 768 attrib values
                    pixel_map = bank0[byte_offset];
                    calc_y = calcY(byte_offset);

                    for (i = 0; i < 8; i++) // foreach pixel within a byte
                    {

                        zx_vidcalc = ff * 8 + i;
                        byte bitpos = (0x80 >> i);
                        bright = (color_attrib & 0B01000000) >> 6;
                        flash = (color_attrib & 0B10000000) >> 7;
                        zx_fore_color = zxcolor((color_attrib & 0B00000111), bright);
                        zx_back_color = zxcolor(((color_attrib & 0B00111000) >> 3), bright);
                        if (flash && (flashing > 16)) {
                            tmp_color = zx_fore_color;
                            zx_fore_color = zx_back_color;
                            zx_back_color = tmp_color;
                        }

                        writeScreen = true;
                        if ((pixel_map & bitpos) != 0)
                            vga.dotFast(zx_vidcalc + 52, calc_y + 3, zx_fore_color);
                        else
                            vga.dotFast(zx_vidcalc + 52, calc_y + 3, zx_back_color);
                        writeScreen = false;
                    }
                }
            }
        }
        tick = 1;
        ts2 = millis();
    }
    TIMERG0.wdt_wprotect = TIMG_WDT_WKEY_VALUE;
    TIMERG0.wdt_feed = 1;
    TIMERG0.wdt_wprotect = 0;
    if (ts2 - ts1 < 20)
        delay(20 - (ts2 - ts1));
    // vTaskDelay(1);
}

// SPECTRUM SCREEN DISPLAY
//
/* Calculate Y coordinate (0-192) from Spectrum screen memory location */
int calcY(int offset) {
    return ((offset >> 11) << 6)                                            // sector start
           + ((offset % 2048) >> 8)                                         // pixel rows
           + ((((offset % 2048) >> 5) - ((offset % 2048) >> 8 << 3)) << 3); // character rows
}

/* Calculate X coordinate (0-255) from Spectrum screen memory location */
int calcX(int offset) { return (offset % 32) << 3; }

unsigned int zxcolor(int c, int bright) {
    word vga_color;

    switch (c) {

    case 0:
        vga_color = BLACK;
        break;
    case 1:
        vga_color = BLUE;
        break;
    case 2:
        vga_color = RED;
        break;
    case 3:
        vga_color = MAGENTA;
        break;
    case 4:
        vga_color = GREEN;
        break;
    case 5:
        vga_color = CYAN;
        break;
    case 6:
        vga_color = YELLOW;
        break;
    case 7:
        vga_color = WHITE;
        break;
    }

#ifdef COLOUR_16
    if (bright && c != 0)
        vga_color |= 0xCE7;
#endif

    return vga_color;
}

/* Load zx keyboard lines from PS/2 */
void do_keyboard() {
    byte kempston = 0;

    bitWrite(z80ports_in[0], 0, keymap[0x12]);
    bitWrite(z80ports_in[0], 1, keymap[0x1a]);
    bitWrite(z80ports_in[0], 2, keymap[0x22]);
    bitWrite(z80ports_in[0], 3, keymap[0x21]);
    bitWrite(z80ports_in[0], 4, keymap[0x2a]);

    bitWrite(z80ports_in[1], 0, keymap[0x1c]);
    bitWrite(z80ports_in[1], 1, keymap[0x1b]);
    bitWrite(z80ports_in[1], 2, keymap[0x23]);
    bitWrite(z80ports_in[1], 3, keymap[0x2b]);
    bitWrite(z80ports_in[1], 4, keymap[0x34]);

    bitWrite(z80ports_in[2], 0, keymap[0x15]);
    bitWrite(z80ports_in[2], 1, keymap[0x1d]);
    bitWrite(z80ports_in[2], 2, keymap[0x24]);
    bitWrite(z80ports_in[2], 3, keymap[0x2d]);
    bitWrite(z80ports_in[2], 4, keymap[0x2c]);

    bitWrite(z80ports_in[3], 0, keymap[0x16]);
    bitWrite(z80ports_in[3], 1, keymap[0x1e]);
    bitWrite(z80ports_in[3], 2, keymap[0x26]);
    bitWrite(z80ports_in[3], 3, keymap[0x25]);
    bitWrite(z80ports_in[3], 4, keymap[0x2e]);

    bitWrite(z80ports_in[4], 0, keymap[0x45]);
    bitWrite(z80ports_in[4], 1, keymap[0x46]);
    bitWrite(z80ports_in[4], 2, keymap[0x3e]);
    bitWrite(z80ports_in[4], 3, keymap[0x3d]);
    bitWrite(z80ports_in[4], 4, keymap[0x36]);

    bitWrite(z80ports_in[5], 0, keymap[0x4d]);
    bitWrite(z80ports_in[5], 1, keymap[0x44]);
    bitWrite(z80ports_in[5], 2, keymap[0x43]);
    bitWrite(z80ports_in[5], 3, keymap[0x3c]);
    bitWrite(z80ports_in[5], 4, keymap[0x35]);

    bitWrite(z80ports_in[6], 0, keymap[0x5a]);
    bitWrite(z80ports_in[6], 1, keymap[0x4b]);
    bitWrite(z80ports_in[6], 2, keymap[0x42]);
    bitWrite(z80ports_in[6], 3, keymap[0x3b]);
    bitWrite(z80ports_in[6], 4, keymap[0x33]);

    bitWrite(z80ports_in[7], 0, keymap[0x29]);
    bitWrite(z80ports_in[7], 1, keymap[0x14]);
    bitWrite(z80ports_in[7], 2, keymap[0x3a]);
    bitWrite(z80ports_in[7], 3, keymap[0x31]);
    bitWrite(z80ports_in[7], 4, keymap[0x32]);

    // Kempston joystick
    z80ports_in[0x1f] = 0;
    bitWrite(z80ports_in[0x1f], 0, !keymap[0x74]);
    bitWrite(z80ports_in[0x1f], 1, !keymap[0x6b]);
    bitWrite(z80ports_in[0x1f], 2, !keymap[0x72]);
    bitWrite(z80ports_in[0x1f], 3, !keymap[0x75]);
    bitWrite(z80ports_in[0x1f], 4, !keymap[0x73]);

    /*if (!keymap[0x75])
        kempston=kempston+8;

    if (!keymap[0x72])
        kempston=kempston+4;

    if (!keymap[0x6b])
        kempston=kempston+2;

    if (!keymap[0x74])
        kempston=kempston+1;

    if (!keymap[0x73])
        kempston=kempston+16;

        z80ports_in[31]=kempston; */
}

/* +-------------+
 | LOOP core 1 |
 +-------------+
 */
void loop() {
    while (1) {
        do_keyboard();
        do_OSD();
        // Z80Emulate(&_zxCpu, _next_total - _total, &_zxContext);
        zx_loop();
        TIMERG0.wdt_wprotect = TIMG_WDT_WKEY_VALUE;
        TIMERG0.wdt_feed = 1;
        TIMERG0.wdt_wprotect = 0;
        vTaskDelay(1); // important to avoid task watchdog timeouts - change this to slow down emu
    }
}
