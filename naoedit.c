#include <stdint.h>
#include "ff.h"
#define NK_PRIVATE
#include "nuklear.h"

// External kernel dependencies 
extern int mouse_left_clicked;
extern void serial_printf(const char* format, ...);
extern void ani_play(void);
extern void ani_stop(void);
extern uint32_t* gfx_framebuffer;
extern uint32_t gfx_width;
extern uint32_t gfx_height;
extern uint32_t gfx_pitch;
extern uint32_t back_buffer[];
extern volatile uint32_t timer_ticks; // CRITICAL: Need timer_ticks for non-blocking delta tracking

// Exported application state flags for kernel.c integration
int naoedit_active = 0;
int naoedit_minimized = 0;

#define MAX_TEXT_LEN 4096
static char text_buffer[MAX_TEXT_LEN] = "Welcome to NaoEdit!\nType your text here...";
static int text_len = 41; 

static char filepath_buffer[128] = "0:/NOTES.TXT";
static int show_open_dialog = 0;
static int show_save_dialog = 0;

// Non-blocking operation state machine
// 0 = Idle, 1 = Pending Open Delay, 2 = Pending Save Delay
static int file_op_state = 0;
static uint32_t file_op_start_tick = 0;

// Floating Alert window state: 0 = None, 1 = Success Notice, 2 = Error Warning
static int alert_state = 0;
static char alert_message[64] = "";

static void execute_actual_open(const char* path) {
    FIL fp;
    FRESULT fr;
    UINT bytes_read;

    serial_printf("[NAOEDIT] Executing delayed file read: '%s'\n", path);
    fr = f_open(&fp, path, FA_READ | FA_OPEN_EXISTING);
    if (fr == FR_OK) {
        fr = f_read(&fp, text_buffer, MAX_TEXT_LEN - 1, &bytes_read);
        if (fr == FR_OK) {
            text_buffer[bytes_read] = '\0';
            text_len = bytes_read;
            serial_printf("[NAOEDIT] Success. Read out %d bytes from '%s'.\n", bytes_read, path);
            alert_state = 0; 
        } else {
            serial_printf("[NAOEDIT ERROR] f_read pipeline fail. FatFs code: %d\n", fr);
            alert_state = 2;
            int idx = 0; char* msg = "Failed to read file data.";
            while(msg[idx]) { alert_message[idx] = msg[idx]; idx++; } alert_message[idx] = '\0';
        }
        f_close(&fp);
    } else {
        serial_printf("[NAOEDIT ERROR] f_open failed. FatFs code: %d\n", fr);
        alert_state = 2;
        int idx = 0; char* msg = "File not found / Invalid path.";
        while(msg[idx]) { alert_message[idx] = msg[idx]; idx++; } alert_message[idx] = '\0';
    }
}

static void execute_actual_save(const char* path) {
    FIL fp;
    FRESULT fr;
    UINT bytes_written;

    serial_printf("[NAOEDIT] Executing delayed disk write: '%s'\n", path);
    fr = f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_OK) {
        fr = f_write(&fp, text_buffer, text_len, &bytes_written);
        if (fr == FR_OK) {
            serial_printf("[NAOEDIT] Success. Safely storage flushed %d bytes into '%s'\n", bytes_written, path);
            alert_state = 1;
            int idx = 0; char* msg = "File written securely to disk.";
            while(msg[idx]) { alert_message[idx] = msg[idx]; idx++; } alert_message[idx] = '\0';
        } else {
            serial_printf("[NAOEDIT ERROR] FatFs write operation rejected. Code: %d\n", fr);
            alert_state = 2;
            int idx = 0; char* msg = "Storage write cycle failed.";
            while(msg[idx]) { alert_message[idx] = msg[idx]; idx++; } alert_message[idx] = '\0';
        }
        f_close(&fp);
    } else {
        serial_printf("[NAOEDIT ERROR] Write safety access denied. Code: %d\n", path, fr);
        alert_state = 2;
        int idx = 0; char* msg = "Cannot open path for writing.";
        while(msg[idx]) { alert_message[idx] = msg[idx]; idx++; } alert_message[idx] = '\0';
    }
}

