#include <stdint.h>
volatile uint8_t ide_interrupt_fired = 0;
extern void irq0(void);
extern void irq1(void);
extern void serial_printf(const char* format, ...);
extern void irq14(void);
// Keyboard buffer to store typed characters safely between frame loops
char keyboard_buffer[32];
int keyboard_head = 0;
int keyboard_tail = 0;

void keyboard_push_char(char c) {
    int next = (keyboard_head + 1) % 32;
    if (next != keyboard_tail) {
        keyboard_buffer[keyboard_head] = c;
        keyboard_head = next;
    }
}

char keyboard_pop_char(void) {
    if (keyboard_head == keyboard_tail) return 0;
    char c = keyboard_buffer[keyboard_tail];
    keyboard_tail = (keyboard_tail + 1) % 32;
    return c;
}

// Track whether a shift key is currently held down (0 = false, 1 = true)
static int shift_pressed = 0;

// Regular USA layout (no Shift)
static const char scancode_to_ascii_normal[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
};

// Shifted USA layout (Shift held down)
static const char scancode_to_ascii_shift[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
};

int mouse_left_clicked = 0;
int mouse_right_clicked = 0;
struct idt_entry_struct {
    uint16_t base_low;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr_struct {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

struct idt_entry_struct idt[256];
struct idt_ptr_struct idt_ptr;

// External assembly functions
extern void idt_load(void);
extern void isr0(void);
extern void isr3(void);

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel     = sel;
    idt[num].always0 = 0;
    idt[num].flags   = flags; // 0x8E is commonly used for kernel interrupt gates
}

// Inline assembly helpers to talk to hardware ports
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

// Remap the PIC controllers so hardware IRQs don't conflict with exceptions
void pic_remap(void) {
    uint8_t a1, a2;

    // Save masks
    a1 = inb(0x21);
    a2 = inb(0xA1);

    // Start initialization sequence (ICW1)
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    // Define vector offsets (ICW2)
    outb(0x21, 0x20); // Master PIC vectors start at 0x20 (32)
    outb(0xA1, 0x28); // Slave PIC vectors start at 0x28 (40)

    // Tell Master PIC there is a slave PIC at IRQ2 (ICW3)
    outb(0x21, 0x04);
    // Tell Slave PIC its cascade identity (ICW3)
    outb(0xA1, 0x02);

    // Set mode to 8086/8088 (ICW4)
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    // Restore masks (0x00 enables all hardware interrupts)
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

// External assembly stub declaration
extern void irq12(void);

// Helper to wait until the keyboard/mouse controller is ready to accept a command
void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        while (timeout--) {
            if ((inb(0x64) & 1) == 1) return; // Data ready to be read
        }
    } else {
        while (timeout--) {
            if ((inb(0x64) & 2) == 0) return; // Ready to write
        }
    }
}

void mouse_write(uint8_t write) {
    mouse_wait(1);
    outb(0x64, 0xD4); // Tell controller to send command to mouse
    mouse_wait(1);
    outb(0x60, write); // Write command byte
}

uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

void init_mouse(void) {
    uint8_t status;

    // Enable the auxiliary mouse device
    mouse_wait(1);
    outb(0x64, 0xA8);
    
    // Enable mouse interrupts
    mouse_wait(1);
    outb(0x64, 0x20); // Command: Read Compaq Status Byte
    mouse_wait(0);
    status = (inb(0x60) | 2); // Set bit 1 (Enable IRQ 12)
    mouse_wait(1);
    outb(0x64, 0x60); // Command: Write Status Byte
    mouse_wait(1);
    outb(0x60, status);
    
    // Tell the mouse to use default configurations
    mouse_write(0xF6);
    mouse_read();  // Acknowledge byte
    
    // Start streaming packets!
    mouse_write(0xF4);
    mouse_read();  // Acknowledge byte
    
    serial_printf("[DEBUG] PS/2 Mouse streaming active.\n");
}

void init_idt(void) {
    idt_ptr.limit = (sizeof(struct idt_entry_struct) * 256) - 1;
    idt_ptr.base  = (uint32_t)&idt;

    // Clear IDT memory out
    for(int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    // Point IDT entries to our Assembly stubs
    idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);
    idt_set_gate(3, (uint32_t)isr3, 0x08, 0x8E);
    
    // Remap the PIC first
    pic_remap();

    // Map Interrupt 32 to our timer assembly stub
    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E);
    
    // Map Interrupt 33 to our keyboard assembly stub
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);
    
    // Map Interrupt 44 to our mouse assembly stub
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    // Call our initialization sequence
    init_mouse();
    idt_load();
}

// Struct representing the CPU registers saved by our assembly stub
struct registers {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
};

int cursor_pos = 0; // Track where to print characters on screen

// Global framebuffer variables that kernel.c will fill out for us
uint32_t* gfx_framebuffer = 0;
uint32_t  gfx_width = 0;
uint32_t  gfx_height = 0;
uint32_t  gfx_pitch = 0;

