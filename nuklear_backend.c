#include <stdint.h>

#define NK_PRIVATE
#define NK_IMPLEMENTATION
#include "nuklear.h"

extern uint32_t* gfx_framebuffer;
extern uint32_t  gfx_width;
extern uint32_t  gfx_height;
extern uint32_t  gfx_pitch;

extern void putpixel(uint32_t* fb, int x, int y, uint32_t color, uint32_t pitch);
extern void idt_draw_rect(int x, int y, int w, int h, uint32_t color);

// Bridging Nuklear's commands to our raw frame buffer
void nk_os_render(struct nk_context *ctx) {
    const struct nk_command *cmd;
    
    nk_foreach(cmd, ctx) {
        switch (cmd->type) {
            case NK_COMMAND_SCISSOR:
                // For now, we can ignore advanced clipping/scissors 
                break;
                
            case NK_COMMAND_RECT: {
                const struct nk_command_rect *r = (const struct nk_command_rect*)cmd;
                uint32_t color = (r->color.r << 16) | (r->color.g << 8) | r->color.b;
                idt_draw_rect(r->x, r->y, r->w, r->h, color);
                break;
            }
            
            case NK_COMMAND_RECT_FILLED: {
                const struct nk_command_rect_filled *r = (const struct nk_command_rect_filled*)cmd;
                uint32_t color = (r->color.r << 16) | (r->color.g << 8) | r->color.b;
                idt_draw_rect(r->x, r->y, r->w, r->h, color);
                break;
            }

            // We will expand this to handle text, lines, and circles!
            default:
                break;
        }
    }
    nk_clear(ctx);
}