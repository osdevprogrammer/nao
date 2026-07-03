#include "ff.h"
#define NK_PRIVATE
#include "nuklear.h"

// External kernel states/variables
extern int mouse_left_clicked;
extern int mouse_x;
extern int mouse_y;
extern void serial_printf(const char* format, ...);

// Exported states for kernel.c
int file_explorer_active = 0;
int file_explorer_minimized = 0;

#define MAX_FILES_PER_DIR 32
#define MAX_FILENAME_LEN  64

typedef struct {
    char name[MAX_FILENAME_LEN];
    uint32_t size;
    int is_directory;
} FileEntry;

static FileEntry current_dir_items[MAX_FILES_PER_DIR];
static int current_item_count = 0;
static char current_path[128] = "0:";
static int explorer_needs_refresh = 1;

// Context Menu States
static int show_context_menu = 0;
static struct nk_vec2 context_menu_pos;
static int context_target_idx = -1; 
static char target_item_name[MAX_FILENAME_LEN] = ""; 

// Operation Dialog Modal States
static int show_rename_dialog = 0;
static int show_delete_dialog = 0;
static int show_mkdir_dialog  = 0;
static char op_input_buffer[64] = "";

static char* mini_strrchr(const char* str, int ch) {
    char* last = 0;
    while (*str) {
        if (*str == ch) last = (char*)str;
        str++;
    }
    return last;
}

static void build_full_path(char* out_buf, const char* base, const char* name) {
    int i = 0;
    while (base[i]) { out_buf[i] = base[i]; i++; }
    if (base[i - 1] != ':') { out_buf[i++] = '/'; }
    int j = 0;
    while (name[j]) { out_buf[i++] = name[j++]; }
    out_buf[i] = '\0';
    serial_printf("[EXPLORER] Path constructed: '%s'\n", out_buf);
}

static void populate_directory(const char* path) {
    DIR dir;
    FILINFO fno;
    FRESULT res;

    serial_printf("[EXPLORER] Attempting disk read at directory path: '%s'\n", path);
    current_item_count = 0;
    
    res = f_opendir(&dir, path);
    if (res != FR_OK) {
        serial_printf("[EXPLORER ERROR] f_opendir failed on target path '%s'. Error code: %d\n", path, res);
        return;
    }

    while (current_item_count < MAX_FILES_PER_DIR) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK) {
            serial_printf("[EXPLORER ERROR] f_readdir stream broken. Error code: %d\n", res);
            break;
        }
        if (fno.fname[0] == 0) break; 
        if (fno.fname[0] == '.' && fno.fname[1] == 0) continue; 

        FileEntry* item = &current_dir_items[current_item_count];
        int n = 0;
        for (; n < MAX_FILENAME_LEN - 1 && fno.fname[n] != '\0'; n++) {
            item->name[n] = fno.fname[n];
        }
        item->name[n] = '\0';
        item->size = fno.fsize;
        item->is_directory = (fno.fattrib & AM_DIR) ? 1 : 0;
        
        serial_printf("[EXPLORER] Found item[%d]: %s (%s, Size: %d bytes)\n", 
                      current_item_count, item->name, item->is_directory ? "DIR" : "FILE", item->size);
        current_item_count++;
    }
    f_closedir(&dir);
    serial_printf("[EXPLORER] Directory parsing cycle finalized. Total processed items: %d\n", current_item_count);
}

void explorer_trigger_refresh(void) {
    serial_printf("[EXPLORER] Manual internal refresh requested.\n");
    explorer_needs_refresh = 1;
}

