# libreldr

A FreeLdr-styled bootloader for YetiOS. One name, one config, one menu —
works on both UEFI and BIOS systems.

The user sees a single boot manager that pixel-matches ReactOS FreeLdr's
MinimalUI: black screen, white text, inverse-video selection bar,
countdown line, F8 hint at the bottom.

## Architecture

`libreldr` is delivered as **two backends presenting one front-end:**

- **UEFI:** `libreldr.efi` — a small gnu-efi program (~500 lines C)
  that draws the FreeLdr menu and hands control to Linux via the
  kernel's built-in EFI stub. This is the file in `src/libreldr.c`.

- **BIOS:** syslinux + a themed `menu.c32` config that mirrors the
  same look, the same wording, the same colors. Syslinux already
  implements the BIOS Linux boot protocol — we just present its menu
  as "libreldr" via branding in the config. The user never sees
  "syslinux" anywhere on screen.

Both backends read the same logical entries (defined once in your build
script), produced into the two configs Stage 7 writes during the install.

This is "libreldr" the *product*. The fact that two implementations live
underneath is an implementation detail.

## Build

The UEFI side:

    sudo apt install gnu-efi build-essential binutils
    make

Output: `libreldr.efi`.

The BIOS side has no build step. Syslinux is installed in the target
during package install (the `sys-boot/syslinux` package is already in
your `YETI_PACKAGE_LIST`).

## Install layout

For a hybrid BIOS+UEFI image, your `/boot` partition (FAT32, marked
bootable, with the ESP flag set) ends up like:

    /EFI/BOOT/BOOTX64.EFI            <- libreldr.efi (UEFI)
    /EFI/libreldr/libreldr.conf     <- UEFI menu config
    /EFI/yetios/vmlinuz.efi          <- kernel (renamed copy of vmlinuz)
    /vmlinuz                         <- kernel (BIOS path)
    /initramfs.img                   <- initrd (BIOS path)
    /syslinux.cfg                    <- BIOS menu config (libreldr-themed)
    /ldlinux.sys                     <- syslinux core (written by `syslinux`)
    /menu.c32                        <- syslinux text-menu module
    /libcom32.c32                    <- syslinux runtime
    /libutil.c32                     <- syslinux runtime

The MBR boot stub (`mbr.bin`, dd'd at offset 0) hands off to syslinux
on BIOS systems. UEFI firmware reads the GPT, finds the ESP, and
launches `BOOTX64.EFI` directly. The same FAT partition serves both.

## How to integrate with yeti-build

Replace `src/stage_07_bootloader.py` with the version in
`integration/stage_07_bootloader.py`. The key changes:

1. The image partition table becomes GPT (with a BIOS Boot Partition
   for the syslinux core, plus an ESP for both backends).
2. Stage 2 (`stage_02_image.py`) needs an updated `parted` invocation
   — see `integration/stage_02_image.py` for the new partition layout.
3. Stage 7 now: copies `libreldr.efi` and `libreldr.conf` to the ESP,
   copies the syslinux .c32 modules from the target, writes a themed
   `syslinux.cfg`, installs the syslinux MBR.

The kernel command line, default entry, and timeout live in
`integration/libreldr_entries.py` — that single Python file is the
source of truth, and Stage 7 generates both `libreldr.conf` (UEFI) and
`syslinux.cfg` (BIOS) from it. Edit entries in one place.

## Visual fidelity

The two backends render through different code paths but produce visually
matching output:

- Both use 80-column text mode, black background, light-gray text.
- Both put an inverse-video selection bar on the chosen entry.
- Both display the same header ("Please select the operating system to
  start:"), help lines, countdown line, and F8 hint.

Minor differences you'll see in practice:

- The exact pixel font is firmware-dependent (UEFI text mode and BIOS
  VGA text mode use different glyphs).
- Syslinux's arrow keys behave identically; F8 on BIOS is a no-op (same
  as on UEFI — the hint is cosmetic on both backends today).

## Test with QEMU

UEFI (with OVMF):

    sudo apt install ovmf
    qemu-system-x86_64 -enable-kvm -m 4G \
        -bios /usr/share/OVMF/OVMF_CODE.fd \
        -drive file=build/yetios.img,format=raw

BIOS (legacy SeaBIOS, QEMU's default):

    qemu-system-x86_64 -enable-kvm -m 4G \
        -drive file=build/yetios.img,format=raw

## Caveats

- **No Secure Boot.** The .efi isn't signed. Disable Secure Boot in
  firmware, or sign with `sbsign` and enroll your key.
- **F8 is cosmetic.** Both backends print the hint but neither wires up
  an advanced boot menu. Add it later if you want.
- **Legacy Syslinux integration.** Keep it out of the default permissive path
  unless you intentionally add a separate component boundary.

## License

MIT License - YetiOS Project.
