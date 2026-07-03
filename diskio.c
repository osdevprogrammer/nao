#include "ff.h"
#include "diskio.h"
#include <stddef.h>
// Volatile ensures the compiler doesn't optimize away the read loop checks
// Remove the 'static' version and replace it with this:
extern volatile uint8_t ide_interrupt_fired;
// Standard Primary ATA Bus Ports
#define ATA_REG_DATA        0x1F0
#define ATA_REG_FEATURES    0x1F1
#define ATA_REG_SECTOR_COUNT 0x1F2
#define ATA_REG_LBA_LOW     0x1F3
#define ATA_REG_LBA_MID     0x1F4
#define ATA_REG_LBA_HIGH    0x1F5
#define ATA_REG_DRIVE       0x1F6
#define ATA_REG_COMMAND     0x1F7
#define ATA_REG_STATUS      0x1F7

// ATA Commands & Status Flags
#define ATA_CMD_READ        0x20
#define ATA_CMD_WRITE       0x30
#define ATA_STATUS_BSY      0x80
#define ATA_STATUS_DRQ      0x08
extern void serial_printf(const char* format, ...);
// Inline assembly wrappers for hardware ports
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    // AT&T syntax: inb %dx, %al
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    // AT&T syntax: outb %al, %dx
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    // AT&T syntax: inw %dx, %ax
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    // AT&T syntax: outw %ax, %dx
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
// Wait for the drive to finish processing and be ready for data transfer
static void ata_wait_ready(void) {
    // Wait until the interrupt handler sets our sync flag to 1
    while (!ide_interrupt_fired) {
        // You can add an assembly 'hlt' or 'nop' instruction here 
        // to minimize CPU power consumption while waiting
        asm volatile("nop");
    }
    
    // Reset the flag for the next read/write transaction
    ide_interrupt_fired = 0;
}
DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    
    // 1. Enable interrupts on the primary controller explicitly
    outb(0x3F6, 0x00); 
    
    // 2. Read the status register once to clear any pending, stale IRQs from boot time
    inb(ATA_REG_STATUS);
    
    return 0; 
}




// Add this helper to select the master drive cleanly
static void ata_select_master(LBA_t sector) {
    // 0xE0 selects Master Drive + sets LBA mode bits
    outb(ATA_REG_DRIVE, 0xE0 | ((sector >> 24) & 0x0F));
    
    // Give the hardware controller a tiny moment to switch drive contexts
    for (volatile int i = 0; i < 1000; i++) { asm volatile("nop"); }
}
// Helper to wait for the asynchronous IRQ 14 interrupt to finish a sector operation
static void ata_wait_interrupt(const char* context) {
    serial_printf("[IDE DEBUG] [%s] Entering wait loop. Current flag status: %d\n", context, ide_interrupt_fired);
    uint32_t loop_counter = 0;
    
    while (!ide_interrupt_fired) {
        asm volatile("nop");
        loop_counter++;
        if (loop_counter % 50000000 == 0) {
            uint8_t current_status = inb(ATA_REG_STATUS);
            // Inside ata_wait_interrupt in diskio.c, change the warning line to:
            serial_printf("[IDE WARNING] Status port bitfield value: %d\n", (uint32_t)inb(ATA_REG_STATUS));
                          context, loop_counter, current_status;
        }
    }
    serial_printf("[IDE DEBUG] [%s] Wait loop broken! Interrupt flag cleared.\n", context);
    ide_interrupt_fired = 0;
}

