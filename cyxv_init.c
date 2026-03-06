/*
 * cyxv_init.c — DLL constructor: legitimately register the CyXV extension
 *
 * Extension registration uses the standard Xorg API:
 *   AddExtension(name, NumEvents, NumErrors, MainProc, SwappedProc,
 *                CloseDownProc, MinorOpcodeProc)
 *
 * AddExtension fills ProcVector[opcode] and SwappedProcVector[opcode]
 * itself — no manual patching of dispatch tables required or performed.
 *
 * The extension name presented to clients is controlled by config:
 *   xvcompat = true   →  "XVideo"    (XV-compatible; mplayer/mpv find it)
 *   xvcompat = false  →  "CyXV-D3D"  (native name; no XV masquerade)
 *
 * Config file: $CYXV_CONFIG | ~/.config/cyxv.conf | /etc/cyxv.conf
 *
 * Symbol resolution (ASLR-safe, no hardcoded VAs):
 *   1. dlsym(RTLD_DEFAULT, name)   — Cygwin POSIX linker (proven by GDB)
 *   2. GetProcAddress(exe, name)   — Windows PE export table
 *   3. /proc/self/maps base + RVA  — last resort per-build offset
 *
 * Build:
 *   gcc -O2 -shared -fPIC -o cyxv.dll \
 *       cyxv_init.c cyxv_dispatch.c cyxv_config.c -lX11 -lpthread
 *
 * Usage:
 *   LD_PRELOAD=$PWD/cyxv.dll XWin :0 -multiwindow -listen tcp &
 */

#define _GNU_SOURCE
#include "cyxv_config.h"
#include "cyxv_dispatch.h"
#include "cyxv_xvproto.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <X11/Xlib.h>

#ifdef __CYGWIN__
# include <windows.h>
#endif

/* ── Build-specific RVA fallbacks ────────────────────────────────────────
 * Used only if dlsym and GetProcAddress both fail.
 * Derive from GDB: RVA = (gdb info address <sym>) VA  −  image_base
 * where image_base = first address in /proc/self/maps for XWin.exe
 */
#define RVA_AddExtension    0x48a4e0ULL
#define RVA_WriteToClient   0x546de0ULL

/* ── Xorg ExtensionEntry (fields we need) ────────────────────────────── */

typedef struct {
    int   index;
    char *name;
    short base;       /* assigned major opcode ← key field */
    short eventBase;
    short errorBase;
} ExtensionEntry;

/* ── Function-pointer types ──────────────────────────────────────────── */

typedef ExtensionEntry * (*AddExtensionFn)(
    const char *name,
    int NumEvents,
    int NumErrors,
    int            (*MainProc)(void *),
    int            (*SwappedProc)(void *),
    void           (*CloseDown)(ExtensionEntry *),
    unsigned short (*MinorOpcode)(void *)
);

typedef void (*WriteToClientFn)(void *client, int len, const void *data);

/* ── /proc/self/maps: XWin.exe image base ────────────────────────────── */

static uintptr_t xwin_image_base(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "XWin") && strchr(line, 'r')) {
            base = (uintptr_t)strtoull(line, NULL, 16);
            break;
        }
    }
    fclose(f);
    return base;
}

/* ── Symbol resolver ─────────────────────────────────────────────────── */

static void *resolve(const char *name, uint64_t rva) {
    /* 1. Cygwin POSIX dynamic linker — works because GDB confirms the
     *    symbol is in XWin.exe's exported Cygwin symbol table.         */
    void *sym = dlsym(RTLD_DEFAULT, name);
    if (sym) { fprintf(stderr, "[CyXV] %-22s dlsym       → %p\n", name, sym); return sym; }

#ifdef __CYGWIN__
    /* 2. Windows PE export table */
    sym = (void *)GetProcAddress(GetModuleHandle(NULL), name);
    if (sym) { fprintf(stderr, "[CyXV] %-22s GetProcAddr → %p\n", name, sym); return sym; }
#endif

    /* 3. image_base + RVA (ASLR-safe: RVA is constant per build) */
    uintptr_t base = xwin_image_base();
    if (base && rva) {
        sym = (void *)(base + rva);
        fprintf(stderr, "[CyXV] %-22s base+RVA    → %p\n", name, sym);
        return sym;
    }

    fprintf(stderr, "[CyXV] FATAL: cannot resolve %s\n", name);
    return NULL;
}