static void explorer_go_up(char* path) {
    int len = 0;
    while (path[len] != '\0') len++;
    
    serial_printf("[EXPLORER] Back-navigation evaluation for current raw path: '%s' (Length: %d)\n", path, len);

    if (len <= 2) {
        serial_printf("[EXPLORER] Navigation abort: already sitting at absolute root disk mount.\n");
        return;
    }

    if (path[len - 1] == '/') {
        path[len - 1] = '\0';
        len--;
    }

    int i = len - 1;
    while (i > 2 && path[i] != '/') {
        i--;
    }

    if (path[i] == '/') {
        if (i == 2) {
            path[3] = '\0';
        } else {
            path[i] = '\0';
        }
    }
    serial_printf("[EXPLORER] Path adjusted backward to: '%s'\n", path);
}
static FRESULT empty_and_delete_dir(char* path) {
    DIR dir;
    FILINFO fno;
    FRESULT res = f_opendir(&dir, path);
    if (res != FR_OK) return res;

    while (1) {
        res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;
        if (fno.fname[0] == '.') continue;

        char sub_path[256];
        build_full_path(sub_path, path, fno.fname);

        if (fno.fattrib & AM_DIR) {
            res = empty_and_delete_dir(sub_path);
        } else {
            res = f_unlink(sub_path);
        }
        if (res != FR_OK) break;
    }
    f_closedir(&dir);
    
    if (res == FR_OK) {
        res = f_unlink(path); // Finally delete the main folder itself
    }
    return res;
}
void render_file_explorer(struct nk_context* ctx, int* active_drag_window_id) {
    if (!file_explorer_active || file_explorer_minimized) return;

    if (explorer_needs_refresh) {
        populate_directory(current_path);
        explorer_needs_refresh = 0;
    }

    int right_clicked = nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_RIGHT);
    int left_clicked  = nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT);

    float cm_w = 130;
    float cm_h = (context_target_idx != -1) ? 60 : 36;
    struct nk_rect cm_rect = nk_rect(context_menu_pos.x, context_menu_pos.y, cm_w, cm_h);

    if (show_context_menu && left_clicked && !nk_input_is_mouse_hovering_rect(&ctx->input, cm_rect)) {
        serial_printf("[EXPLORER] Left-click outside context boundaries. Dismissing context view.\n");
        show_context_menu = 0;
    }

    // --- MAIN EXPLORER WINDOW ---
    if (nk_begin(ctx, "File Explorer", nk_rect(200, 120, 400, 320),
        NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE)) 
    {
        nk_layout_row_begin(ctx, NK_STATIC, 25, 3);
        nk_layout_row_push(ctx, 35);
        if (nk_button_label(ctx, "..")) {
            serial_printf("[EXPLORER] '..' navigation widget click registered.\n");
            explorer_go_up(current_path);
            explorer_needs_refresh = 1;
        }
        
        nk_layout_row_push(ctx, 60);
        if (nk_button_label(ctx, "Refresh")) {
            serial_printf("[EXPLORER] 'Refresh' button widget click registered.\n");
            explorer_needs_refresh = 1;
        }
        
        nk_layout_row_push(ctx, 240);
        nk_label(ctx, current_path, NK_TEXT_LEFT);
        nk_layout_row_end(ctx);

        nk_layout_row_dynamic(ctx, 200, 1);
        if (nk_group_begin(ctx, "FileGroup", NK_WINDOW_BORDER)) {
            int item_hovered_this_frame = 0;

            if (current_item_count == 0) {
                nk_layout_row_dynamic(ctx, 30, 1);
                nk_label(ctx, " [Empty Directory]", NK_TEXT_LEFT);
            }

            for (int i = 0; i < current_item_count; i++) {
                char display_name[90] = {0};
                if (current_dir_items[i].is_directory) {
                    int idx = 0; char* prefix = "[DIR]  ";
                    while(prefix[idx]) { display_name[idx] = prefix[idx]; idx++; }
                    int j = 0; while(current_dir_items[i].name[j]) { display_name[idx++] = current_dir_items[i].name[j++]; }
                } else {
                    int idx = 0; char* prefix = "[FILE] ";
                    while(prefix[idx]) { display_name[idx] = prefix[idx]; idx++; }
                    int j = 0; while(current_dir_items[i].name[j]) { display_name[idx++] = current_dir_items[i].name[j++]; }
                }

                nk_layout_row_dynamic(ctx, 22, 1);
                struct nk_rect item_bounds = nk_widget_bounds(ctx);
                
                if (nk_button_label(ctx, display_name)) {
                    serial_printf("[EXPLORER] Selection click verified on item index %d: '%s'\n", i, current_dir_items[i].name);
                    if (current_dir_items[i].is_directory) {
                        int len = 0; 
                        while (current_path[len]) len++;

                        // FIX: If the path is exactly "0:" or ends with a colon, inject the separating slash
                        if (len == 2 && current_path[1] == ':') {
                            current_path[len++] = '/';
                        } else if (len > 0 && current_path[len - 1] != '/' && current_path[len - 1] != ':') {
                            current_path[len++] = '/';
                        }

                        int j = 0; 
                        while (current_dir_items[i].name[j]) {
                            current_path[len++] = current_dir_items[i].name[j++];
                        }
                        current_path[len] = '\0';

                        serial_printf("[EXPLORER] Branching down into nested directory tree: '%s'\n", current_path);
                        explorer_needs_refresh = 1;
                    }
                }

                if (nk_input_is_mouse_hovering_rect(&ctx->input, item_bounds)) {
                    item_hovered_this_frame = 1;
                    if (right_clicked) {
                        show_context_menu = 1;
                        context_menu_pos = nk_vec2(mouse_x, mouse_y);
                        context_target_idx = i;
                        
                        int c = 0;
                        while(current_dir_items[i].name[c]) { target_item_name[c] = current_dir_items[i].name[c]; c++; }
                        target_item_name[c] = '\0';
                        serial_printf("[EXPLORER] Target context menu spawned on item: '%s' at absolute screen coordinates X:%d, Y:%d\n", target_item_name, mouse_x, mouse_y);
                    }
                }
            }

            struct nk_rect group_bounds = nk_widget_bounds(ctx);
            if (!item_hovered_this_frame && right_clicked && nk_input_is_mouse_hovering_rect(&ctx->input, group_bounds)) {
                show_context_menu = 1;
                context_menu_pos = nk_vec2(mouse_x, mouse_y);
                context_target_idx = -1;
                serial_printf("[EXPLORER] Workspace backdrop right-click registered. Target menu context tagged as empty directory workspace.\n");
            }

            nk_group_end(ctx);
        }
    }

    // Focus Lifecycle Routing
    if (nk_window_has_focus(ctx) || *active_drag_window_id == 3 || *active_drag_window_id == 6 || *active_drag_window_id == 7) {
        if (mouse_left_clicked && *active_drag_window_id == 0) {
            *active_drag_window_id = 3;
            serial_printf("[EXPLORER FOCUS] Drag lock mapped exclusively to Main Explorer (ID 3)\n");
        }
    } else if (nk_window_is_hovered(ctx) && mouse_left_clicked && *active_drag_window_id == 0) {
        if (!nk_item_is_any_active(ctx)) {
            *active_drag_window_id = 3;
            nk_window_set_focus(ctx, "File Explorer");
            serial_printf("[EXPLORER FOCUS] Window focus explicitly claimed by 'File Explorer'\n");
        }
    }
    nk_end(ctx);

    if (nk_window_is_hidden(ctx, "File Explorer")) {
        serial_printf("[EXPLORER] Main window hidden flag read from framework UI title frame.\n");
        file_explorer_active = 0;
        show_context_menu = 0;
    }

    // --- FLOATING CONTEXT MENU ---
    if (show_context_menu) {
        if (nk_begin(ctx, "ExplorerContext", cm_rect, NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
            nk_layout_row_dynamic(ctx, 20, 1);
            
            if (context_target_idx != -1) {
                if (nk_button_label(ctx, "Rename")) {
                    serial_printf("[EXPLORER CONTEXT] Menu context operation selected: 'Rename'\n");
                    show_rename_dialog = 1;
                    int idx = 0; while(target_item_name[idx]) { op_input_buffer[idx] = target_item_name[idx]; idx++; } op_input_buffer[idx] = '\0';
                    show_context_menu = 0;
                }
                if (nk_button_label(ctx, "Delete")) {
                    serial_printf("[EXPLORER CONTEXT] Menu context operation selected: 'Delete'\n");
                    show_delete_dialog = 1;
                    show_context_menu = 0;
                }
            } else {
                if (nk_button_label(ctx, "New Folder")) {
                    serial_printf("[EXPLORER CONTEXT] Workspace blank space option selected: 'New Folder'\n");
                    show_mkdir_dialog = 1;
                    op_input_buffer[0] = '\0';
                    show_context_menu = 0;
                }
            }
        }
        nk_end(ctx);
    }

    // --- RENAME MODAL DIALOG POPUP ---
    if (show_rename_dialog) {
        if (nk_begin(ctx, "Rename Item", nk_rect(240, 180, 280, 120),
            NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE)) 
        {
            nk_layout_row_dynamic(ctx, 25, 1);
            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, op_input_buffer, sizeof(op_input_buffer) - 1, nk_filter_default);

            nk_layout_row_static(ctx, 25, 70, 2);
            if (nk_button_label(ctx, "OK")) {
                char path_old[256];
                char path_new[256];
                build_full_path(path_old, current_path, target_item_name);
                build_full_path(path_new, current_path, op_input_buffer);
                
                serial_printf("[EXPLORER MODAL] Processing FatFs f_rename sequence. '%s' -> '%s'\n", path_old, path_new);
                FRESULT fr = f_rename(path_old, path_new);
                serial_printf("[EXPLORER MODAL] f_rename driver completed execution output code: %d\n", fr);
                
                explorer_needs_refresh = 1;
                show_rename_dialog = 0;
                *active_drag_window_id = 0;
                ctx->active = 0;
                nk_window_set_focus(ctx, "File Explorer");
                nk_end(ctx);
                return; // Early Exit Fix
            }
            if (nk_button_label(ctx, "Cancel")) {
                serial_printf("[EXPLORER MODAL] Rename dialog explicitly canceled by user interface click.\n");
                show_rename_dialog = 0;
                *active_drag_window_id = 0;
                ctx->active = 0;
                nk_window_set_focus(ctx, "File Explorer");
                nk_end(ctx);
                return; // Early Exit Fix
            }
        }
        if (nk_window_has_focus(ctx)) { if (mouse_left_clicked) *active_drag_window_id = 7; }
        nk_end(ctx);
        if (nk_window_is_hidden(ctx, "Rename Item")) {
            serial_printf("[EXPLORER MODAL] Rename frame dismissed via native Close header toggle.\n");
            show_rename_dialog = 0;
            *active_drag_window_id = 0;
            ctx->active = 0;
            nk_window_set_focus(ctx, "File Explorer");
            return; // Early Exit Fix
        }
    }

    // --- DELETE CONFIRMATION MODAL POPUP ---
    if (show_delete_dialog) {
        if (nk_begin(ctx, "Confirm Delete", nk_rect(240, 180, 280, 110),
            NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE)) 
        {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Are you sure you want to delete this?", NK_TEXT_CENTERED);

            nk_layout_row_static(ctx, 25, 70, 2);
            if (nk_button_label(ctx, "OK")) {
                char path_del[256];
                build_full_path(path_del, current_path, target_item_name);
                
                serial_printf("[EXPLORER MODAL] Processing FatFs f_unlink target action path: '%s'\n", path_del);
                FRESULT fr = empty_and_delete_dir(path_del);
                serial_printf("[EXPLORER MODAL] f_unlink driver completed execution output code: %d\n", fr);
                
                explorer_needs_refresh = 1;
                show_delete_dialog = 0;
                *active_drag_window_id = 0;
                ctx->active = 0;
                nk_window_set_focus(ctx, "File Explorer");
                nk_end(ctx);
                return; // Early Exit Fix
            }
            if (nk_button_label(ctx, "Cancel")) {
                serial_printf("[EXPLORER MODAL] Delete operation sequence canceled.\n");
                show_delete_dialog = 0;
                *active_drag_window_id = 0;
                ctx->active = 0;
                nk_window_set_focus(ctx, "File Explorer");
                nk_end(ctx);
                return; // Early Exit Fix
            }
        }
        if (nk_window_has_focus(ctx)) { if (mouse_left_clicked) *active_drag_window_id = 7; }
        nk_end(ctx);
        if (nk_window_is_hidden(ctx, "Confirm Delete")) {
            serial_printf("[EXPLORER MODAL] Delete confirmation frame hidden natively.\n");
            show_delete_dialog = 0;
            *active_drag_window_id = 0;
            ctx->active = 0;
            nk_window_set_focus(ctx, "File Explorer");
            return; // Early Exit Fix
        }
    }

    // --- NEW FOLDER MODAL DIALOG POPUP ---
    if (show_mkdir_dialog) {
        if (nk_begin(ctx, "New Folder", nk_rect(240, 180, 280, 120),
            NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE | NK_WINDOW_TITLE)) 
        {
            nk_layout_row_dynamic(ctx, 25, 1);
            nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, op_input_buffer, sizeof(op_input_buffer) - 1, nk_filter_default);

            nk_layout_row_static(ctx, 25, 70, 2);
            if (nk_button_label(ctx, "OK")) {
                char path_make[256];
                build_full_path(path_make, current_path, op_input_buffer);
                
                serial_printf("[EXPLORER MODAL] Processing FatFs f_mkdir target operation directory string: '%s'\n", path_make);
                FRESULT fr = f_mkdir(path_make);
                serial_printf("[EXPLORER MODAL] f_mkdir driver completed execution output code: %d\n", fr);
                
                explorer_needs_refresh = 1;
                show_mkdir_dialog = 0;
                *active_drag_window_id = 0;
                ctx->active = 0;
                nk_window_set_focus(ctx, "File Explorer");
                nk_end(ctx);
                return; // Early Exit Fix
            }
            if (nk_button_label(ctx, "Cancel")) {
                serial_printf("[EXPLORER MODAL] New folder initialization popup interface canceled.\n");
                show_mkdir_dialog = 0;
                *active_drag_window_id = 0;
                ctx->active = 0;
                nk_window_set_focus(ctx, "File Explorer");
                nk_end(ctx);
                return; // Early Exit Fix
            }
        }
        if (nk_window_has_focus(ctx)) { if (mouse_left_clicked) *active_drag_window_id = 7; }
        nk_end(ctx);
        if (nk_window_is_hidden(ctx, "New Folder")) {
            serial_printf("[EXPLORER MODAL] New Folder frame hidden natively.\n");
            show_mkdir_dialog = 0;
            *active_drag_window_id = 0;
            ctx->active = 0;
            nk_window_set_focus(ctx, "File Explorer");
            return; // Early Exit Fix
        }
    }
}