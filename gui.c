#include <stdint.h>

extern void serial_printf(const char* format, ...);
#define NK_ASSERT(expr) if (!(expr)) { serial_printf("[NUKLEAR ASSERT FAIL] Line %d\n", __LINE__); while(1); }

#define NK_IMPLEMENTATION
#include "nuklear.h"
#include "font.h"

extern uint32_t  gfx_width;
extern uint32_t  gfx_height;
extern void putpixel(uint32_t* fb, int x, int y, uint32_t color, uint32_t pitch);
extern void* kmalloc(uint32_t size);
extern void kfree(void* ptr);

float os_font_get_text_width(nk_handle handle, float height, const char *text, int len) {
    return (float)(len * 8);
}

void* nk_kmalloc_hook(nk_handle unused, void *old_ptr, nk_size size) {
    (void)unused; (void)old_ptr;
    return kmalloc(size);
}

void nk_kfree_hook(nk_handle unused, void *ptr) {
    (void)unused;
    kfree(ptr);
}

void init_nk_backend(struct nk_context *ctx, struct nk_user_font *nk_font) {
    nk_font->userdata = nk_handle_ptr(0);
    nk_font->height = 8.0f;
    nk_font->width = os_font_get_text_width;
    
    serial_printf("[GUI DEBUG] Initializing Nuklear with Dynamic Kernel Heap Memory Allocations...\n");
    
    struct nk_allocator alloc;
    alloc.userdata = nk_handle_ptr(0);
    alloc.alloc = nk_kmalloc_hook;
    alloc.free = nk_kfree_hook;

    if (!nk_init(ctx, &alloc, nk_font)) {
        serial_printf("[FATAL] Nuklear dynamic heap context initialization failed!\n");
        while(1);
    }
    serial_printf("[GUI DEBUG] Nuklear dynamic backend successfully operational.\n");
}


static int scissor_x = 0;
static int scissor_y = 0;
static int scissor_w = 1024; 
static int scissor_h = 768;

// --- CRITICAL UPDATE: ALPHA-BLENDED PRIMITIVE RENDER TARGET ---
void idt_draw_rect_target(uint32_t* target_fb, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint32_t target_pitch = gfx_width * 4;
    uint32_t opaque_color = (r << 16) | (g << 8) | b;
    
    for (int curr_y = y; curr_y < y + h; curr_y++) {
        for (int curr_x = x; curr_x < x + w; curr_x++) {
            if (curr_x >= scissor_x && curr_x < (scissor_x + scissor_w) &&
                curr_y >= scissor_y && curr_y < (scissor_y + scissor_h)) {
                
                if (curr_x >= 0 && curr_x < (int)gfx_width && curr_y >= 0 && curr_y < (int)gfx_height) {
                    if (a == 255) {
                        // Completely Opaque: Quick overwrite shortcut
                        putpixel(target_fb, curr_x, curr_y, opaque_color, target_pitch);
                    } else if (a > 0) {
                        // Linear Alpha Blending math with the underlying background pixels
                        uint32_t bg_pixel = target_fb[curr_y * gfx_width + curr_x];
                        
                        uint8_t bg_r = (bg_pixel >> 16) & 0xFF;
                        uint8_t bg_g = (bg_pixel >> 8)  & 0xFF;
                        uint8_t bg_b = bg_pixel         & 0xFF;

                        uint8_t out_r = ((r * a) + (bg_r * (255 - a))) / 255;
                        uint8_t out_g = ((g * a) + (bg_g * (255 - a))) / 255;
                        uint8_t out_b = ((b * a) + (bg_b * (255 - a))) / 255;

                        uint32_t blended_color = (out_r << 16) | (out_g << 8) | out_b;
                        putpixel(target_fb, curr_x, curr_y, blended_color, target_pitch);
                    }
                    // If a == 0, we do nothing (fully transparent)
                }
            }
        }
    }
}
#define ROUND_TOP_LEFT     (1 << 0)
#define ROUND_TOP_RIGHT    (1 << 1)
#define ROUND_BOTTOM_LEFT  (1 << 2)
#define ROUND_BOTTOM_RIGHT (1 << 3)
#define ROUND_ALL_CORNERS  (ROUND_TOP_LEFT | ROUND_TOP_RIGHT | ROUND_BOTTOM_LEFT | ROUND_BOTTOM_RIGHT)

