#include "ff.h"
#include "cursor_loader.h"

extern uint32_t* gfx_framebuffer;
extern uint32_t gfx_width;
extern uint32_t gfx_height;
extern int mouse_x, mouse_y;
extern void serial_printf(const char* format, ...);

#define MAX_CURSOR_W 32
#define MAX_CURSOR_H 32

static uint8_t cursor_pixels[MAX_CURSOR_W * MAX_CURSOR_H * 4];
static uint32_t c_width = 0;
static uint32_t c_height = 0;
static int hotspot_x = 0;
static int hotspot_y = 0;

void load_system_cursor(const char* path) {
    FIL file;
    UINT br;
    uint8_t header[22]; // 6 bytes file header + 16 bytes directory entry

    if (f_open(&file, path, FA_READ) != FR_OK) {
        serial_printf("[CURSOR] Failed to open %s\n", path);
        return;
    }

    f_read(&file, header, 22, &br);

    // Verify .cur magic number (type must equal 2)
    uint16_t type = *(uint16_t*)&header[2];
    if (type != 2) {
        serial_printf("[CURSOR] Invalid format (not a .cur file)\n");
        f_close(&file);
        return;
    }

    c_width = header[6] ? header[6] : 256;
    c_height = header[7] ? header[7] : 256;
    
    // Extract Hotspot offsets from the directory entry
    hotspot_x = *(uint16_t*)&header[10];
    hotspot_y = *(uint16_t*)&header[12];

    uint32_t data_offset = *(uint32_t*)&header[18];
    f_lseek(&file, data_offset);

    // Read the embedded BMP Info Header (40 bytes) to find the color depth
    uint8_t bmi_header[40];
    f_read(&file, bmi_header, 40, &br);
    uint16_t bpp = *(uint16_t*)&bmi_header[14];

    // Read the actual pixel bitmap data
    if (bpp == 32) { // 32-bit with Alpha Channel
        f_read(&file, cursor_pixels, c_width * c_height * 4, &br);
    } else if (bpp == 24) { // 24-bit RGB
        f_read(&file, cursor_pixels, c_width * c_height * 3, &br);
    }

    serial_printf("[CURSOR] Loaded %dx%d cursor. Hotspot: (%d,%d)\n", c_width, c_height, hotspot_x, hotspot_y);
    f_close(&file);
}

void draw_custom_cursor(void) {
    if (c_width == 0 || c_height == 0) return;

    // Declare the external back buffer so this file can access it
    extern uint32_t back_buffer[];

    // Adjust mouse drawing coordinates using the loaded file's hotspot offsets
    int start_x = mouse_x - hotspot_x;
    int start_y = mouse_y - hotspot_y;

    for (uint32_t y = 0; y < c_height; y++) {
        for (uint32_t x = 0; x < c_width; x++) {
            int scr_x = start_x + x;
            int scr_y = start_y + y;

            if (scr_x < 0 || scr_x >= (int)gfx_width || scr_y < 0 || scr_y >= (int)gfx_height) continue;

            // Invert Y axis because embedded bitmaps are upside down
            uint32_t bmp_y = c_height - 1 - y;
            uint32_t idx = (bmp_y * c_width + x) * 4; // Assuming 32-bit layout

            uint8_t b = cursor_pixels[idx + 0];
            uint8_t g = cursor_pixels[idx + 1];
            uint8_t r = cursor_pixels[idx + 2];
            uint8_t a = cursor_pixels[idx + 3];

            // Pure magenta fallback transparency check or true alpha channel check
            if ((r == 255 && g == 0 && b == 255) || a == 0) continue;

            // FIX: Write directly to back_buffer instead of gfx_framebuffer!
            back_buffer[scr_y * (int)gfx_width + scr_x] = (r << 16) | (g << 8) | b;
        }
    }
}