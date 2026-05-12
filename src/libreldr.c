/*
 * libreldr — a UEFI bootloader for YetiOS that pixel-matches
 * ReactOS FreeLdr's MinimalUI (NTLDR-style) boot menu.
 *
 * NOTE: This is the UEFI half of libreldr. The BIOS half is provided
 * by a libreldr-themed syslinux install — see README.md. The user-
 * facing branding is the same on both; only the firmware-level entry
 * differs. yeti-build's Stage 7 installs whichever the image needs.
 *
 * Hand-off model (UEFI): we don't reimplement the Linux boot protocol.
 * Linux kernels since 3.3 are themselves valid PE/COFF EFI applications
 * (the "EFI stub"), so we just LoadImage + StartImage them with the
 * cmdline passed via LoadOptions.
 *
 * Build: see Makefile. Output: libreldr.efi -> install as
 *   /EFI/BOOT/BOOTX64.EFI  on the ESP.
 *
 * Config file (on same volume as the .efi):
 *   /EFI/libreldr/libreldr.conf
 *
 * Config format (one entry per stanza, blank line separated):
 *   title YetiOS
 *   linux \EFI\yetios\vmlinuz.efi
 *   options root=LABEL=yetios-root ro quiet
 *
 * Global keys (before first 'title'):
 *   timeout 5
 *   default 0
 */

#include <efi.h>
#include <efilib.h>

#define MAX_ENTRIES   16
#define MAX_TITLE     128
#define MAX_PATH_LEN  256
#define MAX_OPTS      512

typedef struct {
    CHAR16 Title[MAX_TITLE];
    CHAR16 Kernel[MAX_PATH_LEN];
    CHAR16 Options[MAX_OPTS];
    BOOLEAN Used;
} BootEntry;

static BootEntry gEntries[MAX_ENTRIES];
static UINTN     gEntryCount = 0;
static UINTN     gDefault    = 0;
static INTN      gTimeout    = 10;

static EFI_SYSTEM_TABLE             *gST;
static EFI_BOOT_SERVICES            *gBS;
static SIMPLE_TEXT_OUTPUT_INTERFACE *gOut;
static SIMPLE_INPUT_INTERFACE       *gIn;
static UINTN gCols, gRows;

/* -------------------------------------------------------------------- */
/* Tiny string helpers (no libc in EFI)                                 */
/* -------------------------------------------------------------------- */

static BOOLEAN
StrEqA(const CHAR8 *s, UINTN len, const char *kw)
{
    UINTN kwlen = 0;
    while (kw[kwlen]) kwlen++;
    if (len != kwlen) return FALSE;
    for (UINTN i = 0; i < len; i++) {
        if (s[i] != (CHAR8)kw[i]) return FALSE;
    }
    return TRUE;
}

static void
AsciiToUcs2(const CHAR8 *src, UINTN srcLen, CHAR16 *dst, UINTN dstCap)
{
    UINTN j = 0;
    for (UINTN i = 0; i < srcLen && j < dstCap - 1; i++) {
        CHAR8 c = src[i];
        if (c == '\r' || c == '\n') break;
        dst[j++] = (CHAR16)c;
    }
    dst[j] = 0;
}

static void
NormalizePath(CHAR16 *p)
{
    while (*p) { if (*p == L'/') *p = L'\\'; p++; }
}

static INTN
ParseInt(const CHAR8 *s, UINTN len)
{
    INTN v = 0; UINTN i = 0; BOOLEAN neg = FALSE;
    if (i < len && s[i] == '-') { neg = TRUE; i++; }
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + (s[i] - '0');
    }
    return neg ? -v : v;
}

/* -------------------------------------------------------------------- */
/* Config file loader                                                   */
/* -------------------------------------------------------------------- */

