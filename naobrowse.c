#include <stdint.h>
#include <string.h>

// Include your lwIP dependencies
#include "lwip/apps/http_client.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

// Include your GUI framework
#define NK_PRIVATE
#include "nuklear.h"

// External utilities provided by your kernel
extern void serial_printf(const char* format, ...);

#define MAX_URL_LEN      128
#define MAX_PAGE_STORAGE 16384 // 16 KB page body buffer
#define HISTORY_DEPTH    10

// Debug Console configuration parameters
#define DEBUG_LINE_COUNT 25
#define DEBUG_LINE_LEN   96

// NaoBrowse Global State Machine Structure
struct naobrowse_browser {
    char url_input[MAX_URL_LEN];       // Text bound to the URL text box
    char current_url[MAX_URL_LEN];     // The currently fetched page address
    
    char raw_html_buffer[MAX_PAGE_STORAGE]; // Raw inbound stream storage
    char rendered_text[MAX_PAGE_STORAGE];   // Processed text ready for the UI
    uint32_t content_len;
    
    // History Navigation Buffers (Stacks)
    char history[HISTORY_DEPTH][MAX_URL_LEN];
    int history_count;
    int current_history_idx;
    
    int is_loading;
    int is_active; // Is window visible

    // On-screen Live UI Debug logs
    char debug_console[DEBUG_LINE_COUNT][DEBUG_LINE_LEN];
    int debug_write_ptr;
};

// Allocate instance globally
static struct naobrowse_browser browser_instance;

// --- STEP 0: Internal Window UI Debug Logger ---
static void naobrowse_log(const char *msg) {
    struct naobrowse_browser *browser = &browser_instance;
    
    // Also write to COM1 so you have a backup tracking trail
    serial_printf("[NaoBrowse UI Log] %s\n", msg);

    // Roll circular storage buffer forwards
    int idx = browser->debug_write_ptr;
    strncpy(browser->debug_console[idx], msg, DEBUG_LINE_LEN - 1);
    browser->debug_console[idx][DEBUG_LINE_LEN - 1] = '\0';
    
    browser->debug_write_ptr = (idx + 1) % DEBUG_LINE_COUNT;
}

// --- STEP 1: Rudimentary Rendering Engine ---
static void naobrowse_render_engine_parse(struct naobrowse_browser *browser) {
    naobrowse_log("Parser engine triggered: Stripping tags...");
    uint32_t src_idx = 0;
    uint32_t dest_idx = 0;
    int inside_tag = 0;

    memset(browser->rendered_text, 0, MAX_PAGE_STORAGE);

    while (src_idx < browser->content_len && dest_idx < (MAX_PAGE_STORAGE - 2)) {
        char current_char = browser->raw_html_buffer[src_idx];

        if (current_char == '<') {
            inside_tag = 1;
            if (strncmp(&browser->raw_html_buffer[src_idx], "<p>", 3) == 0 || 
                strncmp(&browser->raw_html_buffer[src_idx], "<br>", 4) == 0 ||
                strncmp(&browser->raw_html_buffer[src_idx], "</div>", 6) == 0) {
                browser->rendered_text[dest_idx++] = '\n';
            }
        } else if (current_char == '>') {
            inside_tag = 0;
            src_idx++;
            continue;
        }

        if (!inside_tag) {
            browser->rendered_text[dest_idx++] = current_char;
        }
        src_idx++;
    }
    browser->rendered_text[dest_idx] = '\0';
    naobrowse_log("Parser finished preparing text layer structure.");
}

// --- STEP 2: lwIP HTTP Stream Callbacks ---
static err_t naobrowse_http_body_cb(void *arg, struct altcp_pcb *conn, struct pbuf *p, err_t err) {
    (void)conn; (void)err;
    struct naobrowse_browser *browser = (struct naobrowse_browser *)arg;
    
    if (p != NULL) {
        uint16_t chunk_len = p->tot_len;
        
        char debug_msg[64];
        // Print chunk size directly inside the interface
        mini_snprintf(debug_msg, sizeof(debug_msg), "lwIP Body CB: Received chunk segment of %d bytes", chunk_len);
        naobrowse_log(debug_msg);

        if (browser->content_len + chunk_len < (MAX_PAGE_STORAGE - 1)) {
            pbuf_copy_partial(p, browser->raw_html_buffer + browser->content_len, chunk_len, 0);
            browser->content_len += chunk_len;
            browser->raw_html_buffer[browser->content_len] = '\0';
        } else {
            naobrowse_log("CRITICAL ERROR: Incoming HTML exceeds allocated buffer size limits!");
        }
        pbuf_free(p);
    }
    return ERR_OK;
}

