# libreldr — gnu-efi build (UEFI only)
#
# Prereqs (Debian/Ubuntu/Mint):
#   sudo apt install gnu-efi build-essential binutils

ARCH      := $(shell uname -m)
EFI_ARCH  := $(ARCH)
BFD_ARCH  := elf64-x86-64

# ---- UEFI configuration (gnu-efi) ------------------------------------------
EFIINC    := /usr/include/efi
EFILIB    := /usr/lib
EFIINCS    = -I$(EFIINC) -I$(EFIINC)/$(ARCH) -I$(EFIINC)/protocol
EFI_CRT    = $(EFILIB)/crt0-efi-$(ARCH).o
EFI_LDS    = $(EFILIB)/elf_$(ARCH)_efi.lds
EFI_CFLAGS  = $(EFIINCS) -fno-stack-protector -fpic -fshort-wchar \
              -mno-red-zone -ffreestanding -Wall -O2 -DEFI_FUNCTION_WRAPPER
EFI_LDFLAGS = -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic \
              -L $(EFILIB) $(EFI_CRT)

# Sections to keep in the final PE image.
#
# CRITICAL: .rodata MUST be included. gcc puts ASCII string literals
# ("timeout", "title", "linux", etc.) in .rodata by default. If objcopy
# drops that section, the strings are missing at runtime, every StrEqA()
# comparison silently fails, ParseConfig() parses zero entries, and
# libreldr exits with EFI_NOT_FOUND back to the firmware. The screen
# clears (from SetMode) and you land back at the EFI shell with no error
# message — exactly the failure mode where the bootloader runs, the
# screen flashes, and nothing happens.
#
# Also include .rodata.* (gcc may emit named subsections like
# .rodata.str1.1) and .got/.got.plt (PIC needs them for static data
# addressing on some toolchains).
OBJCOPY_SECTIONS = -j .text -j .sdata -j .data -j .rodata -j .rodata.* \
                   -j .got -j .got.plt \
                   -j .dynamic -j .dynsym \
                   -j .rel -j .rela -j .rel.* -j .rela.* \
                   -j .reloc

all: libreldr.efi

src/libreldr.o: src/libreldr.c
	$(CC) $(EFI_CFLAGS) -c $< -o $@

libreldr.so: src/libreldr.o
	$(LD) $(EFI_LDFLAGS) $^ -o $@ -lefi -lgnuefi

libreldr.efi: libreldr.so
	objcopy -I $(BFD_ARCH) -O efi-app-$(EFI_ARCH) $(OBJCOPY_SECTIONS) $< $@

clean:
	rm -f src/*.o libreldr.so libreldr.efi

.PHONY: all clean
