#include "ff.h"
#include "ani_loader.h"
#include "cursor_loader.h"

extern uint32_t* gfx_framebuffer;
extern uint32_t gfx_width;
extern uint32_t gfx_height;
extern uint32_t gfx_pitch;
extern int mouse_x, mouse_y;
extern void serial_printf(const char* format, ...);
extern volatile uint32_t timer_ticks;

// Standard Windows .ani format structures (RIFF-based)
// RIFF: "RIFF" + size + "ACON"
//   "anih" chunk: ANI header (36 bytes)
//   "seq " chunk: optional frame sequence
//   "icon" chunks: each is a .cur file (frame data)

#define ANI_MAX_FRAMES 16
#define ANI_MAX_CURSOR_W 32
#define ANI_MAX_CURSOR_H 32

typedef struct {
    uint32_t duration_jiffies;  // duration in 1/60th seconds (from iRate)
    uint32_t width;
    uint32_t height;
    int16_t hotspot_x;
    int16_t hotspot_y;
    uint8_t pixels[ANI_MAX_CURSOR_W * ANI_MAX_CURSOR_H * 4];
} ani_frame_t;

static ani_frame_t ani_frames[ANI_MAX_FRAMES];
static int ani_frame_count = 0;
static int ani_active = 0;
static int ani_current_frame = 0;
static uint32_t ani_frame_start_tick = 0;

// Convert jiffies (1/60 sec) to timer ticks (1/18 sec)
// jiffies * 18 / 60 = ticks
static uint32_t jiffies_to_ticks(uint32_t jiffies) {
    return (jiffies * 18 + 30) / 60;  // round to nearest
}