void naobrowse_http_result_cb(void *arg, enum ehttpc_result httpc_result, 
                              unsigned int rx_content_len, signed char err, void *last_pbuf) {
    (void)last_pbuf;
    struct naobrowse_browser *browser = (struct naobrowse_browser *)arg;
    
    naobrowse_log("lwIP Callback Alert: TCP Handshake cycle complete!");
    
    char outcome_msg[96];
    if (httpc_result == 0) { // HTTPC_RESULT_OK
        mini_snprintf(outcome_msg, sizeof(outcome_msg), "SUCCESS! Received HTTP 200/OK. Transferred: %u bytes", rx_content_len);
        naobrowse_log(outcome_msg);
        naobrowse_render_engine_parse(browser);
    } else {
        mini_snprintf(outcome_msg, sizeof(outcome_msg), "FAIL: HTTP Connection Refused. Result: %d, lwIP Error: %d", (int)httpc_result, (int)err);
        naobrowse_log(outcome_msg);
        strcpy(browser->rendered_text, "Error: Could not reach target web host.");
    }
    
    browser->is_loading = 0; 
}

// --- STEP 3: Central Network Dispatcher ---
void naobrowse_navigate_to(const char *url_string, int save_to_history) {
    struct naobrowse_browser *browser = &browser_instance;
    
    char nav_log[128];
    mini_snprintf(nav_log, sizeof(nav_log), "Dispatcher action: routing request path to: '%s'", url_string);
    naobrowse_log(nav_log);

    browser->is_loading = 1;
    browser->content_len = 0;
    memset(browser->raw_html_buffer, 0, MAX_PAGE_STORAGE);
    strcpy(browser->current_url, url_string);

    if (save_to_history) {
        naobrowse_log("Saving URL endpoint trace to history backstack...");
        if (browser->current_history_idx < browser->history_count - 1) {
            browser->history_count = browser->current_history_idx + 1;
        }
        if (browser->history_count < HISTORY_DEPTH) {
            strcpy(browser->history[browser->history_count], url_string);
            browser->current_history_idx = browser->history_count;
            browser->history_count++;
        }
    }

    // --- STEP 3: Central Network Dispatcher (Fixed Fragment) ---
    httpc_connection_t settings;
    memset(&settings, 0, sizeof(settings));
    
    // Bind your result callback cleanly
    settings.result_fn = naobrowse_http_result_cb;

    ip_addr_t server_ip;
    naobrowse_log("Binding internal routing table target to QEMU Gateway IP: 10.0.2.2");
    IP4_ADDR(&server_ip, 10, 0, 2, 2);

    naobrowse_log("Invoking asynchronous lwIP network socket routine: httpc_get_file()...");
    
    // CRITICAL FIX: Explicitly clear the tracking handle reference before dispatching.
    // If connection_state isn't NULL, lwIP's HTTP client assumes a transaction is already 
    // mid-flight on this control block and fails to issue the outgoing request payload buffer!
    static httpc_state_t *connection_state = NULL;
    connection_state = NULL; 

    // Use standard root path "/" to grab the index file
    err_t error = httpc_get_file(&server_ip, 80, "/", &settings, naobrowse_http_body_cb, browser, &connection_state);
    
    if (error != ERR_OK) {
        char err_log[64];
        mini_snprintf(err_log, sizeof(err_log), "lwIP Immediate Error: Request could not be queued. Code: %d", error);
        naobrowse_log(err_log);
        
        browser->is_loading = 0;
        strcpy(browser->rendered_text, "NETWORK ROUTING DISPATCH FAILURE: Unreachable Gateway");
    } else {
        naobrowse_log("SYN Packet queued in TX descriptor ring successfully. Handshake completed!");
    }
}

// --- STEP 4: Initialize the Browser UI State ---
void init_naobrowse(void) {
    memset(&browser_instance, 0, sizeof(struct naobrowse_browser));
    strcpy(browser_instance.url_input, "10.0.2.2");
    strcpy(browser_instance.rendered_text, "Welcome to NaoBrowse!\nType an IP Address above to search the web layout network paths.");
    browser_instance.history_count = 0;
    browser_instance.current_history_idx = -1;
    browser_instance.is_loading = 0;
    browser_instance.is_active = 1;
    browser_instance.debug_write_ptr = 0;
    
    naobrowse_log("NaoBrowse Subsystem initialized successfully.");
    naobrowse_log("Awaiting user entry interaction loop...");
}

