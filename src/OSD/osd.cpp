#include "osd.h"
#include "Disk.h"
#include "Emulator/Keyboard/PS2Kbd.h"
#include "Emulator/z80main.h"
#include "ZX-ESPectrum.h"
#include "def/Font.h"
#include "def/files.h"
#include "def/keys.h"
#include "def/msg.h"
#include "Wiimote2Keys.h"

// Cursor to OSD first row,col
void osdHome() { vga.setCursor(osdInsideX(), osdInsideY()); }

// Cursor positioning
void osdAt(byte row, byte col) {
    if (row > osdMaxRows() - 1)
        row = 0;
    if (col > osdMaxCols() - 1)
        col = 0;
    unsigned short y = (row * OSD_FONT_H) + osdInsideY();
    unsigned short x = (col * OSD_FONT_W) + osdInsideX();
    vga.setCursor(x, y);
}

void drawOSD() {
    unsigned short x = scrAlignCenterX(OSD_W);
    unsigned short y = scrAlignCenterY(OSD_H);
    vga.fillRect(x, y, OSD_W, OSD_H, zxcolor(1, 0));
    vga.rect(x, y, OSD_W, OSD_H, zxcolor(0, 0));
    vga.rect(x + 1, y + 1, OSD_W - 2, OSD_H - 2, zxcolor(7, 0));
    vga.setTextColor(zxcolor(0, 0), zxcolor(5, 1));
    vga.setFont(Font6x8);
    osdHome();
    vga.print(OSD_TITLE);
    osdAt(17, 0);
    vga.print(OSD_BOTTOM);
    osdHome();
}

// OSD Main Loop
void do_OSD() {
    static byte last_sna_row = 0;
    static unsigned int last_demo_ts = millis() / 1000;
    boolean cycle_sna = false;
    if (checkAndCleanKey(KEY_F12)) {
        cycle_sna = true;
    } else if (checkAndCleanKey(KEY_F2)) {
        byte opt = menuRun(MENU_TEST);
    } else if (checkAndCleanKey(KEY_PAUSE)) {
        osdCenteredMsg(OSD_PAUSE, LEVEL_INFO);
        while (!checkAndCleanKey(KEY_PAUSE)) {
            delay(5);
        }
    } else if (checkAndCleanKey(KEY_F1)) {
        // Main menu
        byte opt = menuRun(MENU_MAIN);
        if (opt == 2) {
            // Change ROM
            String arch_menu = getArchMenu();
            byte arch_num = menuRun(arch_menu);
            if (arch_num > 0) {
                String romset_menu = getRomsetMenu(rowGet(arch_menu, arch_num));
                byte romset_num = menuRun(romset_menu);
                if (romset_num > 0) {
                    cfg_arch = rowGet(arch_menu, arch_num);
                    cfg_rom_set = rowGet(romset_menu, romset_num);
                    load_rom(cfg_arch, cfg_rom_set);
                    vTaskDelay(2);

                    config_save();
                    vTaskDelay(2);
                    zx_reset();
                }
            }
        } else if (opt == 1) {
            // Change RAM
            unsigned short snanum = menuRun(cfg_sna_file_list);
            if (snanum > 0) {
                if (cfg_demo_mode_on) {
                    setDemoMode(OFF, 0);
                }
                changeSna(rowGet(cfg_sna_file_list, snanum));
            }
        } else if (opt == 3) {
            // Reset
            byte opt2 = menuRun(MENU_RESET);
            if (opt2 == 1) {
                // Soft
                zx_reset();
                if (cfg_ram_file != (String)NO_RAM_FILE)
                    load_ram("/sna/" + cfg_ram_file);
            } else if (opt2 == 2) {
                // Hard
                cfg_ram_file = (String)NO_RAM_FILE;
                config_save();
                zx_reset();
            }
        } else if (opt == 4) {
            // Help
            drawOSD();
            osdAt(2, 0);
            vga.setTextColor(zxcolor(7, 0), zxcolor(1, 0));
            vga.print(OSD_HELP);
            while (!checkAndCleanKey(KEY_F1) && !checkAndCleanKey(KEY_ESC) && !checkAndCleanKey(KEY_ENTER)) {
                vTaskDelay(5);
                updateWiimote2KeysOSD();
            }
        }
        // Exit
    }

    if (cycle_sna || (cfg_demo_mode_on && ((millis() / 1000) - last_demo_ts) > cfg_demo_every)) {
        // Cycle over snapshots
        last_sna_row++;
        if (last_sna_row > rowCount(cfg_sna_file_list) - 1) {
            last_sna_row = 1;
        }
        changeSna(rowGet(cfg_sna_file_list, last_sna_row));
        last_demo_ts = millis() / 1000;
        cycle_sna = false;
    }
}