void render_naoedit(struct nk_context* ctx, int* active_drag_window_id) {
    if (!naoedit_active || naoedit_minimized) return;

    // --- NON-BLOCKING DELAY TIMER HANDLER ---
    if (file_op_state != 0) {
        // Wait out 60 ticks (~3.3 seconds at 18Hz) while letting the UI render smoothly every frame
        if ((timer_ticks - file_op_start_tick) >= 30) {
            ani_stop(); // Turn off spinning cursor animation
            
            if (file_op_state == 1) {
                execute_actual_open(filepath_buffer);
            } else if (file_op_state == 2) {
                execute_actual_save(filepath_buffer);
            }
            file_op_state = 0; // Return to idle state
        }
    }

    // --- MAIN WINDOW ---
    if (nk_begin(ctx, "NaoEdit", nk_rect(100, 80, 450, 350),
        NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE)) 
    {
        // 1. Menu Bar
        nk_menubar_begin(ctx);
        nk_layout_row_static(ctx, 25, 45, 1);
        if (nk_menu_begin_label(ctx, "File", NK_TEXT_LEFT, nk_vec2(120, 100))) {
            nk_layout_row_dynamic(ctx, 22, 1);
            if (nk_menu_item_label(ctx, "Open...", NK_TEXT_LEFT)) {
                if (file_op_state == 0) { // Only allow if not currently waiting out a file action
                    show_open_dialog = 1;
                    show_save_dialog = 0;
                }
            }
            if (nk_menu_item_label(ctx, "Save As...", NK_TEXT_LEFT)) {
                if (file_op_state == 0) {
                    show_save_dialog = 1;
                    show_open_dialog = 0;
                }
            }
            nk_menu_end(ctx);
        }
        nk_menubar_end(ctx);

        // 2. Open/Save Action Fields
        if (show_open_dialog || show_save_dialog) {
            nk_layout_row_dynamic(ctx, 25, 1);
            nk_label(ctx, show_open_dialog ? "Target Path to Open:" : "Target Path to Save As:", NK_TEXT_LEFT);
            
            nk_layout_row_template_begin(ctx, 30);
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_push_static(ctx, 60);
            nk_layout_row_template_push_static(ctx, 60);
            nk_layout_row_template_end(ctx);

            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, filepath_buffer, sizeof(filepath_buffer) - 1, nk_filter_default);
            
            if (nk_button_label(ctx, "OK")) {
                if (file_op_state == 0) {
                    // Turn on cursor spinning animation
                    ani_play();
                    
                    // Set up state flags to start our non-blocking counter
                    file_op_start_tick = timer_ticks;
                    file_op_state = show_open_dialog ? 1 : 2; 
                }
                show_open_dialog = 0;
                show_save_dialog = 0;
            }
            if (nk_button_label(ctx, "Cancel")) {
                show_open_dialog = 0;
                show_save_dialog = 0;
            }
        }

        // 3. Text Edit Area
        int offset = (show_open_dialog || show_save_dialog) ? 60 : 0;
        nk_layout_row_dynamic(ctx, 260 - offset, 1);
        nk_edit_string_zero_terminated(ctx, 
            NK_EDIT_BOX | NK_EDIT_SELECTABLE | NK_EDIT_MULTILINE, 
            text_buffer, MAX_TEXT_LEN - 1, nk_filter_default);
        
        text_len = 0;
        while (text_buffer[text_len] != '\0') text_len++;
    }

    // Main window focus check
    if (nk_window_has_focus(ctx) || *active_drag_window_id == 4) {
        if (mouse_left_clicked && *active_drag_window_id == 0) {
            *active_drag_window_id = 4;
        }
    } else if (nk_window_is_hovered(ctx) && mouse_left_clicked && *active_drag_window_id == 0) {
        if (!nk_item_is_any_active(ctx)) {
            *active_drag_window_id = 4;
            nk_window_set_focus(ctx, "NaoEdit");
        }
    }
    nk_end(ctx);

    if (nk_window_is_hidden(ctx, "NaoEdit")) {
        naoedit_active = 0;
        alert_state = 0;
        file_op_state = 0;
        ani_stop();
    }

    // --- FLOATING POPUP DIALOG WINDOW ---
    if (alert_state != 0) {
        if (nk_begin(ctx, "NaoEdit Alert", nk_rect(200, 160, 280, 120),
            NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE)) 
        {
            nk_layout_row_dynamic(ctx, 30, 1);
            nk_label(ctx, alert_message, NK_TEXT_CENTERED);
            
            nk_layout_row_static(ctx, 25, 70, 1);
            if (nk_button_label(ctx, "OK")) {
                alert_state = 0;
                *active_drag_window_id = 0; 
                ctx->active = 0; 
                nk_window_set_focus(ctx, "NaoEdit");
                nk_end(ctx);
                return; 
            }
        }

        // Handle Popup Dragging & Focus (Window ID 5 assigned for the modal alert)
        if (nk_window_has_focus(ctx) || *active_drag_window_id == 5) {
            if (mouse_left_clicked && *active_drag_window_id == 0) {
                *active_drag_window_id = 5;
            }
        } else if (nk_window_is_hovered(ctx) && mouse_left_clicked && *active_drag_window_id == 0) {
            *active_drag_window_id = 5;
            nk_window_set_focus(ctx, "NaoEdit Alert");
        }
        nk_end(ctx);

        if (nk_window_is_hidden(ctx, "NaoEdit Alert")) {
            alert_state = 0;
            *active_drag_window_id = 0;
            ctx->active = 0;
            nk_window_set_focus(ctx, "NaoEdit");
            return; 
        }
    }
}