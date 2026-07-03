; Multiboot header constants
MODULEALIGN equ 1 << 0
MEMINFO     equ 1 << 1
VIDEO_MODE  equ 1 << 2          ; Tell GRUB we want a graphical framebuffer
FLAGS       equ MODULEALIGN | MEMINFO | VIDEO_MODE
MAGIC       equ 0x1BADB002
CHECKSUM    equ -(MAGIC + FLAGS)

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    
    ; Address fields - mandatory padding when using VIDEO_MODE flag 
    ; but loading an ELF binary. Must be filled with zeros.
    dd 0 ; header_addr
    dd 0 ; load_addr
    dd 0 ; load_end_addr
    dd 0 ; bss_end_addr
    dd 0 ; entry_addr
    
    ; Graphics mode configuration
    dd 0    ; Mode type: 0 = linear graphics mode
    dd 1024 ; Width
    dd 768  ; Height
    dd 32   ; Depth (Bits Per Pixel)
section .bss
align 16
stack_bottom:
    resb 65536 ; Increase to 64 KiB of stack space
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top
    and esp, 0xFFFFFFF0 ; Force 16-byte stack alignment

    push ebx ; Push the Multiboot structure pointer
    call kernel_main
.hang:
    cli
    hlt
    jmp .hang
; Global functions we can call from C
global idt_load
global restart_system
extern idt_ptr
idt_load:
    lidt [idt_ptr]
    ret

; Restart function - uses keyboard controller to reset
restart_system:
    cli
    
    ; Try to pulse CPU reset via keyboard controller
    ; First, wait for keyboard controller to be ready
    mov al, 0xFE
    out 0x64, al
    
    ; Wait a bit, then try falling back to triple fault
    mov ecx, 0x100000
.wait:
    loop .wait
    
    ; Triple fault: load null IDT to cause reset
    lidt [null_idt]
    int 0x03
    
.hang:
    hlt
    jmp .hang

null_idt:
    dw 0     ; Limit
    dd 0     ; Base

; Common ISR stub that saves CPU state, calls C, and restores state
extern isr_handler
isr_common_stub:
    pusha                    ; Pushes edi, esi, ebp, esp, ebx, edx, ecx, eax
    mov ax, ds               ; Lower 16-bits of eax = ds
    push eax                 ; Save the data segment descriptor

    mov ax, 0x10             ; Load the kernel data segment descriptor
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call isr_handler

    pop eax                  ; Reload the original data segment descriptor
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa                     ; Pops edi, esi, ebp...
    add esp, 8               ; Cleans up the pushed error code and ISR number
    sti
    iret                     ; Interrupt return

; Define a couple of basic Intel CPU Exception Handlers (0 to 3)
global isr0
global isr3

; 0: Divide By Zero Exception
isr0:
    cli
    push byte 0              ; Dummy error code
    push byte 0              ; ISR number 0
    jmp isr_common_stub

; 3: Breakpoint Exception
isr3:
    cli
    push byte 0
    push byte 3
    jmp isr_common_stub
global irq0   ; System timer interrupt
irq0:
    cli
    push byte 0  ; Dummy error code
    push byte 32 ; Interrupt number 32 (IRQ 0 + 32)
    jmp isr_common_stub
global irq1   ; Keyboard interrupt
irq1:
    cli
    push byte 0  ; Dummy error code
    push byte 33 ; Interrupt number 33 (IRQ 1 + 32)
    jmp isr_common_stub
global gdt_flush
gdt_flush:
    mov eax, [esp+4]  ; Get the pointer to the GDT pointer
    lgdt [eax]        ; Load the new GDT pointer

    mov ax, 0x10      ; 0x10 is the offset in the GDT to our data segment
    mov ds, ax        ; Load all data segment selectors
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush   ; 0x08 is the offset to our code segment: Far jump!
.flush:
    ret
global irq12   ; PS/2 Mouse interrupt
irq12:
    cli
    push byte 0  ; Dummy error code
    push byte 44 ; Interrupt number 44 (IRQ 12 + 32)
    jmp isr_common_stub
; --- Add this inside your text section near irq12 ---
global irq14   ; Hard Drive primary controller interrupt
irq14:
    cli
    push byte 0  ; Dummy error code
    push byte 46 ; Interrupt number 46 (IRQ 14 + 32 = 46 -> 0x2E)
    jmp isr_common_stub