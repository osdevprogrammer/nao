#include <stdint.h>
#include "ff.h"
#define NK_PRIVATE
#include "nuklear.h"

// External kernel dependencies 
extern int mouse_left_clicked;
extern void serial_printf(const char* format, ...);
extern void ani_play(void);
extern void ani_stop(void);
extern void ani_delay_ticks(uint32_t ticks, uint32_t* back_buffer, uint32_t gfx_width, uint32_t gfx_height, uint32_t gfx_pitch);
extern uint32_t* gfx_framebuffer;
extern uint32_t gfx_width;
extern uint32_t gfx_height;
extern uint32_t gfx_pitch;
extern uint32_t back_buffer[];

// Exported application state flags for kernel.c integration
int naoedit_active = 0;
int naoedit_minimized = 0;

#define MAX_TEXT_LEN 4096
static char text_buffer[MAX_TEXT_LEN] = "Welcome to NaoEdit!\nType your text here...";
static int text_len = 41; 

static char filepath_buffer[128] = "0:/NOTES.TXT";
static int show_open_dialog = 0;
static int show_save_dialog = 0;

// Floating Alert window state: 0 = None, 1 = Success Notice, 2 = Error Warning
static int alert_state = 0;
static char alert_message[64] = "";

static void naoedit_open_file(const char* path) {
    // Show loading cursor during file open
    ani_play();

    FIL fp;
    FRESULT fr;
    UINT bytes_read;

    serial_printf("[NAOEDIT] Initializing file read cycle request for path location: '%s'\n", path);
    fr = f_open(&fp, path, FA_READ | FA_OPEN_EXISTING);
    if (fr == FR_OK) {
        fr = f_read(&fp, text_buffer, MAX_TEXT_LEN - 1, &bytes_read);
        if (fr == FR_OK) {
            text_buffer[bytes_read] = '\0';
            text_len = bytes_read;
            serial_printf("[NAOEDIT] Success. Read out %d bytes from '%s'.\n", bytes_read, path);
            alert_state = 0; 
        } else {
            serial_printf("[NAOEDIT ERROR] f_read pipeline fail inside file layout vector. FatFs code: %d\n", fr);
            alert_state = 2;
            int idx = 0; char* msg = "Failed to read file data.";
            while(msg[idx]) { alert_message[idx] = msg[idx]; idx++; } alert_message[idx] = '\0';
        }
        f_close(&fp);
    } else {
        serial_printf("[NAOEDIT ERROR] f_open failed to establish stream handle for target path '%s'. FatFs code: %d\n", path, fr);
        alert_state = 2;
        int idx = 0; char* msg = "File not found / Invalid path.";
        while(msg[idx]) { alert_message[idx] = msg[idx]; idx++; } alert_message[idx] = '\0';
    }
}

static void naoedit_save_file(const char* path) {
    // Show loading cursor during file save
    ani_play();

    FIL fp;
    FRESULT fr;
    UINT bytes_written;

    serial_printf("[NAOEDIT] Initializing disk output write cycle request to location path: '%s'\n", path);
    fr = f_open(&fp, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_OK) {
        fr = f_write(&fp, text_buffer, text_len, &bytes_written);
        if (fr == FR_OK) {
            serial_printf("[NAOEDIT] Success. Safely storage flushed %d bytes directly into disk mount path '%s'\n", bytes_written, path);
            alert_state = 1;
            int idx = 0; char* msg = "File written securely to disk.";
            while(msg[idx]) { alert_message[idx] = msg[idx]; idx++; } alert_message[idx] = '\0';
        } else {
            serial_printf("[NAOEDIT ERROR] FatFs hardware sector write operation rejected. Code: %d\n", fr);
            alert_state = 2;
            int idx = 0; char* msg = "Storage write cycle failed.";
            while(msg[idx]) { alert_message[idx] = msg[idx]; idx++; } alert_message[idx] = '\0';
        }
        f_close(&fp);
    } else {
        serial_printf("[NAOEDIT ERROR] Write safety access denied to system path destination '%s'. Code: %d\n", path, fr);
        alert_state = 2;
        int idx = 0; char* msg = "Cannot open path for writing.";
        while(msg[idx]) { alert_message[idx] = msg[idx]; idx++; } alert_message[idx] = '\0';
    }
}

void render_naoedit(struct nk_context* ctx, int* active_drag_window_id) {
    if (!naoedit_active || naoedit_minimized) return;

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
                serial_printf("[NAOEDIT UI] Dropdown interaction action flagged: Open menu item clicked.\n");
                show_open_dialog = 1;
                show_save_dialog = 0;
            }
            if (nk_menu_item_label(ctx, "Save As...", NK_TEXT_LEFT)) {
                serial_printf("[NAOEDIT UI] Dropdown interaction action flagged: Save As menu item clicked.\n");
                show_save_dialog = 1;
                show_open_dialog = 0;
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
                serial_printf("[NAOEDIT UI] Execution action choice field 'OK' toggled.\n");
                if (show_open_dialog) naoedit_open_file(filepath_buffer);
                else naoedit_save_file(filepath_buffer);
                show_open_dialog = 0;
                show_save_dialog = 0;
            }
            if (nk_button_label(ctx, "Cancel")) {
                serial_printf("[NAOEDIT UI] Inline action command row path parameter edit canceled.\n");
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
            serial_printf("[NAOEDIT FOCUS] Active dragging token bound explicitly to parent frame NaoEdit (ID 4).\n");
        }
    } else if (nk_window_is_hovered(ctx) && mouse_left_clicked && *active_drag_window_id == 0) {
        if (!nk_item_is_any_active(ctx)) {
            *active_drag_window_id = 4;
            nk_window_set_focus(ctx, "NaoEdit");
            serial_printf("[NAOEDIT FOCUS] Focused component pointer reassigned entirely to text app window canvas.\n");
        }
    }
    nk_end(ctx);

    if (nk_window_is_hidden(ctx, "NaoEdit")) {
        serial_printf("[NAOEDIT UI] Parent canvas hidden flag received. Ending application pipeline state.\n");
        naoedit_active = 0;
        alert_state = 0;
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
                serial_printf("[NAOEDIT MODAL] Alert popup accepted via 'OK' button.\n");
                alert_state = 0;
                *active_drag_window_id = 0; 
                ctx->active = 0; 
                nk_window_set_focus(ctx, "NaoEdit");
                nk_end(ctx);
                return; // Early Exit Fix
            }
        }

        // Handle Popup Dragging & Focus (Window ID 5 assigned for the modal alert)
        if (nk_window_has_focus(ctx) || *active_drag_window_id == 5) {
            if (mouse_left_clicked && *active_drag_window_id == 0) {
                *active_drag_window_id = 5;
                serial_printf("[NAOEDIT FOCUS] Dragging token assigned explicitly to Alert Popup (ID 5).\n");
            }
        } else if (nk_window_is_hovered(ctx) && mouse_left_clicked && *active_drag_window_id == 0) {
            *active_drag_window_id = 5;
            nk_window_set_focus(ctx, "NaoEdit Alert");
            serial_printf("[NAOEDIT FOCUS] Focus explicitly bound to child alert popup layer window.\n");
        }
        nk_end(ctx);

        if (nk_window_is_hidden(ctx, "NaoEdit Alert")) {
            serial_printf("[NAOEDIT MODAL] Alert dialog header close action registered. Focus returning to principal frame container context.\n");
            alert_state = 0;
            *active_drag_window_id = 0;
            ctx->active = 0;
            nk_window_set_focus(ctx, "NaoEdit");
            return; // Early Exit Fix
        }
    }
}