static EFI_STATUS
ReadConfigFile(EFI_HANDLE ImageHandle, CHAR8 **buf, UINTN *size)
{
    EFI_STATUS Status;
    EFI_LOADED_IMAGE      *LoadedImage;
    EFI_FILE_IO_INTERFACE *FsProto;
    EFI_FILE_HANDLE        Root, File;
    EFI_GUID FsGuid = SIMPLE_FILE_SYSTEM_PROTOCOL;

    Status = uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle,
                               &LoadedImageProtocol, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    Status = uefi_call_wrapper(gBS->HandleProtocol, 3,
                               LoadedImage->DeviceHandle,
                               &FsGuid, (VOID **)&FsProto);
    if (EFI_ERROR(Status)) return Status;

    Status = uefi_call_wrapper(FsProto->OpenVolume, 2, FsProto, &Root);
    if (EFI_ERROR(Status)) return Status;

    Status = uefi_call_wrapper(Root->Open, 5, Root, &File,
                               L"\\EFI\\libreldr\\libreldr.conf",
                               EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        uefi_call_wrapper(Root->Close, 1, Root);
        return Status;
    }

    UINT8 InfoBuf[sizeof(EFI_FILE_INFO) + 256];
    UINTN InfoSize = sizeof(InfoBuf);
    EFI_GUID InfoGuid = EFI_FILE_INFO_ID;
    Status = uefi_call_wrapper(File->GetInfo, 4, File, &InfoGuid,
                               &InfoSize, InfoBuf);
    if (EFI_ERROR(Status)) goto done;

    UINT64 fsize = ((EFI_FILE_INFO *)InfoBuf)->FileSize;
    if (fsize > 65536) fsize = 65536;

    Status = uefi_call_wrapper(gBS->AllocatePool, 3, EfiLoaderData,
                               (UINTN)fsize + 1, (VOID **)buf);
    if (EFI_ERROR(Status)) goto done;

    UINTN ReadSize = (UINTN)fsize;
    Status = uefi_call_wrapper(File->Read, 3, File, &ReadSize, *buf);
    if (EFI_ERROR(Status)) {
        uefi_call_wrapper(gBS->FreePool, 1, *buf);
        *buf = NULL;
        goto done;
    }
    (*buf)[ReadSize] = 0;
    *size = ReadSize;

done:
    uefi_call_wrapper(File->Close, 1, File);
    uefi_call_wrapper(Root->Close, 1, Root);
    return Status;
}

static BOOLEAN
NextLine(const CHAR8 *buf, UINTN size, UINTN *pos,
         const CHAR8 **lineOut, UINTN *lenOut)
{
    if (*pos >= size) return FALSE;
    UINTN start = *pos;
    while (*pos < size && buf[*pos] != '\n') (*pos)++;
    UINTN end = *pos;
    if (end > start && buf[end - 1] == '\r') end--;
    if (*pos < size) (*pos)++;
    *lineOut = &buf[start];
    *lenOut  = end - start;
    return TRUE;
}

static void
TrimLeading(const CHAR8 **s, UINTN *len)
{
    while (*len > 0 && (**s == ' ' || **s == '\t')) { (*s)++; (*len)--; }
}

static void
ParseConfig(const CHAR8 *buf, UINTN size)
{
    UINTN pos = 0;
    BootEntry *cur = NULL;
    const CHAR8 *line;
    UINTN len;

    while (NextLine(buf, size, &pos, &line, &len)) {
        TrimLeading(&line, &len);
        if (len == 0 || line[0] == '#') continue;

        UINTN k = 0;
        while (k < len && line[k] != ' ' && line[k] != '\t') k++;
        const CHAR8 *val = &line[k];
        UINTN vallen = len - k;
        TrimLeading(&val, &vallen);

        if (StrEqA(line, k, "timeout")) {
            gTimeout = ParseInt(val, vallen);
        } else if (StrEqA(line, k, "default")) {
            gDefault = (UINTN)ParseInt(val, vallen);
        } else if (StrEqA(line, k, "title")) {
            if (gEntryCount >= MAX_ENTRIES) continue;
            cur = &gEntries[gEntryCount++];
            cur->Used = TRUE;
            AsciiToUcs2(val, vallen, cur->Title, MAX_TITLE);
        } else if (StrEqA(line, k, "linux")) {
            if (cur) {
                AsciiToUcs2(val, vallen, cur->Kernel, MAX_PATH_LEN);
                NormalizePath(cur->Kernel);
            }
        } else if (StrEqA(line, k, "options")) {
            if (cur) AsciiToUcs2(val, vallen, cur->Options, MAX_OPTS);
        }
    }
}