/* ── Init thread ─────────────────────────────────────────────────────── */

static void *init_thread(void *arg) {
    (void)arg;

    /* Load config before anything else */
    CyxvConfig cfg;
    cyxv_config_load(&cfg);

    const char *extname = cyxv_extension_name(&cfg);
    fprintf(stderr, "[CyXV] xvcompat=%s → registering as \"%s\"\n",
            cfg.xvcompat ? "true" : "false", extname);

    /* Wait for XWin's InitExtensions() to complete.
     * The server logs "Initializing built-in extension ..." during startup.
     * AddExtension is only safe to call after that sequence finishes.    */
    sleep(2);

    /* ── Resolve required symbols ─────────────────────────────────── */

    AddExtensionFn  add_ext = resolve("AddExtension",  RVA_AddExtension);
    WriteToClientFn wtc     = resolve("WriteToClient", RVA_WriteToClient);

    if (!add_ext) {
        fprintf(stderr, "[CyXV] Cannot find AddExtension — aborting\n");
        return NULL;
    }

    if (wtc)  cyxv_set_write_fn(wtc);
    else      fprintf(stderr, "[CyXV] WARNING: WriteToClient not found — replies disabled\n");

    /* ── Private Display for render thread ────────────────────────── */

    Display *dpy = XOpenDisplay(cfg.display);
    if (dpy) {
        cyxv_set_display(dpy, DefaultScreen(dpy));
        cyxv_start_render_thread();
    } else {
        fprintf(stderr, "[CyXV] WARNING: XOpenDisplay(%s) failed — rendering disabled\n",
                cfg.display);
    }

    /* ── Register extension (standard Xorg API, no patching) ─────── */

    ExtensionEntry *entry = add_ext(
        extname,              /* "XVideo" or "CyXV-D3D" per config     */
        0,                    /* NumEvents — no async events in MVP     */
        0,                    /* NumErrors                              */
        cyxv_dispatch,        /* MainProc — fills ProcVector[opcode]    */
        cyxv_dispatch_swapped,/* SwappedProc — fills SwappedProcVector  */
        NULL,                 /* CloseDownProc                          */
        cyxv_minor_opcode     /* MinorOpcodeProc                        */
    );
    /* AddExtension internally writes ProcVector[entry->base] = MainProc
     * and SwappedProcVector[entry->base] = SwappedProc.
     * No manual VirtualProtect or vector patching needed.               */

    if (entry) {
        fprintf(stderr,
                "[CyXV] \"%s\" registered: opcode=%d entry=%p\n"
                "[CyXV] Verify: DISPLAY=:0 xdpyinfo | grep %s\n",
                extname, (int)(unsigned short)entry->base, (void *)entry,
                cfg.xvcompat ? "XVideo" : "CyXV-D3D");
    } else {
        fprintf(stderr,
                "[CyXV] AddExtension returned NULL\n"
                "[CyXV] Possible causes: already registered, called too early,\n"
                "[CyXV] or extension table is full (max 128 extensions).\n");
    }

    return NULL;
}

/* ── DLL constructor ─────────────────────────────────────────────────── */

__attribute__((constructor))
static void cyxv_ctor(void) {
    fprintf(stderr, "[CyXV] libcyxv loaded — spawning init thread\n");
    pthread_t tid;
    pthread_create(&tid, NULL, init_thread, NULL);
    pthread_detach(tid);
}
