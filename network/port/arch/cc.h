#ifndef CC_H
#define CC_H

#include <stdint.h>
#include <stddef.h>
typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;
typedef uint32_t sys_prot_t;
#define BYTE_ORDER LITTLE_ENDIAN

#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

extern void serial_printf(const char* format, ...);
#define LWIP_PLATFORM_DIAG(x) do { serial_printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { serial_printf("[LWIP ASSERT]: %s\n", x); while(1); } while(0)
// Provide primitive string library bindings (or map to your existing ones)


void* memcpy(void* dest, const void* src, size_t n);
size_t strlen(const char* s);
int strncmp(const char* s1, const char* s2, size_t n);
void* memmove(void* dest, const void* src, size_t n);

// Critical section stubs (Since we are single-threaded NO_SYS=1)
static inline sys_prot_t sys_arch_protect(void) { return 0; }
static inline void sys_arch_unprotect(sys_prot_t pval) { (void)pval; }

// System time tracker stub (Returns time in milliseconds)
// Tie this to an internal PIT or APIC clock variable if you have one!
static inline uint32_t sys_now(void) {
    static uint32_t fake_ticks = 0;
    return fake_ticks++; 
}

// Map GCC localization references
extern const unsigned short int **__ctype_b_loc(void);
#endif