// Read a 4-byte little-endian uint32 from a buffer
static uint32_t read_le32(const uint8_t* buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

// Read a 2-byte little-endian uint16 from a buffer
static uint16_t read_le16(const uint8_t* buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

// Parse a .cur file from memory and extract frame data
// Returns 1 on success, 0 on failure
static int parse_cur_frame(const uint8_t* cur_data, uint32_t cur_size, ani_frame_t* frame) {
    if (cur_size < 22) {
        serial_printf("[ANI] .cur data too small (%u bytes)\n", cur_size);
        return 0;
    }

    // Verify .cur magic (type must be 2)
    uint16_t type = read_le16(cur_data + 2);
    if (type != 2) {
        serial_printf("[ANI] Invalid .cur type in frame: %d\n", type);
        return 0;
    }

    uint32_t cw = cur_data[6] ? cur_data[6] : 256;
    uint32_t ch = cur_data[7] ? cur_data[7] : 256;
    int16_t hx = (int16_t)read_le16(cur_data + 10);
    int16_t hy = (int16_t)read_le16(cur_data + 12);
    uint32_t data_offset = read_le32(cur_data + 18);

    // Clamp dimensions
    if (cw > ANI_MAX_CURSOR_W) cw = ANI_MAX_CURSOR_W;
    if (ch > ANI_MAX_CURSOR_H) ch = ANI_MAX_CURSOR_H;

    frame->width = cw;
    frame->height = ch;
    frame->hotspot_x = hx;
    frame->hotspot_y = hy;

    // Read BMP info header (40 bytes) to find color depth
    if (data_offset + 40 > cur_size) {
        serial_printf("[ANI] .cur data offset out of bounds\n");
        return 0;
    }
    const uint8_t* bmi = cur_data + data_offset;
    uint16_t bpp = read_le16(bmi + 14);
    uint32_t pixel_start = data_offset + 40;

    // Read pixel data
    uint32_t pixel_bytes = cw * ch * 4;
    if (pixel_start + pixel_bytes > cur_size) {
        // Try with fewer bytes for 24-bit
        if (bpp == 24) {
            pixel_bytes = cw * ch * 3;
        }
        if (pixel_start + pixel_bytes > cur_size) {
            serial_printf("[ANI] .cur pixel data truncated\n");
            return 0;
        }
    }

    // Clear frame pixels to transparent magenta
    for (uint32_t i = 0; i < cw * ch * 4; i++) {
        frame->pixels[i] = 0;
    }

    if (bpp == 32) {
        // 32-bit with alpha
        for (uint32_t y = 0; y < ch; y++) {
            for (uint32_t x = 0; x < cw; x++) {
                uint32_t src_idx = (y * cw + x) * 4;
                uint32_t dst_idx = ((ch - 1 - y) * cw + x) * 4;  // flip Y
                frame->pixels[dst_idx + 0] = cur_data[pixel_start + src_idx + 0];  // B
                frame->pixels[dst_idx + 1] = cur_data[pixel_start + src_idx + 1];  // G
                frame->pixels[dst_idx + 2] = cur_data[pixel_start + src_idx + 2];  // R
                frame->pixels[dst_idx + 3] = cur_data[pixel_start + src_idx + 3];  // A
            }
        }
    } else if (bpp == 24) {
        // 24-bit RGB (no alpha)
        for (uint32_t y = 0; y < ch; y++) {
            for (uint32_t x = 0; x < cw; x++) {
                uint32_t src_idx = (y * cw + x) * 3;
                uint32_t dst_idx = ((ch - 1 - y) * cw + x) * 4;
                frame->pixels[dst_idx + 0] = cur_data[pixel_start + src_idx + 0];  // B
                frame->pixels[dst_idx + 1] = cur_data[pixel_start + src_idx + 1];  // G
                frame->pixels[dst_idx + 2] = cur_data[pixel_start + src_idx + 2];  // R
                frame->pixels[dst_idx + 3] = 255;  // fully opaque
            }
        }
    } else {
        serial_printf("[ANI] Unsupported .cur bpp: %d\n", bpp);
        return 0;
    }

    return 1;
}

void ani_init(void) {
    ani_frame_count = 0;
    ani_active = 0;
    ani_current_frame = 0;
    ani_frame_start_tick = 0;
}

int ani_load(const char* path) {
    FIL file;
    UINT br;

    ani_init();

    if (f_open(&file, path, FA_READ) != FR_OK) {
        serial_printf("[ANI] Failed to open %s\n", path);
        return 0;
    }

    DWORD file_size = f_size(&file);
    serial_printf("[ANI] File size: %lu bytes\n", file_size);

    // Read the entire file into memory for parsing
    // (RIFF parsing is much easier with random access)
    uint8_t* file_data = (uint8_t*)0;
    // Use a static buffer since we're in kernel space
    static uint8_t ani_file_buf[65536];  // 64KB max for .ani files
    if (file_size > sizeof(ani_file_buf)) {
        serial_printf("[ANI] File too large (%lu bytes, max %u)\n", file_size, sizeof(ani_file_buf));
        f_close(&file);
        return 0;
    }
    file_data = ani_file_buf;
    if (f_read(&file, file_data, file_size, &br) != FR_OK || br != file_size) {
        serial_printf("[ANI] Failed to read file\n");
        f_close(&file);
        return 0;
    }
    f_close(&file);

    // Parse RIFF header
    if (file_size < 12 || file_data[0] != 'R' || file_data[1] != 'I' || file_data[2] != 'F' || file_data[3] != 'F') {
        serial_printf("[ANI] Not a RIFF file\n");
        return 0;
    }

    uint32_t riff_size = read_le32(file_data + 4);
    if (file_data[8] != 'A' || file_data[9] != 'C' || file_data[10] != 'O' || file_data[11] != 'N') {
        serial_printf("[ANI] Not an ACON (ANI cursor) file\n");
        return 0;
    }

    serial_printf("[ANI] Valid RIFF ACON file (%u bytes)\n", riff_size + 8);

    // Parse chunks
    uint32_t offset = 12;
    uint32_t anih_rate = 60;  // default: 1 second (60 jiffies)
    uint32_t anih_frames = 0;
    uint32_t anih_steps = 0;
    int has_anih = 0;
    uint8_t seq_buffer[256];  // sequence buffer (parsed but not used for playback order)
    int seq_count = 0;

    while (offset + 8 <= file_size) {
        char chunk_id[5];
        chunk_id[0] = file_data[offset];
        chunk_id[1] = file_data[offset + 1];
        chunk_id[2] = file_data[offset + 2];
        chunk_id[3] = file_data[offset + 3];
        chunk_id[4] = '\0';
        uint32_t chunk_size = read_le32(file_data + offset + 4);
        uint32_t chunk_end = offset + 8 + chunk_size;
        // Align to 2 bytes
        if (chunk_end & 1) chunk_end++;

        serial_printf("[ANI] Chunk: '%s' size=%u\n", chunk_id, chunk_size);

        if (chunk_id[0] == 'a' && chunk_id[1] == 'n' && chunk_id[2] == 'i' && chunk_id[3] == 'h') {
            // ANI header chunk (36 bytes)
            if (chunk_size >= 36 && offset + 8 + 36 <= file_size) {
                const uint8_t* anih = file_data + offset + 8;
                anih_frames = read_le32(anih + 4);   // cFrames
                anih_steps = read_le32(anih + 8);    // cSteps
                // cx, cy at +12, +16
                anih_rate = read_le32(anih + 20);    // iRate (jiffies, 1/60 sec)
                if (anih_rate == 0) anih_rate = 60;  // default to 1 second
                has_anih = 1;
                serial_printf("[ANI] Header: frames=%u, steps=%u, rate=%u jiffies\n", anih_frames, anih_steps, anih_rate);
            }
        } else if (chunk_id[0] == 's' && chunk_id[1] == 'e' && chunk_id[2] == 'q' && chunk_id[3] == ' ') {
            // Sequence chunk - list of frame indices to play
            seq_count = chunk_size / 4;
            if (seq_count > 256) seq_count = 256;
            for (int i = 0; i < seq_count; i++) {
                seq_buffer[i] = (uint8_t)read_le32(file_data + offset + 8 + i * 4);
            }
            serial_printf("[ANI] Sequence: %d entries\n", seq_count);
        } else if (chunk_id[0] == 'L' && chunk_id[1] == 'I' && chunk_id[2] == 'S' && chunk_id[3] == 'T') {
            // LIST chunk - parse sub-chunks inside (icon frames)
            uint32_t list_start = offset + 8;
            uint32_t list_end = offset + 8 + chunk_size;
            if (list_end & 1) list_end++;
            
            uint32_t sub_offset = list_start;
            while (sub_offset + 8 <= list_end && ani_frame_count < ANI_MAX_FRAMES) {
                char sub_id[5];
                sub_id[0] = file_data[sub_offset];
                sub_id[1] = file_data[sub_offset + 1];
                sub_id[2] = file_data[sub_offset + 2];
                sub_id[3] = file_data[sub_offset + 3];
                sub_id[4] = '\0';
                uint32_t sub_size = read_le32(file_data + sub_offset + 4);
                uint32_t sub_end = sub_offset + 8 + sub_size;
                if (sub_end & 1) sub_end++;

                if (sub_id[0] == 'i' && sub_id[1] == 'c' && sub_id[2] == 'o' && sub_id[3] == 'n') {
                    // Icon chunk inside LIST - contains a .cur file
                    const uint8_t* cur_data = file_data + sub_offset + 8;
                    if (parse_cur_frame(cur_data, sub_size, &ani_frames[ani_frame_count])) {
                        ani_frames[ani_frame_count].duration_jiffies = anih_rate;
                        serial_printf("[ANI] Frame %d: %dx%d, hotspot(%d,%d), rate=%u jiffies\n",
                                      ani_frame_count,
                                      ani_frames[ani_frame_count].width,
                                      ani_frames[ani_frame_count].height,
                                      ani_frames[ani_frame_count].hotspot_x,
                                      ani_frames[ani_frame_count].hotspot_y,
                                      anih_rate);
                        ani_frame_count++;
                    }
                }

                sub_offset = sub_end;
            }
            serial_printf("[ANI] LIST contained %d icon frames\n", ani_frame_count);
        } else if (chunk_id[0] == 'i' && chunk_id[1] == 'c' && chunk_id[2] == 'o' && chunk_id[3] == 'n') {
            // Icon chunk at top level (not in LIST)
            if (ani_frame_count < ANI_MAX_FRAMES) {
                const uint8_t* cur_data = file_data + offset + 8;
                uint32_t cur_size = chunk_size;
                if (parse_cur_frame(cur_data, cur_size, &ani_frames[ani_frame_count])) {
                    ani_frames[ani_frame_count].duration_jiffies = anih_rate;
                    serial_printf("[ANI] Frame %d: %dx%d, hotspot(%d,%d), rate=%u jiffies\n",
                                  ani_frame_count,
                                  ani_frames[ani_frame_count].width,
                                  ani_frames[ani_frame_count].height,
                                  ani_frames[ani_frame_count].hotspot_x,
                                  ani_frames[ani_frame_count].hotspot_y,
                                  anih_rate);
                    ani_frame_count++;
                }
            }
        }

        offset = chunk_end;
        if (offset >= file_size) break;
    }

    if (ani_frame_count == 0) {
        serial_printf("[ANI] No frames loaded\n");
        return 0;
    }

    serial_printf("[ANI] Loaded %d frames from %s\n", ani_frame_count, path);
    return 1;
}

void ani_play(void) {
    if (ani_frame_count == 0) return;
    ani_active = 1;
    ani_current_frame = 0;
    ani_frame_start_tick = timer_ticks;
    serial_printf("[ANI] Animation started (%d frames)\n", ani_frame_count);
}

void ani_stop(void) {
    ani_active = 0;
    ani_current_frame = 0;
    serial_printf("[ANI] Animation stopped\n");
}

int ani_update(uint32_t current_tick) {
    if (!ani_active || ani_frame_count == 0) return 0;

    uint32_t elapsed = current_tick - ani_frame_start_tick;
    uint32_t total_duration = 0;

    // Calculate total duration of all frames in ticks
    for (int i = 0; i < ani_frame_count; i++) {
        total_duration += jiffies_to_ticks(ani_frames[i].duration_jiffies);
    }

    if (total_duration == 0) {
        ani_current_frame = 0;
    } else {
        uint32_t mod_elapsed = elapsed % total_duration;
        uint32_t accum = 0;
        ani_current_frame = ani_frame_count - 1;
        for (int i = 0; i < ani_frame_count; i++) {
            accum += jiffies_to_ticks(ani_frames[i].duration_jiffies);
            if (mod_elapsed < accum) {
                ani_current_frame = i;
                break;
            }
        }
    }

    return 1;
}

void ani_draw(int m_x, int m_y) {
    if (!ani_active || ani_frame_count == 0) return;

    ani_frame_t* frame = &ani_frames[ani_current_frame];
    if (frame->width == 0 || frame->height == 0) return;

    extern uint32_t back_buffer[];

    int start_x = m_x - frame->hotspot_x;
    int start_y = m_y - frame->hotspot_y;

    for (uint32_t y = 0; y < frame->height; y++) {
        for (uint32_t x = 0; x < frame->width; x++) {
            int scr_x = start_x + x;
            int scr_y = start_y + y;

            if (scr_x < 0 || scr_x >= (int)gfx_width || scr_y < 0 || scr_y >= (int)gfx_height) continue;

            uint32_t idx = (y * frame->width + x) * 4;

            uint8_t b = frame->pixels[idx + 0];
            uint8_t g = frame->pixels[idx + 1];
            uint8_t r = frame->pixels[idx + 2];
            uint8_t a = frame->pixels[idx + 3];

            // Pure magenta fallback transparency check or true alpha channel check
            if ((r == 255 && g == 0 && b == 255) || a == 0) continue;

            back_buffer[scr_y * (int)gfx_width + scr_x] = (r << 16) | (g << 8) | b;
        }
    }
}

int ani_is_active(void) {
    return ani_active;
}

// Blocking delay that animates the cursor while waiting
void ani_delay_ticks(uint32_t ticks, uint32_t* bb, uint32_t gw, uint32_t gh, uint32_t gp) {
    if (!ani_active || ani_frame_count == 0) {
        uint32_t start = timer_ticks;
        while ((timer_ticks - start) < ticks) {
            // Spin
        }
        return;
    }

    uint32_t start = timer_ticks;
    uint32_t last_frame_draw = 0;

    while ((timer_ticks - start) < ticks) {
        ani_update(timer_ticks);

        if (timer_ticks != last_frame_draw) {
            last_frame_draw = timer_ticks;
            ani_draw(mouse_x, mouse_y);

            for (uint32_t y = 0; y < gh; y++) {
                for (uint32_t x = 0; x < gw; x++) {
                    gfx_framebuffer[y * (gp / 4) + x] = bb[y * gw + x];
                }
            }
        }
    }
}