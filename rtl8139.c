#include <stdint.h>
#include <string.h>

#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

// RTL8139 Register Offsets
#define REG_MAC0         0x00     // MAC Address
#define REG_MAR0         0x08     // Multicast Filter
#define REG_TXSTATUS0    0x10     // Transmit Status (4 registers, 4 bytes each)
#define REG_TXADDR0      0x20     // Transmit Start Address (4 registers)
#define REG_RBSTART      0x30     // Receive Buffer Start Address
#define REG_COMMAND      0x37     // Command Register
#define REG_CAPR         0x38     // Current Address of Packet Read
#define REG_IMR          0x3C     // Interrupt Mask Register
#define REG_ISR          0x3E     // Interrupt Status Register
#define REG_TCR          0x40     // Transmit Configuration Register
#define REG_RCR          0x44     // Receive Configuration Register
#define REG_CONFIG1      0x52     // Configuration Register 1

// Driver State Variables
static uint32_t io_base = 0; // Changed to uint32_t to match 32-bit BAR allocations safely
static uint8_t  rx_buffer[8192 + 16 + 1518] __attribute__((aligned(4))); // 8KB + margins
static uint32_t rx_offset = 0;

static uint8_t  tx_buffers[4][1536] __attribute__((aligned(4)));
static uint8_t  tx_counter = 0;
uint8_t  mac_address[6] = {0};

// Forward declaration of external printf
extern void serial_printf(const char* fmt, ...);

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %b0, %w1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    asm volatile("outw %w0, %w1" : : "a"(val), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %w1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %w1, %b0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile("inw %w1, %w0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile("inl %w1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Read values from PCI space - Using standard 32-bit BAR handling logic
uint32_t pci_read_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    
    asm volatile("outl %0, %w1" : : "a"(address), "Nd"((uint16_t)0xCF8));
    
    uint32_t val;
    asm volatile("inl %w1, %0" : "=a"(val) : "Nd"((uint16_t)0xCFC));
    
    if (offset == 0x10) {
        return val; // Return raw 32-bit BAR0
    }
    
    return (val >> ((offset & 2) * 8)) & 0xFFFF;
}

void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    asm volatile("outl %0, %w1" : : "a"(address), "Nd"((uint16_t)0xCF8));
    asm volatile("outl %0, %w1" : : "a"(val), "Nd"((uint16_t)0xCFC));
}

// Scans PCI space for the RTL8139 card with verbose diagnostics
int init_rtl8139(void) {
    serial_printf("[RTL8139 DEBUG] Starting PCI Bus Scan...\n");
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t vendor = pci_read_config(bus, slot, 0, 0x00);
            uint32_t device = pci_read_config(bus, slot, 0, 0x02);
            
            if (vendor == RTL8139_VENDOR_ID && device == RTL8139_DEVICE_ID) {
                
                
                uint32_t bar0 = pci_read_config(bus, slot, 0, 0x10);
                // Fallback to simple %x for your printf engine
                
                
                io_base = bar0 & ~0x3;
                
                
                if (io_base == 0 || io_base == 0xFFFC) {
                    
                    return 0;
                }

                uint32_t pci_cmd = pci_read_config(bus, slot, 0, 0x04);
                pci_cmd |= (1 << 2); 
                pci_write_config_dword(bus, slot, 0, 0x04, pci_cmd);
                
                
                
                outb(io_base + REG_CONFIG1, 0x00);
                
                
                outb(io_base + REG_COMMAND, 0x10);
                
                uint32_t timeout = 0;
                while ((inb(io_base + REG_COMMAND) & 0x10) != 0) {
                    timeout++;
                    if (timeout > 1000000) {
                        
                        return 0;
                    }
                }
                
                
                for(int i = 0; i < 6; i++) {
                    mac_address[i] = inb(io_base + REG_MAC0 + i);
                }
                // Printing MAC address elements via standard %d to avoid %02X parser bugs
                serial_printf("[RTL8139 DEBUG] MAC: %d:%d:%d:%d:%d:%d\n", 
                              mac_address[0], mac_address[1], mac_address[2], 
                              mac_address[3], mac_address[4], mac_address[5]);
                
                outl(io_base + REG_RBSTART, (uint32_t)&rx_buffer[0]);
                outw(io_base + REG_IMR, 0x0000); 
                outl(io_base + REG_RCR, 0x0000000F | (1 << 7)); 
                outb(io_base + REG_COMMAND, 0x0C); 
                
                
                return 1; 
            }
        }
    }
    
    return 0; 
}

// --- TRANSMIT FUNCTION WITH ACCURATE FAULT INTERCEPTORS ---

void nic_transmit_packet(uint8_t* buffer, uint32_t length) {
    

    if (io_base == 0 || io_base == 0xFFFC) {
        
        return;
    }

    if (buffer == 0) {
        
        return;
    }

    if (length > 1536 || length == 0) {
        
        return;
    }

    serial_printf("[RTL8139 TRACE] Copying %d bytes to TX Buffer #%d at memory: 0x%x\n", 
                  length, (int)tx_counter, (uint32_t)tx_buffers[tx_counter]);
    
    // Explicit safety wrap around memcpy
    memcpy(tx_buffers[tx_counter], buffer, length);
    
    uint32_t addr_reg = io_base + REG_TXADDR0 + (tx_counter * 4);
    uint32_t status_reg = io_base + REG_TXSTATUS0 + (tx_counter * 4);
    
    
    outl(addr_reg, (uint32_t)tx_buffers[tx_counter]);
    
    
    outl(status_reg, length & 0x1FFF);

    tx_counter = (tx_counter + 1) % 4;
    
}

uint32_t nic_poll_received_packet(uint8_t* buffer) {
    if (io_base == 0) return 0;

    if (inb(io_base + REG_COMMAND) & 0x01) {
        return 0; 
    }

    uint32_t offset = rx_offset;
    uint16_t* packet_header = (uint16_t*)&rx_buffer[offset];
    uint16_t status = packet_header[0];
    uint16_t length = packet_header[1];

    if (status & 0x01) {
        serial_printf("[RTL8139 HW] Packet Detected! Size: %d\n", length - 4);
        memcpy(buffer, &rx_buffer[offset + 4], length - 4);

        rx_offset = (offset + length + 4 + 3) & ~3;
        if (rx_offset >= 8192) {
            rx_offset -= 8192;
        }

        outw(io_base + REG_CAPR, rx_offset - 0x10);
        return length - 4; 
    } else {
        if (length == 0 || length > 1536) length = 64;
        rx_offset = (offset + length + 4 + 3) & ~3;
        if (rx_offset >= 8192) rx_offset -= 8192;
        outw(io_base + REG_CAPR, rx_offset - 0x10);
    }

    return 0;
}