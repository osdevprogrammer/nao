# Compiler and assembler definitions
CC = gcc-13
AS = nasm
LD = ld

# Flags
ASFLAGS = -f elf32
LDFLAGS = -m elf_i386 -T linker.ld
# Added -DLWIP_RAW=1 here to enable raw sockets/pings in the lwip codebase
CFLAGS = -m32 -c -std=gnu99 -ffreestanding -fno-stack-protector -D_FORTIFY_SOURCE=0 -O2 -Wall -Wextra -DLWIP_RAW=1 -I. \
         -I./network/port \
         -I./network/lwip/src/include \
         -I./network/lwip/src/include/lwip/apps
# 2. Track the exact source files we need from the lwip tree
LWIP_SRCS = ./network/lwip/src/core/init.c \
            ./network/lwip/src/core/mem.c \
            ./network/lwip/src/core/memp.c \
            ./network/lwip/src/core/pbuf.c \
            ./network/lwip/src/core/timeouts.c \
            ./network/lwip/src/core/def.c \
            ./network/lwip/src/core/inet_chksum.c \
            ./network/lwip/src/core/raw.c \
            ./network/lwip/src/core/ipv4/ip4.c \
            ./network/lwip/src/core/ipv4/ip4_addr.c \
            ./network/lwip/src/core/ipv4/ip4_frag.c \
            ./network/lwip/src/core/ipv4/icmp.c \
            ./network/lwip/src/core/netif.c \
            ./network/lwip/src/core/tcp.c \
            ./network/lwip/src/core/tcp_in.c \
            ./network/lwip/src/core/tcp_out.c \
            ./network/lwip/src/core/udp.c \
            ./network/lwip/src/netif/ethernet.c \
            ./network/lwip/src/core/ipv4/etharp.c \
            ./network/lwip/src/core/ipv4/dhcp.c \
            ./network/port/netif_driver.c \
			./network/lwip/src/apps/http/http_client.o \
            ./rtl8139.c

# 3. Convert those source file paths directly into .o file outputs
LWIP_OBJS = $(LWIP_SRCS:.c=.o)

# 4. Append them cleanly to your main operating system OBJS tracking rule
OBJS = boot.o kernel.o idt.o gdt.o serial.o gui.o mm.o diskio.o ff.o explorer.o naoedit.o ffunicode.o cursor_loader.o ani_loader.o naobrowse.o naoview.o \
       $(LWIP_OBJS)

# Outputs
TARGET = nao.bin
ISO_OUT = nao.iso

# Persistent storage definitions
DISK_RAW = hdd.img
DISK_QCOW2 = hdd.qcow2
DISK_SIZE_MB = 16

.PHONY: all clean clean-all run

all: $(TARGET) $(ISO_OUT) $(DISK_QCOW2)

# Link the kernel binary
$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $(TARGET) $(OBJS)

# Assemble boot.asm
boot.o: boot.asm
	$(AS) $(ASFLAGS) boot.asm -o boot.o

# Pattern rule for compiling C files cleanly
%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

# Build the bootable ISO
$(ISO_OUT): $(TARGET)
	@mkdir -p iso/boot/grub
	@cp $(TARGET) iso/boot/$(TARGET)
	@echo 'set timeout=0' > iso/boot/grub/grub.cfg
	@echo 'set default=0' >> iso/boot/grub/grub.cfg
	@echo '' >> iso/boot/grub/grub.cfg
	@echo 'menuentry "My Custom OS" {' >> iso/boot/grub/grub.cfg
	@echo '    multiboot /boot/$(TARGET)' >> iso/boot/grub/grub.cfg
	@echo '    boot' >> iso/boot/grub/grub.cfg
	@echo '}' >> iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO_OUT) iso
	@rm -rf iso

# Create a persistent 10MB VirtualBox QCOW2 hard disk if it doesn't exist
$(DISK_QCOW2):
	@echo "[MAKE] Generating fresh raw storage container..."
	dd if=/dev/zero of=$(DISK_RAW) bs=1M count=$(DISK_SIZE_MB)
	
	@echo "[MAKE] Formatting raw disk layout with FAT16 filesystem..."
	sudo mkfs.vfat -F 16 $(DISK_RAW)
	
	@echo "[MAKE] Injecting system configuration and bitmap assets..."
	@if [ -f arrow.cur ]; then mcopy -i $(DISK_RAW) arrow.cur ::/arrow.cur; fi
	@if [ -f arrow_load.ani ]; then mcopy -i $(DISK_RAW) arrow_load.ani ::/arrow_load.ani; fi
	@if [ -f connected.png ]; then mcopy -i $(DISK_RAW) connected.png ::/connected.png; fi
	@if [ -f noconnect.png ]; then mcopy -i $(DISK_RAW) noconnect.png ::/noconnect.png; fi
	@if [ -f boot.png ]; then mcopy -i $(DISK_RAW) boot.png ::/boot.png; fi
	@if [ -f shutdown.png ]; then mcopy -i $(DISK_RAW) shutdown.png ::/shutdown.png; fi
	@echo "[MAKE] Converting raw disk layout into VirtualBox QCOW2 image..."
	rm -f $(DISK_QCOW2)
	qemu-img convert -f raw -O qcow2 $(DISK_RAW) $(DISK_QCOW2)
	rm -f $(DISK_RAW)

clean:
	rm -f $(OBJS) $(TARGET) $(ISO_OUT) $(DISK_QCOW2)

run: $(ISO_OUT) $(DISK_QCOW2)
	qemu-system-i386 \
    	-cdrom $(ISO_OUT) \
    	-drive file=$(DISK_QCOW2),format=qcow2,bus=0,unit=0,media=disk \
    	-vga std \
    	-boot d \
    	-netdev user,id=net0,net=10.0.2.0/24,dhcpstart=10.0.2.15 \
    	-device rtl8139,netdev=net0 \
    	-object filter-dump,id=dump0,netdev=net0,file=net_debug.pcap
