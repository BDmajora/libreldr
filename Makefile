# libreldr — gnu-efi + syslinux com32 build.
#
# Prereqs (Debian/Ubuntu/Mint):
#   sudo apt install gnu-efi build-essential binutils \
#                    syslinux syslinux-common
#   # plus syslinux source tree for com32 headers (set SYSLINUX_SRC below)

ARCH      := $(shell uname -m)

# ---- UEFI side (gnu-efi) ---------------------------------------------------
EFIINC    := /usr/include/efi
EFILIB    := /usr/lib
EFIINCS    = -I$(EFIINC) -I$(EFIINC)/$(ARCH) -I$(EFIINC)/protocol
EFI_CRT    = $(EFILIB)/crt0-efi-$(ARCH).o
EFI_LDS    = $(EFILIB)/elf_$(ARCH)_efi.lds
EFI_CFLAGS  = $(EFIINCS) -fno-stack-protector -fpic -fshort-wchar \
              -mno-red-zone -ffreestanding -Wall -O2 -DEFI_FUNCTION_WRAPPER
EFI_LDFLAGS = -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic \
              -L $(EFILIB) $(EFI_CRT)

# ---- BIOS side (syslinux com32) -------------------------------------------
# Point this at a syslinux source tree containing com32/include & com32/lib.
# On Debian: apt source syslinux ; SYSLINUX_SRC=/path/to/syslinux-6.04
SYSLINUX_SRC ?= /usr/share/syslinux/com32
COM32_INC    := -I$(SYSLINUX_SRC)/include -I$(SYSLINUX_SRC)/lib \
                -I$(SYSLINUX_SRC)/libutil/include
COM32_CFLAGS := -m32 -march=i386 -Os -fomit-frame-pointer -ffreestanding \
                -fno-stack-protector -fno-pie -no-pie -nostdinc \
                -Wall $(COM32_INC) -D__COM32__
COM32_LDFLAGS := -m elf_i386 -shared --hash-style=gnu -T $(SYSLINUX_SRC)/lib/com32.ld
COM32_LIBS   := $(SYSLINUX_SRC)/libutil/libutil.c32 \
                $(SYSLINUX_SRC)/lib/libcom32.c32

all: libreldr.efi libreldr.c32

# ---- UEFI ------------------------------------------------------------------
src/libreldr.o: src/libreldr.c
	$(CC) $(EFI_CFLAGS) -c $< -o $@

libreldr.so: src/libreldr.o
	$(LD) $(EFI_LDFLAGS) $^ -o $@ -lefi -lgnuefi

libreldr.efi: libreldr.so
	objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
	        -j .rel -j .rela -j .reloc \
	        --target=efi-app-$(ARCH) $< $@

# ---- BIOS ------------------------------------------------------------------
src/libreldr_bios.o: src/libreldr_bios.c
	$(CC) $(COM32_CFLAGS) -c $< -o $@

libreldr.elf: src/libreldr_bios.o
	$(LD) $(COM32_LDFLAGS) -o $@ $^ $(COM32_LIBS)

libreldr.c32: libreldr.elf
	objcopy -O elf32-i386 $< $@

clean:
	rm -f src/*.o libreldr.so libreldr.efi libreldr.elf libreldr.c32

.PHONY: all clean