/*
 * libreldr — a UEFI-only bootloader for YetiOS.
 * Hand-off model: Loads Linux kernels via the EFI stub (LoadImage + StartImage).
 *
 * UI mimics ReactOS FreeLoader:
 *   - Entries indented 2 spaces.
 *   - Selected entry has highlight bar spanning *only* the width of the
 *     widest title (plus a small padding), not the full screen.
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
static UINTN     gCols, gRows;
static UINTN     gHighlightWidth = 0;

#define ENTRY_INDENT       2
#define HIGHLIGHT_PADDING  4

/* -------------------------------------------------------------------- */
/* Helpers                                                              */
/* -------------------------------------------------------------------- */

static UINTN StrLen16(const CHAR16 *s) {
    UINTN n = 0;
    while (*s++) n++;
    return n;
}

static BOOLEAN StrEqA(const CHAR8 *s, UINTN len, const char *kw) {
    UINTN kwlen = 0;
    while (kw[kwlen]) kwlen++;
    if (len != kwlen) return FALSE;
    for (UINTN i = 0; i < len; i++) {
        if (s[i] != (CHAR8)kw[i]) return FALSE;
    }
    return TRUE;
}

static void AsciiToUcs2(const CHAR8 *src, UINTN srcLen, CHAR16 *dst, UINTN dstCap) {
    UINTN j = 0;
    for (UINTN i = 0; i < srcLen && j < dstCap - 1; i++) {
        CHAR8 c = src[i];
        if (c == '\r' || c == '\n') break;
        dst[j++] = (CHAR16)c;
    }
    dst[j] = 0;
}

static void NormalizePath(CHAR16 *p) {
    while (*p) { if (*p == L'/') *p = L'\\'; p++; }
}

static INTN ParseInt(const CHAR8 *s, UINTN len) {
    INTN v = 0; UINTN i = 0; BOOLEAN neg = FALSE;
    if (i < len && s[i] == '-') { neg = TRUE; i++; }
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + (s[i] - '0');
    }
    return neg ? -v : v;
}

static void PutCh(CHAR16 c) {
    CHAR16 buf[2] = { c, 0 };
    uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, buf);
}

static void Spaces(UINTN count) {
    for (UINTN i = 0; i < count; i++) PutCh(L' ');
}

/* -------------------------------------------------------------------- */
/* Configuration                                                        */
/* -------------------------------------------------------------------- */