static void
DefaultEntries(void)
{
    StrCpy(gEntries[0].Title,   L"YetiOS");
    StrCpy(gEntries[0].Kernel,  L"\\EFI\\yetios\\vmlinuz.efi");
    StrCpy(gEntries[0].Options, L"root=LABEL=yetios-root ro quiet");
    gEntries[0].Used = TRUE;

    StrCpy(gEntries[1].Title,   L"YetiOS (Debug)");
    StrCpy(gEntries[1].Kernel,  L"\\EFI\\yetios\\vmlinuz.efi");
    StrCpy(gEntries[1].Options, L"root=LABEL=yetios-root rw debug");
    gEntries[1].Used = TRUE;

    gEntryCount = 2;
    gTimeout = 5;
    gDefault = 0;
}

/* -------------------------------------------------------------------- */
/* MinimalUI rendering                                                  */
/* -------------------------------------------------------------------- */

#define ATTR_NORMAL    EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)
#define ATTR_SELECTED  EFI_TEXT_ATTR(EFI_BLACK,     EFI_LIGHTGRAY)

static void SetAttr(UINTN a) { uefi_call_wrapper(gOut->SetAttribute, 2, gOut, a); }
static void GotoXY(UINTN x, UINTN y) { uefi_call_wrapper(gOut->SetCursorPosition, 3, gOut, x, y); }
static void PutS(CHAR16 *s) { uefi_call_wrapper(gOut->OutputString, 2, gOut, s); }

/* Pixel-match the screenshot:
 *
 *   Please select the operating system to start:
 *
 *       [highlight bar with title]
 *       Other entry
 *       ...
 *
 *   Use ↑ and ↓ to move the highlight to your choice.
 *   Press ENTER to choose.
 *
 *   Seconds until highlighted choice will be started automatically: N
 *
 *
 *   For troubleshooting and advanced startup options for YetiOS, press F8.
 */
static void
DrawMenu(UINTN selected, INTN remaining)
{
    SetAttr(ATTR_NORMAL);
    uefi_call_wrapper(gOut->ClearScreen, 1, gOut);
    uefi_call_wrapper(gOut->EnableCursor, 2, gOut, FALSE);

    GotoXY(0, 1);
    PutS(L"Please select the operating system to start:");

    UINTN row = 3;
    for (UINTN i = 0; i < gEntryCount; i++) {
        GotoXY(0, row + i);
        PutS(L"    ");
        if (i == selected) {
            SetAttr(ATTR_SELECTED);
            PutS(gEntries[i].Title);
            UINTN tlen = StrLen(gEntries[i].Title);
            UINTN bar  = (tlen < 36) ? 36 - tlen : 1;
            for (UINTN k = 0; k < bar; k++) PutS(L" ");
            SetAttr(ATTR_NORMAL);
        } else {
            PutS(gEntries[i].Title);
        }
    }

    UINTN after = row + gEntryCount + 1;

    GotoXY(0, after);
    /* CP437 / Unicode arrows: U+2191 ↑, U+2193 ↓ */
    PutS(L"Use \x2191 and \x2193 to move the highlight to your choice.");
    GotoXY(0, after + 1);
    PutS(L"Press ENTER to choose.");

    GotoXY(0, after + 3);
    if (remaining >= 0) {
        CHAR16 buf[128];
        SPrint(buf, sizeof(buf),
               L"Seconds until highlighted choice will be started automatically: %d  ",
               remaining);
        PutS(buf);
    } else {
        for (UINTN k = 0; k < gCols; k++) PutS(L" ");
    }

    if (gRows > 4) {
        GotoXY(0, gRows - 3);
        PutS(L"For troubleshooting and advanced startup options for YetiOS, press F8.");
    }
}

/* -------------------------------------------------------------------- */
/* Hand off to a Linux kernel via the EFI stub                          */
/* -------------------------------------------------------------------- */

