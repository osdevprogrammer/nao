#ifndef BOOT_SCREEN_H
#define BOOT_SCREEN_H

#include <stdint.h>
#include "font.h"

// External references from kernel.c / idt.c
extern uint32_t* gfx_framebuffer;
extern uint32_t  gfx_width;
extern uint32_t  gfx_height;
extern uint32_t  gfx_pitch;
extern volatile uint32_t timer_ticks;
extern void putpixel(uint32_t* fb, int x, int y, uint32_t color, uint32_t pitch);

// Drawing primitives for the boot screen
static void boot_draw_rect(uint32_t* fb, int x, int y, int w, int h, uint32_t color, uint32_t pitch) {
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            putpixel(fb, x + col, y + row, color, pitch);
        }
    }
}

static void boot_draw_string(uint32_t* fb, int x, int y, const char* str, uint32_t color, uint32_t pitch) {
    while (*str) {
        draw_char(fb, x, y, *str, color, pitch, putpixel);
        x += 8;
        str++;
    }
}

static void boot_draw_progress_bar(uint32_t* fb, int x, int y, int width, int height, int progress_pct, uint32_t pitch) {
    uint32_t border_color = 0x00AAAAAA;
    uint32_t bg_color = 0x00333333;
    uint32_t fill_color = 0x0044AAFF;

    boot_draw_rect(fb, x, y, width, height, border_color, pitch);
    boot_draw_rect(fb, x + 2, y + 2, width - 4, height - 4, bg_color, pitch);

    int fill_w = ((width - 4) * progress_pct) / 100;
    if (fill_w > 0) {
        boot_draw_rect(fb, x + 2, y + 2, fill_w, height - 4, fill_color, pitch);
    }
}

// Draw an arbitrary RGBA pixel buffer (pre-flipped) to the framebuffer
static void boot_draw_image(uint32_t* fb, int img_x, int img_y, uint32_t* pixels, int img_w, int img_h, uint32_t pitch) {
    for (int py = 0; py < img_h; py++) {
        for (int px = 0; px < img_w; px++) {
            uint32_t pixel = pixels[py * img_w + px];
            uint8_t a = (pixel >> 24) & 0xFF;
            if (a < 10) continue; // Skip transparent pixels
            putpixel(fb, img_x + px, img_y + py, pixel, pitch);
        }
    }
}

// Shows the boot screen. If boot_img is non-NULL, displays it centered above the progress bar.
// boot_img_w and boot_img_h specify the image dimensions.
void show_boot_screen(uint32_t* boot_img, int boot_img_w, int boot_img_h) {
    int screen_w = (int)gfx_width;
    int screen_h = (int)gfx_height;
    uint32_t pitch = gfx_pitch;

    // Background color - dark navy
    uint32_t bg_color = 0x001A1A2E;

    // Clear screen
    boot_draw_rect(gfx_framebuffer, 0, 0, screen_w, screen_h, bg_color, pitch);

    uint32_t text_color = 0x00FFFFFF;
    uint32_t status_color = 0x00AAAAAA;

    int bar_width = 320;
    int bar_height = 22;
    int bar_x = (screen_w - bar_width) / 2;
    int bar_y;

    if (boot_img && boot_img_w > 0 && boot_img_h > 0) {
        // Center image above progress bar
        int img_x = (screen_w - boot_img_w) / 2;
        int img_y = (screen_h - boot_img_h - bar_height - 80) / 2;
        if (img_y < 10) img_y = 10;

        boot_draw_image(gfx_framebuffer, img_x, img_y, boot_img, boot_img_w, boot_img_h, pitch);
        bar_y = img_y + boot_img_h + 30;
    } else {
        // Fallback: show "Nao OS" text centered
        const char* title = "Nao OS";
        int text_len = 0;
        while (title[text_len]) text_len++;
        int title_x = (screen_w - (text_len * 8)) / 2;
        int title_y = screen_h / 2 - 40;

        boot_draw_string(gfx_framebuffer, title_x, title_y, title, 0x0044AAFF, pitch);
        boot_draw_string(gfx_framebuffer, title_x, title_y + 20, "Loading...", status_color, pitch);
        bar_y = title_y + 60;
    }

    // Animate progress bar
    const char* stages[] = {
        "Initializing system...",
        "Loading drivers...",
        "Mounting storage...",
        "Starting network...",
        "Preparing desktop..."
    };
    int num_stages = sizeof(stages) / sizeof(stages[0]);

    for (int pct = 0; pct <= 100; pct++) {
        // Draw progress bar
        boot_draw_progress_bar(gfx_framebuffer, bar_x, bar_y, bar_width, bar_height, pct, pitch);

        // Draw percentage text
        char pct_str[8];
        if (pct == 100) {
            pct_str[0] = '1'; pct_str[1] = '0'; pct_str[2] = '0'; pct_str[3] = '%'; pct_str[4] = '\0';
        } else if (pct >= 10) {
            pct_str[0] = (pct / 10) + '0';
            pct_str[1] = (pct % 10) + '0';
            pct_str[2] = '%'; pct_str[3] = '\0';
        } else {
            pct_str[0] = pct + '0';
            pct_str[1] = '%'; pct_str[2] = '\0';
        }

        // Clear previous percentage text area
        boot_draw_rect(gfx_framebuffer, bar_x + bar_width + 10, bar_y, 50, bar_height, bg_color, pitch);
        boot_draw_string(gfx_framebuffer, bar_x + bar_width + 12, bar_y + (bar_height - 8) / 2, pct_str, text_color, pitch);

        // Draw status text below bar
        int stage_idx = (pct * num_stages) / 100;
        if (stage_idx >= num_stages) stage_idx = num_stages - 1;

        boot_draw_rect(gfx_framebuffer, bar_x, bar_y + bar_height + 5, bar_width, 14, bg_color, pitch);
        int status_len = 0;
        while (stages[stage_idx][status_len]) status_len++;
        int status_x = bar_x + (bar_width - (status_len * 8)) / 2;
        boot_draw_string(gfx_framebuffer, status_x, bar_y + bar_height + 5, stages[stage_idx], status_color, pitch);

        // Wait ~1 timer tick (~55ms) between steps
        int target_wait = (pct >= 90) ? 2 : 1;
        uint32_t timeout_ticks = timer_ticks + target_wait;
        while ((int)timer_ticks < (int)timeout_ticks) {
            if ((int)(timer_ticks - timeout_ticks) > 1000) break;
        }
    }

    // Brief pause at 100%
    uint32_t pause_target = timer_ticks + 5;
    while ((int)timer_ticks < (int)pause_target) {
        if ((int)(timer_ticks - pause_target) > 1000) break;
    }

    // Clear boot screen for transition to desktop
    boot_draw_rect(gfx_framebuffer, 0, 0, screen_w, screen_h, bg_color, pitch);
}

#endif /* BOOT_SCREEN_H */