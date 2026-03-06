/*
 * cyxv_init.c — DLL constructor: register XV extension via AddExtension,
 *               then verify/patch ProcVector & SwappedProcVector directly.
 *
 * ASLR solution:
 *   GDB confirmed that ProcVector, SwappedProcVector, AddExtension, and
 *   WriteToClient are all visible as named symbols in XWin.exe's Cygwin
 *   dynamic symbol table.  dlsym(NULL, name) resolves them at runtime
 *   without any hardcoded address — fully ASLR-safe.
 *
 *   Fallback chain (only if dlsym fails):
 *     GetProcAddress(GetModuleHandle(NULL), name)  ← Windows PE export table
 *     /proc/self/maps base + build-specific RVA    ← last resort
 *
 * Injection flow:
 *   1. dlsym → AddExtension, WriteToClient, ProcVector, SwappedProcVector
 *   2. AddExtension("XVideo") → XWin assigns a major opcode N, fills
 *      ProcVector[N] with our handler automatically.
 *   3. We read back N from ExtensionEntry.base and double-check that
 *      ProcVector[N] == our handler; if not (some builds skip filling it),
 *      we patch it directly.
 *   4. SwappedProcVector[N] is always set explicitly (some builds leave it
 *      pointing to a no-op swapped stub).
 *
 * Build:
 *   gcc -O2 -shared -fPIC -o cyxv.dll cyxv_init.c cyxv_dispatch.c \
 *       -lX11 -lpthread
 *
 * Usage:
 *   LD_PRELOAD=$PWD/cyxv.dll XWin :0 -multiwindow -listen tcp &
 */

#define _GNU_SOURCE
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

/* ── Build-specific RVA fallbacks (derive from GDB: VA - image_base) ────────
 *
 *   (gdb) info address AddExtension
 *   Symbol "AddExtension" is at 0x10048a4e0
 *   image_base from /proc/self/maps XWin.exe line: 0x100000000
 *   RVA = 0x10048a4e0 - 0x100000000 = 0x48a4e0
 *
 * These are only used if all three primary methods fail.
 */
#define RVA_AddExtension    0x48a4e0ULL
#define RVA_WriteToClient   0x546de0ULL
#define RVA_ProcVector      0x587600ULL   /* update from GDB: p &ProcVector  */
#define RVA_SwappedProcVector 0x587a00ULL /* p &SwappedProcVector            */

/* ── Xorg ExtensionEntry (only the fields we need) ──────────────────────── */

typedef struct {
    int   index;        /* extension index in extension table */
    char *name;
    short base;         /* ← assigned major opcode */
    short eventBase;
    short errorBase;
    /* ... more fields we don't care about */
} ExtensionEntry;

/* ── Function-pointer types ─────────────────────────────────────────────── */

typedef ExtensionEntry * (*AddExtensionFn)(
    const char *name,
    int NumEvents,
    int NumErrors,
    int            (*MainProc)(void *client),
    int            (*SwappedMainProc)(void *client),
    void           (*CloseDownProc)(ExtensionEntry *),
    unsigned short (*MinorOpcodeProc)(void *client)
);

typedef void (*WriteToClientFn)(void *client, int len, const void *data);

/* ── /proc/self/maps: find XWin.exe image base ──────────────────────────── */

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

/* ── Generic symbol resolver ────────────────────────────────────────────── */
/*
 * Tries (in order):
 *   1. dlsym(NULL, name)            — Cygwin POSIX linker, proven by GDB
 *   2. GetProcAddress(exe, name)    — Windows PE export table
 *   3. image_base + rva             — ASLR-safe offset fallback
 */
static void *resolve(const char *name, uint64_t rva) {
    /* 1 */
    void *sym = dlsym(RTLD_DEFAULT, name);
    if (sym) {
        fprintf(stderr, "[CyXV] %-24s dlsym        → %p\n", name, sym);
        return sym;
    }
#ifdef __CYGWIN__
    /* 2 */
    sym = (void *)GetProcAddress(GetModuleHandle(NULL), name);
    if (sym) {
        fprintf(stderr, "[CyXV] %-24s GetProcAddr  → %p\n", name, sym);
        return sym;
    }
#endif
    /* 3 */
    uintptr_t base = xwin_image_base();
    if (base && rva) {
        sym = (void *)(base + rva);
        fprintf(stderr, "[CyXV] %-24s base+RVA     → %p  (base=%p)\n",
                name, sym, (void *)base);
        return sym;
    }

    fprintf(stderr, "[CyXV] FATAL: cannot resolve %s\n", name);
    return NULL;
}