uint32_t timer_ticks = 0;

// Helper function to draw a solid pixel block (for tracking/indicators)
void idt_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!gfx_framebuffer) return;
    for (int curr_y = y; curr_y < y + h; curr_y++) {
        for (int curr_x = x; curr_x < x + w; curr_x++) {
            if (curr_x >= 0 && curr_x < (int)gfx_width && curr_y >= 0 && curr_y < (int)gfx_height) {
                uint32_t* pixel_address = (uint32_t*)((uint8_t*)gfx_framebuffer + curr_y * gfx_pitch + curr_x * 4);
                *pixel_address = color;
            }
        }
    }
}

int mouse_x = 500;
int mouse_y = 400;
uint8_t mouse_cycle = 0;
int8_t mouse_packet[3];

void isr_handler(struct registers regs) {
    // Handle Timer Interrupt (IRQ 0)
    if (regs.int_no == 32) {
        timer_ticks++;
        
        // Every 18 ticks (~1 second), blink a small 20x20 green square in the top-right corner
        if (timer_ticks % 18 == 0) {
            idt_draw_rect(gfx_width - 30, 10, 20, 20, 0x00FF00); // Bright Green
        } else if (timer_ticks % 18 == 9) {
            idt_draw_rect(gfx_width - 30, 10, 20, 20, 0x002B36); // Back to background color
        }

        outb(0x20, 0x20);
    }
    // Handle Keyboard Interrupt (IRQ 1)
    else if (regs.int_no == 33) {
        uint8_t scancode = inb(0x60);
        
        // Check for Shift Key Press events
        if (scancode == 0x2A || scancode == 0x36) {
            shift_pressed = 1;
        }
        // Check for Shift Key Release events (bit 7 set / scancode + 0x80)
        else if (scancode == 0xAA || scancode == 0xB6) {
            shift_pressed = 0;
        }
        // Handle regular key translation
        else if (!(scancode & 0x80)) { // Key press only
            if (scancode < sizeof(scancode_to_ascii_normal)) {
                // Select the correct lookup table depending on our shift state flag
                char ascii = shift_pressed ? scancode_to_ascii_shift[scancode] 
                                           : scancode_to_ascii_normal[scancode];
                if (ascii != 0) {
                    keyboard_push_char(ascii);
                }
            }
        }
        
        outb(0x20, 0x20);
    }
    // Handle Mouse Interrupt (IRQ 12)
    // --- UPDATE YOUR MOUSE INTERRUPT SEGMENT INSIDE isr_handler ---
    else if (regs.int_no == 44) {
        uint8_t status = inb(0x64);
        
        // If ANY data byte is sitting on the port, we must pull it off
        if (status & 0x01) {
            uint8_t mouse_byte = inb(0x60);

            // Only parse it if it actually belongs to the auxiliary mouse device
            if (status & 0x20) {
                if (mouse_cycle == 0 && !(mouse_byte & 0x08)) {
                    mouse_cycle = 0;
                } else {
                    mouse_packet[mouse_cycle++] = mouse_byte;
                }

                if (mouse_cycle == 3) {
                    mouse_cycle = 0;
                    uint8_t flags = mouse_packet[0];
                    int16_t rel_x = mouse_packet[1];
                    int16_t rel_y = mouse_packet[2];

                    mouse_left_clicked = (flags & 0x01);
                    mouse_right_clicked = (flags & 0x02) ? 1 : 0;
                    if (flags & 0x10) rel_x |= 0xFF00;
                    if (flags & 0x20) rel_y |= 0xFF00;

                    mouse_x += rel_x;
                    mouse_y -= rel_y;

                    if (mouse_x < 0) mouse_x = 0;
                    if (mouse_y < 0) mouse_y = 0;
                    if (mouse_x > (int)gfx_width - 10) mouse_x = gfx_width - 10;
                    if (mouse_y > (int)gfx_height - 10) mouse_y = gfx_height - 10;
                }
            }
        }
        
        outb(0xA0, 0x20);
        outb(0x20, 0x20);
    }
    else if (regs.int_no == 46) {
        serial_printf("[HW INT] IRQ 14 Fired!\n");
        // 1. Read ATA status register (Port 0x1F7) to acknowledge hardware state
        inb(0x1F7); 

        // 2. Set sync notification true
        ide_interrupt_fired = 1;

        // 3. Signal PIC End-Of-Interrupt
        outb(0xA0, 0x20); // Slave PIC
        outb(0x20, 0x20); // Master PIC
    }
    else {
        if (regs.int_no >= 32 && regs.int_no <= 47) {
            if (regs.int_no >= 40) outb(0xA0, 0x20);
            outb(0x20, 0x20);
        } else {
            serial_printf("[FATAL INTERRUPT FLAG DETECTED] CPU tripped critical Exception ID: %d, Error Code: %d\n", regs.int_no, regs.err_code);
            serial_printf("[EIP Address location]: %d\n", regs.eip);
            while(1); 
        }
    }
}