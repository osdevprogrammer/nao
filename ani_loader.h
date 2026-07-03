#ifndef ANI_LOADER_H
#define ANI_LOADER_H

#include <stdint.h>

#define ANI_MAX_FRAMES 16
#define ANI_MAX_CURSOR_W 32
#define ANI_MAX_CURSOR_H 32

// Callback type for drawing a cursor frame
typedef void (*ani_draw_frame_fn)(int mouse_x, int mouse_y);

// Initialize the ANI system
void ani_init(void);

// Load an animated cursor from an .ani file
// Returns 1 on success, 0 on failure
int ani_load(const char* path);

// Start playing the loaded animation
void ani_play(void);

// Stop the current animation
void ani_stop(void);

// Update animation state - call this each frame
// Returns 1 if animation is active, 0 if not
int ani_update(uint32_t current_tick);

// Draw the current frame of the animation
void ani_draw(int mouse_x, int mouse_y);

// Check if animation is currently active
int ani_is_active(void);

// Wait for a specified number of timer ticks while animating
// This blocks until the delay expires, drawing the animation each frame
void ani_delay_ticks(uint32_t ticks, uint32_t* back_buffer, uint32_t gfx_width, uint32_t gfx_height, uint32_t gfx_pitch);

#endif