static EFI_STATUS
BootEntryRun(EFI_HANDLE ImageHandle, BootEntry *entry)
{
    EFI_STATUS Status;
    EFI_LOADED_IMAGE *LoadedImage;
    EFI_DEVICE_PATH  *DevicePath;
    EFI_HANDLE        NewImage;

    Status = uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle,
                               &LoadedImageProtocol, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    DevicePath = FileDevicePath(LoadedImage->DeviceHandle, entry->Kernel);
    if (!DevicePath) return EFI_OUT_OF_RESOURCES;

    Status = uefi_call_wrapper(gBS->LoadImage, 6,
                               FALSE, ImageHandle, DevicePath, NULL, 0,
                               &NewImage);
    if (EFI_ERROR(Status)) return Status;

    Status = uefi_call_wrapper(gBS->HandleProtocol, 3, NewImage,
                               &LoadedImageProtocol, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    /* Linux EFI stub reads LoadOptions as a UCS-2 string. */
    LoadedImage->LoadOptions     = entry->Options;
    LoadedImage->LoadOptionsSize = (StrLen(entry->Options) + 1) * sizeof(CHAR16);

    return uefi_call_wrapper(gBS->StartImage, 3, NewImage, NULL, NULL);
}

/* -------------------------------------------------------------------- */
/* Entry point                                                          */
/* -------------------------------------------------------------------- */

EFI_STATUS EFIAPI
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);
    gST  = SystemTable;
    gBS  = SystemTable->BootServices;
    gOut = SystemTable->ConOut;
    gIn  = SystemTable->ConIn;

    /* Largest text mode available. */
    UINTN best_mode = 0, best_area = 0;
    for (UINTN m = 0; m < (UINTN)gOut->Mode->MaxMode; m++) {
        UINTN c, r;
        if (uefi_call_wrapper(gOut->QueryMode, 4, gOut, m, &c, &r)
                == EFI_SUCCESS && c * r > best_area) {
            best_area = c * r;
            best_mode = m;
        }
    }
    uefi_call_wrapper(gOut->SetMode, 2, gOut, best_mode);
    uefi_call_wrapper(gOut->QueryMode, 4, gOut, best_mode, &gCols, &gRows);

    /* Config: try file, fall back to defaults. */
    CHAR8 *cfgBuf = NULL;
    UINTN  cfgSize = 0;
    if (ReadConfigFile(ImageHandle, &cfgBuf, &cfgSize) == EFI_SUCCESS) {
        ParseConfig(cfgBuf, cfgSize);
        uefi_call_wrapper(gBS->FreePool, 1, cfgBuf);
    }
    if (gEntryCount == 0) DefaultEntries();
    if (gDefault >= gEntryCount) gDefault = 0;

    UINTN selected  = gDefault;
    INTN  remaining = gTimeout;

    DrawMenu(selected, remaining);

    EFI_EVENT timer;
    uefi_call_wrapper(gBS->CreateEvent, 5,
                      EVT_TIMER, 0, NULL, NULL, &timer);
    uefi_call_wrapper(gBS->SetTimer, 3,
                      timer, TimerPeriodic, 10000000ULL); /* 1s */

    EFI_EVENT events[2] = { gIn->WaitForKey, timer };

    for (;;) {
        UINTN idx;
        uefi_call_wrapper(gBS->WaitForEvent, 3, 2, events, &idx);

        if (idx == 1) {
            if (remaining > 0) {
                remaining--;
                DrawMenu(selected, remaining);
            } else if (remaining == 0) {
                break;
            }
            continue;
        }

        EFI_INPUT_KEY key;
        if (uefi_call_wrapper(gIn->ReadKeyStroke, 2, gIn, &key)
                != EFI_SUCCESS) continue;

        remaining = -1; /* user input cancels the countdown */

        if (key.ScanCode == SCAN_UP) {
            selected = (selected == 0) ? gEntryCount - 1 : selected - 1;
        } else if (key.ScanCode == SCAN_DOWN) {
            selected = (selected + 1) % gEntryCount;
        } else if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            break;
        }
        DrawMenu(selected, remaining);
    }

    uefi_call_wrapper(gBS->CloseEvent, 1, timer);
    SetAttr(ATTR_NORMAL);
    uefi_call_wrapper(gOut->ClearScreen, 1, gOut);

    EFI_STATUS Status = BootEntryRun(ImageHandle, &gEntries[selected]);

    /* Only reached if the kernel handover failed. */
    Print(L"\nBoot failed: %r\nPress any key to reboot.\n", Status);
    EFI_INPUT_KEY k; UINTN idx;
    uefi_call_wrapper(gBS->WaitForEvent, 3, 1, &gIn->WaitForKey, &idx);
    uefi_call_wrapper(gIn->ReadKeyStroke, 2, gIn, &k);
    uefi_call_wrapper(gST->RuntimeServices->ResetSystem, 4,
                      EfiResetCold, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;
}