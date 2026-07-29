# libreldr

`libreldr` is the YetiOS UEFI boot menu. It draws a FreeLdr-styled text menu
and starts the selected EFI program.

For the current FreeBSD-based YetiOS image, `libreldr` is installed as:

```text
EFI/BOOT/BOOTX64.EFI
EFI/libreldr/libreldr.efi
EFI/libreldr/libreldr.conf
```

The default YetiOS entry chainloads FreeBSD's `loader.efi`:

```text
timeout 5
default 0

title YetiOS
chain \EFI\freebsd\loader.efi
```

Linux EFI-stub entries are still supported with the existing `linux` and
`options` directives, but the FreeBSD path uses `chain`.

## Build

```bash
make
```

Output:

```text
libreldr.efi
```

The current Makefile expects a gnu-efi style development environment with
`cc`, `ld`, `objcopy`, and the gnu-efi headers/libraries available.

## Dependency Status

The current UEFI binary links against `gnu-efi` startup/support objects:

```text
crt0-efi-*.o
libefi
libgnuefi
```

The `gnu-efi` project is listed upstream as BSD licensed, so this is not a GPL
runtime dependency. However, it is still a `gnu-efi` dependency. If the YetiOS
commercial base requires no GNU-named dependency at all, the next `libreldr`
milestone is to replace `gnu-efi` with YetiOS-owned minimal UEFI definitions and
startup code or another reviewed permissive-only UEFI support layer.

## Config Directives

```text
timeout SECONDS
default INDEX
title DISPLAY NAME
chain \EFI\path\to\program.efi
linux \EFI\path\to\vmlinuz.efi
options kernel command line
```

Use `chain` for FreeBSD `loader.efi`. Use `linux` plus `options` for Linux
EFI-stub kernels.

## Caveats

- Secure Boot is not wired up yet; the EFI binary is unsigned.
- F8 is currently only a visual hint.
- BIOS/Syslinux integration is outside the current permissive YetiOS base path.

## License

MIT License - YetiOS Project.