/* ── Patch a dispatch vector slot ─────────────────────────────────────────
 *
 * ProcVector is a plain C array of function pointers declared as:
 *   int (*ProcVector[256])(ClientPtr);
 * We index it by the opcode returned from AddExtension.
 */
static void patch_vector(void **vec, int slot, void *fn, const char *vecname) {
    if (!vec) return;
#ifdef __CYGWIN__
    /* Make the page writable (it may be in a read-only segment) */
    DWORD old;
    VirtualProtect(&vec[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
#endif
    vec[slot] = fn;
    fprintf(stderr, "[CyXV] %s[%d] = %p\n", vecname, slot, fn);
#ifdef __CYGWIN__
    VirtualProtect(&vec[slot], sizeof(void *), old, &old);
#endif
}

/* ── Delayed init thread ─────────────────────────────────────────────────── */

static void *init_thread(void *arg) {
    (void)arg;

    /* Wait for XWin's InitExtensions() to finish.
     * The server logs "Initializing built-in extension XFIXES" etc.
     * AddExtension is safe to call after that point (~500 ms usual). */
    sleep(2);

    /* ── Resolve all symbols ────────────────────────────────────────── */

    AddExtensionFn  add_ext = resolve("AddExtension",       RVA_AddExtension);
    WriteToClientFn wtc     = resolve("WriteToClient",      RVA_WriteToClient);
    void          **PV      = resolve("ProcVector",         RVA_ProcVector);
    void          **SPV     = resolve("SwappedProcVector",  RVA_SwappedProcVector);

    if (!add_ext) { fprintf(stderr, "[CyXV] Giving up.\n"); return NULL; }

    if (wtc)  cyxv_set_write_fn(wtc);
    else      fprintf(stderr, "[CyXV] WARNING: WriteToClient missing — no replies\n");

    /* ── Open private Display for rendering ─────────────────────────── */
    Display *dpy = XOpenDisplay(":0");
    if (dpy)  cyxv_set_display(dpy, DefaultScreen(dpy));
    else      fprintf(stderr, "[CyXV] WARNING: XOpenDisplay(:0) failed\n");

    /* ── Call AddExtension ──────────────────────────────────────────── */
    ExtensionEntry *entry = add_ext(
        "XVideo",
        0,                    /* NumEvents  */
        0,                    /* NumErrors  */
        cyxv_dispatch,
        cyxv_dispatch_swapped,
        NULL,                 /* CloseDownProc */
        cyxv_minor_opcode
    );

    if (!entry) {
        fprintf(stderr, "[CyXV] AddExtension returned NULL "
                "(already registered, or called too early)\n");
        return NULL;
    }

    int opcode = (int)(unsigned short)entry->base;
    fprintf(stderr, "[CyXV] XVideo registered: opcode=%d  entry=%p\n",
            opcode, (void *)entry);

    /* ── Verify and force-patch both dispatch vectors ───────────────── */
    /*
     * AddExtension fills ProcVector[opcode] automatically.
     * We verify it is our function; if not (paranoia / different build),
     * we patch it.  SwappedProcVector is always patched explicitly because
     * some builds install a generic "not implemented" swapped stub.
     */
    if (PV) {
        if (PV[opcode] != (void *)cyxv_dispatch) {
            fprintf(stderr, "[CyXV] ProcVector[%d] was %p, patching\n",
                    opcode, PV[opcode]);
            patch_vector(PV, opcode, (void *)cyxv_dispatch, "ProcVector");
        } else {
            fprintf(stderr, "[CyXV] ProcVector[%d] already correct ✓\n", opcode);
        }
    }

    if (SPV)
        patch_vector(SPV, opcode, (void *)cyxv_dispatch_swapped, "SwappedProcVector");

    fprintf(stderr, "[CyXV] Done. Test with:\n"
            "  DISPLAY=:0 xdpyinfo | grep XVideo\n"
            "  DISPLAY=:0 mplayer -vo xv <file>\n");
    return NULL;
}

/* ── DLL constructor ─────────────────────────────────────────────────────── */

__attribute__((constructor))
static void cyxv_ctor(void) {
    fprintf(stderr, "[CyXV] libcyxv loaded — spawning init thread\n");
    pthread_t tid;
    pthread_create(&tid, NULL, init_thread, NULL);
    pthread_detach(tid);
}