// --- STEP 5: Nuklear Window UI Compositor ---
void render_naobrowse(struct nk_context *ctx) {
    struct naobrowse_browser *browser = &browser_instance;

    if (!browser->is_active) return;

    // Increased default height from 450 to 520 to make adequate room for the new logging console layout pane
    if (nk_begin(ctx, "NaoBrowse v1.0", nk_rect(80, 40, 650, 520),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | 
                 NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE)) 
    {
        nk_layout_row_template_begin(ctx, 30);
        nk_layout_row_template_push_static(ctx, 35);  // Back button
        nk_layout_row_template_push_static(ctx, 35);  // Forward button
        nk_layout_row_template_push_dynamic(ctx);     // URL Input entry line
        nk_layout_row_template_push_static(ctx, 45);  // Go navigation button
        nk_layout_row_template_end(ctx);

        // --- BUTTON: BACK TRACK ---
        int back_active = (browser->current_history_idx > 0);
        if (!back_active) nk_widget_disable_begin(ctx);
        if (nk_button_label(ctx, "<<")) {
            naobrowse_log("UI Control Action: Back Button Clicked.");
            browser->current_history_idx--;
            strcpy(browser->url_input, browser->history[browser->current_history_idx]);
            naobrowse_navigate_to(browser->url_input, 0);
        }
        if (!back_active) nk_widget_disable_end(ctx);

        // --- BUTTON: FORWARD TRACK ---
        int fwd_active = (browser->current_history_idx < browser->history_count - 1);
        if (!fwd_active) nk_widget_disable_begin(ctx);
        if (nk_button_label(ctx, ">>")) {
            naobrowse_log("UI Control Action: Forward Button Clicked.");
            browser->current_history_idx++;
            strcpy(browser->url_input, browser->history[browser->current_history_idx]);
            naobrowse_navigate_to(browser->url_input, 0);
        }
        if (!fwd_active) nk_widget_disable_end(ctx);

        // --- INPUT FIELD: URL BAR ---
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, browser->url_input, sizeof(browser->url_input) - 1, nk_filter_default);

        // --- BUTTON: GO ---
        if (nk_button_label(ctx, "Go")) {
            naobrowse_log("UI Control Action: 'Go' Button Clicked.");
            browser->is_loading = 1; 
            browser->content_len = 0;
            naobrowse_navigate_to(browser->url_input, 1);
        }
        
        // --- CORE VIEWPORT RENDER AREA ---
        // Dynamically scales to allocate room for viewport, leaving 120px at bottom for the diagnostic system console
        nk_layout_row_dynamic(ctx, nk_window_get_height(ctx) - 215, 1);
        
        if (nk_group_begin(ctx, "WebPagePanel", NK_WINDOW_BORDER | NK_WINDOW_ROM)) {
            if (browser->is_loading) {
                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label(ctx, "Loading resource arrays... Connecting over TCP...", NK_TEXT_CENTERED);
            } else {
                nk_layout_row_dynamic(ctx, nk_window_get_height(ctx) - 230, 1);
                nk_label_wrap(ctx, browser->rendered_text);
            }
            nk_group_end(ctx);
        }

        // --- NEW: ON-SCREEN DIAGNOSTIC TERMINAL LOG PANE ---
        // --- ON-SCREEN DIAGNOSTIC TERMINAL LOG PANE ---
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, "[System Diagnostic Output Log Monitor]", NK_TEXT_LEFT);

        nk_layout_row_dynamic(ctx, 90, 1);
        if (nk_group_begin(ctx, "DebugConsolePanel", NK_WINDOW_BORDER)) { // Removed NK_WINDOW_ROM to allow active scrollbar mouse interaction
            
            // Loop sequentially through circular buffer mapping from back to front
            int start_pos = browser->debug_write_ptr;
            for (int i = 0; i < DEBUG_LINE_COUNT; i++) {
                int read_idx = (start_pos + i) % DEBUG_LINE_COUNT;
                if (browser->debug_console[read_idx][0] != '\0') {
                    // CRITICAL FIX: Every single log string must declare its own layout row properties
                    nk_layout_row_dynamic(ctx, 14, 1);
                    nk_label(ctx, browser->debug_console[read_idx], NK_TEXT_LEFT);
                }
            }
            nk_group_end(ctx);
        }
    }
    nk_end(ctx);
}