static EFI_STATUS ReadConfigFile(EFI_HANDLE ImageHandle, CHAR8 **buf, UINTN *size) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE      *LoadedImage;
    EFI_FILE_IO_INTERFACE *FsProto;
    EFI_FILE_HANDLE        Root, File;
    EFI_GUID FsGuid = SIMPLE_FILE_SYSTEM_PROTOCOL;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, LoadedImage->DeviceHandle, &FsGuid, (VOID **)&FsProto);
    if (EFI_ERROR(Status)) return Status;

    Status = uefi_call_wrapper(FsProto->OpenVolume, 2, FsProto, &Root);
    if (EFI_ERROR(Status)) return Status;

    Status = uefi_call_wrapper(Root->Open, 5, Root, &File, L"\\EFI\\libreldr\\libreldr.conf", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        uefi_call_wrapper(Root->Close, 1, Root);
        return Status;
    }

    UINT8 InfoBuf[sizeof(EFI_FILE_INFO) + 256];
    UINTN InfoSize = sizeof(InfoBuf);
    EFI_GUID InfoGuid = EFI_FILE_INFO_ID;
    Status = uefi_call_wrapper(File->GetInfo, 4, File, &InfoGuid, &InfoSize, InfoBuf);
    if (EFI_ERROR(Status)) goto done;

    UINT64 fsize = ((EFI_FILE_INFO *)InfoBuf)->FileSize;
    if (fsize > 65536) fsize = 65536;

    Status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, (UINTN)fsize + 1, (VOID **)buf);
    if (EFI_ERROR(Status)) goto done;

    UINTN ReadSize = (UINTN)fsize;
    Status = uefi_call_wrapper(File->Read, 3, File, &ReadSize, *buf);
    if (EFI_ERROR(Status)) {
        uefi_call_wrapper(BS->FreePool, 1, *buf);
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

static void ParseConfig(const CHAR8 *buf, UINTN size) {
    UINTN pos = 0;
    BootEntry *cur = NULL;
    const CHAR8 *line;
    UINTN len;

    while (pos < size) {
        UINTN start = pos;
        while (pos < size && buf[pos] != '\n') pos++;
        UINTN end = pos;
        if (end > start && buf[end - 1] == '\r') end--;
        if (pos < size) pos++;
        line = &buf[start];
        len = end - start;

        while (len > 0 && (*line == ' ' || *line == '\t')) { line++; len--; }
        if (len == 0 || line[0] == '#') continue;

        UINTN k = 0;
        while (k < len && line[k] != ' ' && line[k] != '\t') k++;
        const CHAR8 *val = &line[k];
        UINTN vallen = len - k;
        while (vallen > 0 && (*val == ' ' || *val == '\t')) { val++; vallen--; }

        if (StrEqA(line, k, "timeout")) {
            gTimeout = ParseInt(val, vallen);
        } else if (StrEqA(line, k, "default")) {
            gDefault = (UINTN)ParseInt(val, vallen);
        } else if (StrEqA(line, k, "title")) {
            if (gEntryCount < MAX_ENTRIES) {
                cur = &gEntries[gEntryCount++];
                cur->Used = TRUE;
                AsciiToUcs2(val, vallen, cur->Title, MAX_TITLE);
            }
        } else if (StrEqA(line, k, "linux") && cur) {
            AsciiToUcs2(val, vallen, cur->Kernel, MAX_PATH_LEN);
            NormalizePath(cur->Kernel);
        } else if (StrEqA(line, k, "options") && cur) {
            AsciiToUcs2(val, vallen, cur->Options, MAX_OPTS);
        }
    }
}

/* -------------------------------------------------------------------- */
/* UI                                                                   */
/* -------------------------------------------------------------------- */

#define ATTR_NORMAL    EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)
#define ATTR_SELECTED  EFI_TEXT_ATTR(EFI_BLACK,     EFI_LIGHTGRAY)

#define ROW_HEADER      0
#define ROW_FIRST_ENTRY 2
#define ROW_AFTER_LIST  (ROW_FIRST_ENTRY + gEntryCount)
#define ROW_HINT1       (ROW_AFTER_LIST + 1)
#define ROW_HINT2       (ROW_AFTER_LIST + 2)
#define ROW_COUNTDOWN   (ROW_AFTER_LIST + 4)
#define ROW_F8          (ROW_AFTER_LIST + 6)

/* Highlight bar = widest title + padding, clamped to fit on the line. */
static void ComputeHighlightWidth(void) {
    UINTN maxLen = 0;
    for (UINTN i = 0; i < gEntryCount; i++) {
        UINTN n = StrLen16(gEntries[i].Title);
        if (n > maxLen) maxLen = n;
    }
    UINTN w = maxLen + HIGHLIGHT_PADDING;
    UINTN avail = (gCols > ENTRY_INDENT + 1) ? gCols - ENTRY_INDENT - 1 : 0;
    if (w > avail) w = avail;
    gHighlightWidth = w;
}

static void DrawEntry(UINTN idx, BOOLEAN isSelected) {
    uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut,
                      0, ROW_FIRST_ENTRY + idx);
    Spaces(ENTRY_INDENT);

    if (isSelected) {
        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, ATTR_SELECTED);
    }

    UINTN written = 0;
    CHAR16 *t = gEntries[idx].Title;
    while (*t && written < gHighlightWidth) {
        PutCh(*t++);
        written++;
    }
    while (written < gHighlightWidth) {
        PutCh(L' ');
        written++;
    }

    if (isSelected) {
        uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, ATTR_NORMAL);
    }
}

static void DrawMenuFull(UINTN selected) {
    uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, ATTR_NORMAL);
    uefi_call_wrapper(ST->ConOut->ClearScreen,  1, ST->ConOut);
    uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, FALSE);

    uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, ROW_HEADER);
    uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut,
        L"Please select the operating system to start:");

    for (UINTN i = 0; i < gEntryCount; i++) {
        DrawEntry(i, i == selected);
    }

    uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, ROW_HINT1);
    uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut,
        L"Use \x2191 and \x2193 to move the highlight to your choice.");
    uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, ROW_HINT2);
    uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut,
        L"Press ENTER to choose.");

    uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, ROW_F8);
    uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut,
        L"For troubleshooting and advanced startup options for YetiOS, press F8.");
}

static void DrawCountdown(INTN remaining) {
    uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, ATTR_NORMAL);
    uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, ROW_COUNTDOWN);
    if (remaining >= 0) {
        Print(L"Seconds until highlighted choice will be started automatically: %d  ",
              remaining);
    } else {
        UINTN limit = (gCols > 0 && gCols < 80) ? gCols - 1 : 79;
        for (UINTN i = 0; i < limit; i++) PutCh(L' ');
    }
}

/* -------------------------------------------------------------------- */
/* Boot execution                                                       */
/* -------------------------------------------------------------------- */

