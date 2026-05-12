/*
 * libreldr_bios.c — BIOS-side libreldr.
 *
 * Built as a syslinux com32 module (libreldr.c32). syslinux loads us as
 * the UI, we draw the ReactOS MinimalUI menu, then call
 * syslinux_run_kernel_image() to chain to the chosen Linux kernel.
 *
 * Reads /libreldr.conf from the boot partition (same format as the
 * UEFI side).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <console.h>
#include <getkey.h>
#include <syslinux/boot.h>
#include <syslinux/loadfile.h>

#define MAX_ENTRIES  16
#define MAX_TITLE    128
#define MAX_PATH     256
#define MAX_OPTS     512

typedef struct {
    char title[MAX_TITLE];
    char kernel[MAX_PATH];
    char initrd[MAX_PATH];
    char options[MAX_OPTS];
} BootEntry;

static BootEntry g_entries[MAX_ENTRIES];
static int       g_count    = 0;
static int       g_default  = 0;
static int       g_timeout  = 10;

/* ---- ANSI helpers ------------------------------------------------- */

#define ESC "\033"
static void goto_xy(int x, int y) { printf(ESC "[%d;%dH", y + 1, x + 1); }
static void clear_screen(void)    { printf(ESC "[2J" ESC "[H"); }
static void attr_normal(void)     { printf(ESC "[0;37;40m"); }  /* light gray on black */
static void attr_selected(void)   { printf(ESC "[0;30;47m"); }  /* black on light gray */
static void hide_cursor(void)     { printf(ESC "[?25l"); }
static void show_cursor(void)     { printf(ESC "[?25h"); }

/* ---- Config parsing ----------------------------------------------- */

static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

static void parse_config(const char *buf, size_t size)
{
    char line[1024];
    size_t pos = 0;
    BootEntry *cur = NULL;

    while (pos < size) {
        size_t i = 0;
        while (pos < size && buf[pos] != '\n' && i < sizeof(line) - 1)
            line[i++] = buf[pos++];
        line[i] = 0;
        if (pos < size && buf[pos] == '\n') pos++;
        trim(line);
        if (line[0] == 0 || line[0] == '#') continue;

        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        char *val = sp + 1;
        while (*val == ' ' || *val == '\t') val++;

        if (strcmp(line, "timeout") == 0) {
            g_timeout = atoi(val);
        } else if (strcmp(line, "default") == 0) {
            g_default = atoi(val);
        } else if (strcmp(line, "title") == 0) {
            if (g_count >= MAX_ENTRIES) continue;
            cur = &g_entries[g_count++];
            memset(cur, 0, sizeof(*cur));
            strncpy(cur->title, val, MAX_TITLE - 1);
        } else if (strcmp(line, "linux") == 0 && cur) {
            strncpy(cur->kernel, val, MAX_PATH - 1);
        } else if (strcmp(line, "initrd") == 0 && cur) {
            strncpy(cur->initrd, val, MAX_PATH - 1);
        } else if (strcmp(line, "options") == 0 && cur) {
            strncpy(cur->options, val, MAX_OPTS - 1);
        }
    }
}

static void load_config(void)
{
    void *data = NULL;
    size_t size = 0;
    if (loadfile("/libreldr.conf", &data, &size) == 0 && data) {
        parse_config((const char *)data, size);
        free(data);
    }
    if (g_count == 0) {
        /* Hardcoded fallback so we still boot something. */
        strcpy(g_entries[0].title,   "YetiOS");
        strcpy(g_entries[0].kernel,  "/vmlinuz");
        strcpy(g_entries[0].initrd,  "/initramfs.img");
        strcpy(g_entries[0].options, "root=LABEL=yetios-root ro quiet");
        g_count = 1;
        g_timeout = 5;
    }
}

/* ---- Menu rendering ----------------------------------------------- */

#define SCREEN_ROWS 25

static void draw_menu(int selected, int remaining)
{
    attr_normal();
    clear_screen();
    hide_cursor();

    /* Row 0: header, left-aligned, no indent. */
    goto_xy(0, 0);
    printf("Please select the operating system to start:");

    /* Rows 2+: entries with 4-space indent. */
    for (int i = 0; i < g_count; i++) {
        goto_xy(0, 2 + i);
        printf("    ");
        if (i == selected) {
            attr_selected();
            printf("%s", g_entries[i].title);
            attr_normal();
        } else {
            printf("%s", g_entries[i].title);
        }
    }

    int r = 2 + g_count;

    /* Two-line prompt, exactly like ReactOS. */
    goto_xy(0, r + 1);
    printf("Use \x18 and \x19 to move the highlight to your choice.");
    goto_xy(0, r + 2);
    printf("Press ENTER to choose.");

    /* Countdown, blank line above it. */
    if (remaining >= 0) {
        goto_xy(0, r + 4);
        printf("Seconds until highlighted choice will be started automatically: %d ",
               remaining);
    }

    /* F8 hint at bottom of screen. */
    goto_xy(0, SCREEN_ROWS - 2);
    printf("For troubleshooting and advanced startup options for YetiOS, press F8.");

    fflush(stdout);
}

/* ---- Boot a chosen entry ------------------------------------------ */

static void boot_entry(BootEntry *e)
{
    /* Build the syslinux-style command line: "kernel initrd=... options" */
    char cmdline[MAX_PATH + MAX_OPTS + 64];

    if (e->initrd[0]) {
        snprintf(cmdline, sizeof(cmdline), "initrd=%s %s",
                 e->initrd, e->options);
    } else {
        snprintf(cmdline, sizeof(cmdline), "%s", e->options);
    }

    attr_normal();
    clear_screen();
    show_cursor();

    /* This does not return on success. */
    syslinux_run_kernel_image(e->kernel, cmdline, 0, IMAGE_TYPE_KERNEL);

    /* If it did return, the kernel failed to load. */
    printf("\nFailed to boot %s\nPress any key to reboot.\n", e->kernel);
    get_key(stdin, 0);
    syslinux_reboot(0);
}

/* ---- Entry point -------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    openconsole(&dev_stdcon_r, &dev_ansiserial_w);

    load_config();
    if (g_default >= g_count) g_default = 0;

    int selected  = g_default;
    int remaining = g_timeout;

    draw_menu(selected, remaining);

    while (1) {
        /* Poll-based countdown: get_key with 1s timeout. */
        int key = get_key(stdin, remaining > 0 ? 10 : 0);   /* tenths of sec */

        if (key == KEY_NONE) {
            if (remaining > 0) {
                remaining--;
                draw_menu(selected, remaining);
                if (remaining == 0) break;
            }
            continue;
        }

        /* User pressed something — cancel autoboot. */
        remaining = -1;

        if (key == KEY_UP) {
            selected = (selected == 0) ? g_count - 1 : selected - 1;
        } else if (key == KEY_DOWN) {
            selected = (selected + 1) % g_count;
        } else if (key == '\r' || key == '\n' || key == KEY_ENTER) {
            break;
        }
        draw_menu(selected, remaining);
    }

    boot_entry(&g_entries[selected]);
    return 0;
}