// Helper to poll the status port quickly for data transfer availability (DRQ)
static void ata_poll_drq(void) {
    serial_printf("[IDE DEBUG] Polling DRQ status...\n");
    inb(ATA_REG_STATUS); inb(ATA_REG_STATUS);
    inb(ATA_REG_STATUS); inb(ATA_REG_STATUS);
    
    uint8_t status;
    while ((status = inb(ATA_REG_STATUS)) & ATA_STATUS_BSY) {
        asm volatile("nop");
    }
    serial_printf("[IDE DEBUG] BSY cleared. Status: 0x%02X\n", status);
    
    while (!((status = inb(ATA_REG_STATUS)) & ATA_STATUS_DRQ)) {
        asm volatile("nop");
    }
    serial_printf("[IDE DEBUG] DRQ set successfully! Status: 0x%02X\n", status);
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    uint16_t* ptr = (uint16_t*)buff;

    serial_printf("[IDE TRACE] disk_read initiated. LBA: %u, Count: %u, Buffer Addr: 0x%X\n", (uint32_t)sector, count, (uint32_t)buff);

    for (UINT i = 0; i < count; i++) {
        LBA_t current_sector = sector + i;
        serial_printf("[IDE TRACE] Reading sub-sector %u of %u (LBA: %u)\n", i + 1, count, (uint32_t)current_sector);

        ata_select_master(current_sector);

        outb(ATA_REG_FEATURES, 0x00);
        outb(ATA_REG_SECTOR_COUNT, 1);
        outb(ATA_REG_LBA_LOW, (uint8_t)current_sector);
        outb(ATA_REG_LBA_MID, (uint8_t)(current_sector >> 8));
        outb(ATA_REG_LBA_HIGH, (uint8_t)(current_sector >> 16));

        serial_printf("[IDE TRACE] Clearing interrupt flag and issuing ATA_CMD_READ\n");
        ide_interrupt_fired = 0;
        outb(ATA_REG_COMMAND, ATA_CMD_READ);

        // ATA Spec 400ns recovery delay
        inb(ATA_REG_STATUS); inb(ATA_REG_STATUS);
        inb(ATA_REG_STATUS); inb(ATA_REG_STATUS);

        ata_wait_interrupt("disk_read");

        serial_printf("[IDE TRACE] Commencing 256-word data port pump into memory...\n");
        for (int w = 0; w < 256; w++) {
            *ptr++ = inw(ATA_REG_DATA);
        }
        serial_printf("[IDE TRACE] Sub-sector %u read completed.\n", i + 1);
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    const uint16_t* ptr = (const uint16_t*)buff;

    serial_printf("[IDE TRACE] disk_write initiated. LBA: %u, Count: %u, Buffer Addr: 0x%X\n", (uint32_t)sector, count, (uint32_t)buff);

    for (UINT i = 0; i < count; i++) {
        LBA_t current_sector = sector + i;
        serial_printf("[IDE TRACE] Writing sub-sector %u of %u (LBA: %u)\n", i + 1, count, (uint32_t)current_sector);

        ata_select_master(current_sector);

        outb(ATA_REG_FEATURES, 0x00);
        outb(ATA_REG_SECTOR_COUNT, 1);
        outb(ATA_REG_LBA_LOW, (uint8_t)current_sector);
        outb(ATA_REG_LBA_MID, (uint8_t)(current_sector >> 8));
        outb(ATA_REG_LBA_HIGH, (uint8_t)(current_sector >> 16));
        
        // 1. CLEAR the interrupt flag BEFORE issuing the command or data payload
        ide_interrupt_fired = 0;

        outb(ATA_REG_COMMAND, ATA_CMD_WRITE);

        // ATA Spec 400ns recovery delay
        inb(ATA_REG_STATUS); inb(ATA_REG_STATUS);
        inb(ATA_REG_STATUS); inb(ATA_REG_STATUS);

        ata_poll_drq();

        serial_printf("[IDE TRACE] Commencing 256-word data port write allocation from memory...\n");
        for (int w = 0; w < 256; w++) {
            outw(ATA_REG_DATA, *ptr++);
        }

        // 2. The flag tracking logic reset step has been safely moved up.
        serial_printf("[IDE TRACE] Waiting for write confirmation...\n");
        
        ata_wait_interrupt("disk_write");
        
        // 3. CRUCIAL STEP: Clear the BSY bit by reading the status port one extra time 
        // to finalize the transaction acknowledgment loop.
        inb(ATA_REG_STATUS);

        serial_printf("[IDE TRACE] Sub-sector %u write completed.\n", i + 1);
    }
    return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0) return RES_PARERR;
    switch(cmd) {
        case CTRL_SYNC: return RES_OK;
        // Adjust these to match the size of the disk image created in your Makefile
        case GET_SECTOR_COUNT: *(LBA_t*)buff = 20480; return RES_OK; // 10MB disk sample
        case GET_SECTOR_SIZE:  *(WORD*)buff = 512; return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD*)buff = 1; return RES_OK;
    }
    return RES_PARERR;
}

DWORD get_fattime(void) {
    return ((DWORD)(2026 - 1980) << 25) | ((DWORD)6 << 21) | ((DWORD)28 << 16);
}
// Minimalist implementation of memcmp for bare-metal FatFs linking
int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = (const unsigned char*)s1;
    const unsigned char* p2 = (const unsigned char*)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

// Minimalist implementation of strchr for bare-metal FatFs linking
char* strchr(const char* s, int c) {
    while (*s != (char)c) {
        if (!*s) {
            return 0;
        }
        s++;
    }
    return (char*)s;
}
// Minimalist implementation of memset for bare-metal FatFs linking
void* memset(void* s, int c, unsigned int n) {
    unsigned char* p = (unsigned char*)s;
    for (unsigned int i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }
    return s;
}