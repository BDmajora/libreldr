/*
 * libreldr — a UEFI-only bootloader for YetiOS.
 * Hand-off model: Loads Linux kernels via the EFI stub (LoadImage + StartImage).
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

/* -------------------------------------------------------------------- */
/* Helpers                                                              */
/* -------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------- */
/* Configuration Logic                                                  */
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
/* UI & Execution                                                       */
/* -------------------------------------------------------------------- */

#define ATTR_NORMAL    EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK)
#define ATTR_SELECTED  EFI_TEXT_ATTR(EFI_BLACK,     EFI_LIGHTGRAY)

static void DrawMenu(UINTN selected, INTN remaining) {
    uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, ATTR_NORMAL);
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);
    uefi_call_wrapper(ST->ConOut->EnableCursor, 2, ST->ConOut, FALSE);

    uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, 0);
    uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, L"Please select the operating system to start:");

    for (UINTN i = 0; i < gEntryCount; i++) {
        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, 2 + i);
        uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, L"        ");
        if (i == selected) {
            uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, ATTR_SELECTED);
            uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, gEntries[i].Title);
            uefi_call_wrapper(ST->ConOut->SetAttribute, 2, ST->ConOut, ATTR_NORMAL);
        } else {
            uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, gEntries[i].Title);
        }
    }

    UINTN r = 2 + gEntryCount;
    uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, r + 1);
    uefi_call_wrapper(ST->ConOut->OutputString, 2, ST->ConOut, L"Use \x2191 and \x2193 to move highlight. ENTER to boot.");

    if (remaining >= 0) {
        uefi_call_wrapper(ST->ConOut->SetCursorPosition, 3, ST->ConOut, 0, r + 3);
        Print(L"Automatic boot in: %d", remaining);
    }
}

static EFI_STATUS BootEntryRun(EFI_HANDLE ImageHandle, BootEntry *entry) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE *LoadedImage;
    EFI_DEVICE_PATH  *DevicePath;
    EFI_HANDLE        NewImage;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    DevicePath = FileDevicePath(LoadedImage->DeviceHandle, entry->Kernel);
    if (!DevicePath) return EFI_OUT_OF_RESOURCES;

    Status = uefi_call_wrapper(BS->LoadImage, 6, FALSE, ImageHandle, DevicePath, NULL, 0, &NewImage);
    if (EFI_ERROR(Status)) return Status;

    Status = uefi_call_wrapper(BS->HandleProtocol, 3, NewImage, &LoadedImageProtocol, (VOID **)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    LoadedImage->LoadOptions     = entry->Options;
    LoadedImage->LoadOptionsSize = (StrLen(entry->Options) + 1) * sizeof(CHAR16);

    return uefi_call_wrapper(BS->StartImage, 3, NewImage, NULL, NULL);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    InitializeLib(ImageHandle, SystemTable);

    UINTN best_mode = 0, best_area = 0;
    for (UINTN m = 0; m < (UINTN)ST->ConOut->Mode->MaxMode; m++) {
        UINTN c, r;
        if (uefi_call_wrapper(ST->ConOut->QueryMode, 4, ST->ConOut, m, &c, &r) == EFI_SUCCESS && c * r > best_area) {
            best_area = c * r; best_mode = m;
        }
    }
    uefi_call_wrapper(ST->ConOut->SetMode, 2, ST->ConOut, best_mode);
    uefi_call_wrapper(ST->ConOut->QueryMode, 4, ST->ConOut, best_mode, &gCols, &gRows);

    CHAR8 *cfgBuf = NULL;
    UINTN  cfgSize = 0;
    if (ReadConfigFile(ImageHandle, &cfgBuf, &cfgSize) == EFI_SUCCESS) {
        ParseConfig(cfgBuf, cfgSize);
        uefi_call_wrapper(BS->FreePool, 1, cfgBuf);
    }

    if (gEntryCount == 0) return EFI_NOT_FOUND;

    UINTN selected  = gDefault;
    INTN  remaining = gTimeout;

    EFI_EVENT timer;
    uefi_call_wrapper(BS->CreateEvent, 5, EVT_TIMER, 0, NULL, NULL, &timer);
    uefi_call_wrapper(BS->SetTimer, 3, timer, TimerPeriodic, 10000000ULL);
    EFI_EVENT events[2] = { ST->ConIn->WaitForKey, timer };

    for (;;) {
        UINTN idx;
        DrawMenu(selected, remaining);
        uefi_call_wrapper(BS->WaitForEvent, 3, 2, events, &idx);

        if (idx == 1) {
            if (remaining > 0) remaining--;
            else if (remaining == 0) break;
            continue;
        }

        EFI_INPUT_KEY key;
        if (uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key) != EFI_SUCCESS) continue;
        remaining = -1;

        if (key.ScanCode == SCAN_UP) selected = (selected == 0) ? gEntryCount - 1 : selected - 1;
        else if (key.ScanCode == SCAN_DOWN) selected = (selected + 1) % gEntryCount;
        else if (key.UnicodeChar == CHAR_CARRIAGE_RETURN) break;
    }

    uefi_call_wrapper(BS->CloseEvent, 1, timer);
    uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

    EFI_STATUS Status = BootEntryRun(ImageHandle, &gEntries[selected]);
    Print(L"Boot failed: %r\n", Status);
    
    uefi_call_wrapper(RT->ResetSystem, 4, EfiResetCold, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS;
}