void idt_draw_rect_rounded_target(uint32_t* target_fb, int x, int y, int w, int h, 
                                  uint16_t rounding, uint8_t r, uint8_t g, uint8_t b, uint8_t a, 
                                  int filled, uint8_t corner_flags) 
{
    uint32_t target_pitch = gfx_width * 4;
    uint32_t opaque_color = (r << 16) | (g << 8) | b;

    for (int curr_y = y; curr_y < y + h; curr_y++) {
        for (int curr_x = x; curr_x < x + w; curr_x++) {
            
            if (curr_x >= scissor_x && curr_x < (scissor_x + scissor_w) &&
                curr_y >= scissor_y && curr_y < (scissor_y + scissor_h)) {
                
                if (curr_x >= 0 && curr_x < (int)gfx_width && curr_y >= 0 && curr_y < (int)gfx_height) {
                    
                    int is_corner = 0;
                    int cx = 0, cy = 0;

                    // Match corners dynamically, validating permissions via the corner_flags bitmask
                    if ((corner_flags & ROUND_TOP_LEFT) && (curr_x < x + rounding && curr_y < y + rounding)) {
                        is_corner = 1; cx = x + rounding; cy = y + rounding;
                    } else if ((corner_flags & ROUND_TOP_RIGHT) && (curr_x >= x + w - rounding && curr_y < y + rounding)) {
                        is_corner = 1; cx = x + w - rounding - 1; cy = y + rounding;
                    } else if ((corner_flags & ROUND_BOTTOM_LEFT) && (curr_x < x + rounding && curr_y >= y + h - rounding)) {
                        is_corner = 1; cx = x + rounding; cy = y + h - rounding - 1;
                    } else if ((corner_flags & ROUND_BOTTOM_RIGHT) && (curr_x >= x + w - rounding && curr_y >= y + h - rounding)) {
                        is_corner = 1; cx = x + w - rounding - 1; cy = y + h - rounding - 1;
                    }

                    if (is_corner) {
                        int dx = curr_x - cx;
                        int dy = curr_y - cy;
                        int dist_sq = dx * dx + dy * dy;
                        int r_sq = rounding * rounding;

                        if (filled) {
                            if (dist_sq > r_sq) continue;
                        } else {
                            int inner_r = rounding - 1;
                            if (dist_sq > r_sq || dist_sq < (inner_r * inner_r)) continue;
                        }
                    } else if (!filled) {
                        if (curr_x > x && curr_x < x + w - 1 && curr_y > y && curr_y < y + h - 1) {
                            continue;
                        }
                    }

                    // Fixed-Point Alpha Blending
                    if (a == 255) {
                        putpixel(target_fb, curr_x, curr_y, opaque_color, target_pitch);
                    } else if (a > 0) {
                        uint32_t bg_pixel = target_fb[curr_y * gfx_width + curr_x];
                        uint8_t bg_r = (bg_pixel >> 16) & 0xFF;
                        uint8_t bg_g = (bg_pixel >> 8)  & 0xFF;
                        uint8_t bg_b = bg_pixel         & 0xFF;

                        uint8_t out_r = ((r * a) + (bg_r * (255 - a))) / 255;
                        uint8_t out_g = ((g * a) + (bg_g * (255 - a))) / 255;
                        uint8_t out_b = ((b * a) + (bg_b * (255 - a))) / 255;

                        uint32_t blended_color = (out_r << 16) | (out_g << 8) | out_b;
                        putpixel(target_fb, curr_x, curr_y, blended_color, target_pitch);
                    }
                }
            }
        }
    }
}
void nk_os_render_target(struct nk_context *ctx, uint32_t* target_buffer) {
    const struct nk_command *cmd;
    
    scissor_x = 0;
    scissor_y = 0;
    scissor_w = (int)gfx_width;
    scissor_h = (int)gfx_height;
    
    nk_foreach(cmd, ctx) {
        switch (cmd->type) {
            case NK_COMMAND_RECT: {
                const struct nk_command_rect *r = (const struct nk_command_rect*)cmd;
                if (r->rounding > 0) {
                    // Check if this looks like a title bar border vs a full window wrapper frame
                    uint8_t flags = (r->h < 35) ? (ROUND_TOP_LEFT | ROUND_TOP_RIGHT) : ROUND_ALL_CORNERS;
                    idt_draw_rect_rounded_target(target_buffer, r->x, r->y, r->w, r->h, r->rounding,
                                                 r->color.r, r->color.g, r->color.b, r->color.a, 0, flags);
                } else {
                    idt_draw_rect_target(target_buffer, r->x, r->y, r->w, 1, r->color.r, r->color.g, r->color.b, r->color.a);
                    idt_draw_rect_target(target_buffer, r->x, r->y + r->h - 1, r->w, 1, r->color.r, r->color.g, r->color.b, r->color.a);
                    idt_draw_rect_target(target_buffer, r->x, r->y, 1, r->h, r->color.r, r->color.g, r->color.b, r->color.a);
                    idt_draw_rect_target(target_buffer, r->x + r->w - 1, r->y, 1, r->h, r->color.r, r->color.g, r->color.b, r->color.a);
                }
                break;
            }
            case NK_COMMAND_LINE: {
                const struct nk_command_line *l = (const struct nk_command_line*)cmd;
                
                int dx = (l->end.x - l->begin.x);
                int dy = (l->end.y - l->begin.y);
                
                int abs_dx = (dx < 0) ? -dx : dx;
                int abs_dy = (dy < 0) ? -dy : dy;
                
                int steps = (abs_dx > abs_dy) ? abs_dx : abs_dy;
                
                if (steps == 0) {
                    idt_draw_rect_target(target_buffer, l->begin.x, l->begin.y, 1, 1, l->color.r, l->color.g, l->color.b, l->color.a);
                } else {
                    float x_inc = (float)dx / (float)steps;
                    float y_inc = (float)dy / (float)steps;
                    float current_x = l->begin.x;
                    float current_y = l->begin.y;
                    
                    for (int i = 0; i <= steps; i++) {
                        idt_draw_rect_target(target_buffer, (int)current_x, (int)current_y, 1, 1, l->color.r, l->color.g, l->color.b, l->color.a);
                        current_x += x_inc;
                        current_y += y_inc;
                    }
                }
                break;
            }
            case NK_COMMAND_SCISSOR: {
                const struct nk_command_scissor *s = (const struct nk_command_scissor*)cmd;
                scissor_x = s->x;
                scissor_y = s->y;
                scissor_w = s->w;
                scissor_h = s->h;
                break;
            }
            case NK_COMMAND_RECT_FILLED: {
                const struct nk_command_rect_filled *r = (const struct nk_command_rect_filled*)cmd;
                if (r->rounding > 0) {
                    uint8_t flags = ROUND_ALL_CORNERS;
                    
                    // SAFE BACKEND TRICK: Instead of referencing current_theme across files,
                    // check if the color matches the context's window header title bar color!
                    if (r->h < 35 && ctx && 
                        r->color.r == ctx->style.window.header.normal.data.color.r && 
                        r->color.g == ctx->style.window.header.normal.data.color.g) 
                    {
                        flags = (ROUND_TOP_LEFT | ROUND_TOP_RIGHT);
                    }
                    
                    idt_draw_rect_rounded_target(target_buffer, r->x, r->y, r->w, r->h, r->rounding,
                                                 r->color.r, r->color.g, r->color.b, r->color.a, 1, flags);
                } else {
                    idt_draw_rect_target(target_buffer, r->x, r->y, r->w, r->h, r->color.r, r->color.g, r->color.b, r->color.a);
                }
                break;
            }
            case NK_COMMAND_TEXT: {
                const struct nk_command_text *t = (const struct nk_command_text*)cmd;
                for (int i = 0; i < t->length; i++) {
                    int char_x = t->x + (i * 8);
                    int char_y = t->y;
                    
                    for (int row = 0; row < 8; row++) {
                        for (int col = 0; col < 8; col++) {
                            int pixel_x = char_x + col;
                            int pixel_y = char_y + row;
                            
                            if (pixel_x >= scissor_x && pixel_x < (scissor_x + scissor_w) &&
                                pixel_y >= scissor_y && pixel_y < (scissor_y + scissor_h)) {
                                
                                extern const uint8_t font_bitmap[128][8];
                                if ((uint8_t)t->string[i] <= 127) {
                                    uint8_t row_data = font_bitmap[(uint8_t)t->string[i]][row];
                                    if (row_data & (0x80 >> col)) {
                                        // Renders UI font bytes with proper alpha channels mapping
                                        idt_draw_rect_target(target_buffer, pixel_x, pixel_y, 1, 1, t->foreground.r, t->foreground.g, t->foreground.b, t->foreground.a);
                                    }
                                }
                            }
                        }
                    }
                }
                break;
            }
            
            case NK_COMMAND_IMAGE: {
                const struct nk_command_image *img = (const struct nk_command_image*)cmd;
                if (img->img.handle.ptr == (void*)0) break;

                uint32_t *source_pixels = (uint32_t*)img->img.handle.ptr;
                
                int start_x = img->x;
                int start_y = img->y;
                int img_w   = img->w;
                int img_h   = img->h;

                for (int y_offset = 0; y_offset < img_h; y_offset++) {
                    for (int x_offset = 0; x_offset < img_w; x_offset++) {
                        int pixel_x = start_x + x_offset;
                        int pixel_y = start_y + y_offset;

                        if (pixel_x >= scissor_x && pixel_x < (scissor_x + scissor_w) &&
                            pixel_y >= scissor_y && pixel_y < (scissor_y + scissor_h)) {

                            if (pixel_x >= 0 && pixel_x < (int)gfx_width && pixel_y >= 0 && pixel_y < (int)gfx_height) {
                                
                                uint32_t raw_pixel = source_pixels[y_offset * img_w + x_offset];
                                uint8_t a = (raw_pixel >> 24) & 0xFF;

                                if (a == 255) {
                                    putpixel(target_buffer, pixel_x, pixel_y, raw_pixel & 0x00FFFFFF, gfx_width * 4);
                                } else if (a > 0) {
                                    uint32_t bg_pixel = target_buffer[pixel_y * gfx_width + pixel_x];
                                    
                                    uint8_t src_r = (raw_pixel >> 16) & 0xFF;
                                    uint8_t src_g = (raw_pixel >> 8)  & 0xFF;
                                    uint8_t src_b = raw_pixel         & 0xFF;

                                    uint8_t bg_r = (bg_pixel >> 16) & 0xFF;
                                    uint8_t bg_g = (bg_pixel >> 8)  & 0xFF;
                                    uint8_t bg_b = bg_pixel         & 0xFF;

                                    uint8_t out_r = ((src_r * a) + (bg_r * (255 - a))) / 255;
                                    uint8_t out_g = ((src_g * a) + (bg_g * (255 - a))) / 255;
                                    uint8_t out_b = ((src_b * a) + (bg_b * (255 - a))) / 255;

                                    uint32_t blended_color = (out_r << 16) | (out_g << 8) | out_b;
                                    putpixel(target_buffer, pixel_x, pixel_y, blended_color, gfx_width * 4);
                                }
                            }
                        }
                    }
                }
                break;
            }
            default: 
                break;
        }
    }
    
    nk_clear(ctx);
}