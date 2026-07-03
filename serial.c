#include <stdint.h>
#include <stdarg.h>

#define COM1 0x3F8

// Hardware port I/O helpers
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

// Initialize COM1 serial port
void init_serial(void) {
    outb(COM1 + 1, 0x00);    // Disable all interrupts
    outb(COM1 + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(COM1 + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(COM1 + 1, 0x00);    //                  (hi byte)
    outb(COM1 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(COM1 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

int is_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void write_serial_char(char a) {
    while (is_transmit_empty() == 0);
    outb(COM1, a);
}

void write_serial_string(const char* s) {
    for (int i = 0; s[i] != '\0'; i++) {
        write_serial_char(s[i]);
    }
}

// Minimal helper to format integers to text
void serial_print_int(int num) {
    char buf[32];
    int i = 0;
    if (num == 0) {
        write_serial_char('0');
        return;
    }
    if (num < 0) {
        write_serial_char('-');
        num = -num;
    }
    while (num > 0) {
        buf[i++] = (num % 10) + '0';
        num /= 10;
    }
    for (int j = i - 1; j >= 0; j--) {
        write_serial_char(buf[j]);
    }
}

// Custom simple print utility for tracking
void serial_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    for (int i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%' && format[i+1] != '\0') {
            i++;
            if (format[i] == 's') {
                char* s = va_arg(args, char*);
                write_serial_string(s);
            } else if (format[i] == 'd') {
                int d = va_arg(args, int);
                serial_print_int(d);
            } else if (format[i] == 'c') {
                char c = (char)va_arg(args, int);
                write_serial_char(c);
            }
        } else {
            write_serial_char(format[i]);
        }
    }
    va_end(args);
}