static EFI_STATUS BootEntryRun(EFI_HANDLE ImageHandle, BootEntry *entry) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE *LoadedImage;
    EFI_DEVICE_PATH  *DevicePath;
    EFI_HANDLE        NewImage;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle,
                               &LoadedImageProtocol, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    DevicePath = FileDevicePath(LoadedImage->DeviceHandle, entry->Kernel);
    if (!DevicePath) return EFI_OUT_OF_RESOURCES;

    Status = uefi_call_wrapper(BS->LoadImage, 6, FALSE, ImageHandle,
                               DevicePath, NULL, 0, &NewImage);
    if (EFI_ERROR(Status)) return Status;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, NewImage,
                               &LoadedImageProtocol, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    LoadedImage->LoadOptions     = entry->Options;
    LoadedImage->LoadOptionsSize = (StrLen(entry->Options) + 1) * sizeof(CHAR16);

    return uefi_call_wrapper(BS->StartImage, 3, NewImage, NULL, NULL);
}

/* -------------------------------------------------------------------- */
/* Entry point                                                          */
/* -------------------------------------------------------------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);

    UINTN best_mode = 0, best_area = 0;
    for (UINTN m = 0; m < (UINTN)ST->ConOut->Mode->MaxMode; m++) {
        UINTN c, r;
        if (uefi_call_wrapper(ST->ConOut->QueryMode, 4, ST->ConOut, m, &c, &r) == EFI_SUCCESS
                && c * r > best_area) {
            best_area = c * r;
            best_mode = m;
        }
    }
    uefi_call_wrapper(ST->ConOut->SetMode, 2, ST->ConOut, best_mode);
    uefi_call_wrapper(ST->ConOut->QueryMode, 4, ST->ConOut, best_mode, &gCols, &gRows);

    if (gCols == 0) gCols = 80;
    if (gRows == 0) gRows = 25;

    CHAR8 *cfgBuf  = NULL;
    UINTN  cfgSize = 0;
    if (ReadConfigFile(ImageHandle, &cfgBuf, &cfgSize) == EFI_SUCCESS) {
        ParseConfig(cfgBuf, cfgSize);
        uefi_call_wrapper(BS->FreePool, 1, cfgBuf);
    }

    if (gEntryCount == 0) return EFI_NOT_FOUND;

    ComputeHighlightWidth();

    UINTN selected  = gDefault;
    INTN  remaining = gTimeout;

    DrawMenuFull(selected);
    DrawCountdown(remaining);

    EFI_EVENT timer;
    uefi_call_wrapper(BS->CreateEvent, 5, EVT_TIMER, 0, NULL, NULL, &timer);
    uefi_call_wrapper(BS->SetTimer, 3, timer, TimerPeriodic, 10000000ULL);
    EFI_EVENT events[2] = { ST->ConIn->WaitForKey, timer };

    for (;;) {
        UINTN idx;
        uefi_call_wrapper(BS->WaitForEvent, 3, 2, events, &idx);

        if (idx == 1) {
            if (remaining > 0) {
                remaining--;
                DrawCountdown(remaining);
            } else if (remaining == 0) {
                break;
            }
            continue;
        }

        EFI_INPUT_KEY key;
        if (uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key) != EFI_SUCCESS)
            continue;

        if (remaining >= 0) {
            remaining = -1;
            DrawCountdown(-1);
        }

        if (key.ScanCode == SCAN_UP) {
            UINTN prev = selected;
            selected = (selected == 0) ? gEntryCount - 1 : selected - 1;
            DrawEntry(prev,     FALSE);
            DrawEntry(selected, TRUE);
        } else if (key.ScanCode == SCAN_DOWN) {
            UINTN prev = selected;
            selected = (selected + 1) % gEntryCount;
            DrawEntry(prev,     FALSE);
            DrawEntry(selected, TRUE);
        } else if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) {
            break;
        }
    }

    uefi_call_wrapper(BS->CloseEvent, 1, timer);
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

    EFI_STATUS Status = BootEntryRun(ImageHandle, &gEntries[selected]);
    Print(L"Boot failed: %r\n", Status);

    EFI_EVENT pause;
    uefi_call_wrapper(BS->CreateEvent, 5, EVT_TIMER, 0, NULL, NULL, &pause);
    uefi_call_wrapper(BS->SetTimer, 3, pause, TimerRelative, 50000000ULL);
    UINTN dummy;
    uefi_call_wrapper(BS->WaitForEvent, 3, 1, &pause, &dummy);
    uefi_call_wrapper(BS->CloseEvent, 1, pause);

    uefi_call_wrapper(RT->ResetSystem, 4, EfiResetCold, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;
}