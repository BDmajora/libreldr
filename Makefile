# libreldr — gnu-efi build.
# Prereqs (Debian/Ubuntu/Mint):
#   sudo apt install gnu-efi build-essential binutils

ARCH        := $(shell uname -m)
EFIINC      := /usr/include/efi
EFIINCS     := -I$(EFIINC) -I$(EFIINC)/$(ARCH) -I$(EFIINC)/protocol
EFILIB      := /usr/lib
EFI_CRT     := $(EFILIB)/crt0-efi-$(ARCH).o
EFI_LDS     := $(EFILIB)/elf_$(ARCH)_efi.lds

CFLAGS      := $(EFIINCS) -fno-stack-protector -fpic -fshort-wchar \
               -mno-red-zone -ffreestanding -Wall -O2 \
               -DEFI_FUNCTION_WRAPPER

LDFLAGS     := -nostdlib -znocombreloc -T $(EFI_LDS) -shared \
               -Bsymbolic -L $(EFILIB) $(EFI_CRT)

all: libreldr.efi

src/libreldr.o: src/libreldr.c
	$(CC) $(CFLAGS) -c $< -o $@

libreldr.so: src/libreldr.o
	$(LD) $(LDFLAGS) $^ -o $@ -lefi -lgnuefi

libreldr.efi: libreldr.so
	objcopy -j .text -j .sdata -j .data -j .dynamic \
	        -j .dynsym  -j .rel  -j .rela -j .reloc \
	        --target=efi-app-$(ARCH) $< $@

clean:
	rm -f src/*.o libreldr.so libreldr.efi

.PHONY: all clean