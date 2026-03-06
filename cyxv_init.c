/*
 * cyxv_init.c — DLL constructor: find AddExtension, register XV extension
 *
 * ASLR-safe symbol resolution strategy (tried in order):
 *
 *   1. dlsym(NULL, "AddExtension")
 *      Works when Cygwin's POSIX dynamic linker has the symbol
 *      (GCC -export-dynamic or Cygwin auto-export).
 *
 *   2. GetProcAddress(GetModuleHandle(NULL), "AddExtension")
 *      Works when the symbol is in the PE export table
 *      (gcc -Wl,--export-all-symbols or explicit .def file).
 *
 *   3. /proc/self/maps + PE export directory scan
 *      Reads XWin.exe's own PE headers from memory to walk the export
 *      table by name — works even without explicit export directives,
 *      because the PE loader still builds an in-memory export directory
 *      if the image was linked with --export-all-symbols (common for
 *      Cygwin Xorg builds).
 *
 *   4. /proc/self/maps base + known file offset (build-specific)
 *      If we know the RVA of AddExtension for this exact XWin.exe build,
 *      compute VA = image_base + rva.  The RVA is stable per build even
 *      when ASLR moves the image_base.  RVA is stored in CYXV_ADDEXT_RVA.
 *
 * Build:
 *   gcc -O2 -shared -fPIC -o cyxv.dll cyxv_init.c cyxv_dispatch.c \
 *       -lX11 -lpthread -lXext
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
#include <unistd.h>
#include <stdint.h>
#include <X11/Xlib.h>

/* Windows/Cygwin PE headers ─────────────────────────────────────────────── */
#ifdef __CYGWIN__
# include <windows.h>
#else
/* Minimal PE structures for non-Cygwin builds (shouldn't happen) */
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t  BYTE;
#endif

/* ── Build-specific fallback (update from GDB for each XWin.exe build) ────
 *
 * How to find these values for a new build:
 *   (gdb) info address AddExtension
 *   → "Symbol AddExtension is at 0x10048a4e0"
 *   → image_base (from PE header or /proc/self/maps): 0x100000000
 *   → CYXV_ADDEXT_RVA = 0x10048a4e0 - 0x100000000 = 0x48a4e0
 *
 * We use the RVA because it is ASLR-invariant for a given binary build.
 */
#define CYXV_ADDEXT_RVA        0x48a4e0ULL   /* offset within XWin.exe image */
#define CYXV_WRITETOCLIENT_RVA 0x546de0ULL   /* likewise for WriteToClient   */

/* ── Type aliases ─────────────────────────────────────────────────────────── */

typedef void * (*AddExtensionFn)(
    const char *name,
    int NumEvents,
    int NumErrors,
    int  (*MainProc)(void *client),
    int  (*SwappedMainProc)(void *client),
    void (*CloseDownProc)(void *ext),
    unsigned short (*MinorOpcodeProc)(void *client)
);

typedef void (*WriteToClientFn)(void *client, int len, const void *data);

/* ── Method 1: dlsym ────────────────────────────────────────────────────── */

static void *try_dlsym(const char *sym) {
    void *h = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
    if (!h) return NULL;
    void *fn = dlsym(h, sym);
    dlclose(h);
    return fn;
}

/* ── Method 2: Windows GetProcAddress ───────────────────────────────────── */

static void *try_getprocaddr(const char *sym) {
#ifdef __CYGWIN__
    HMODULE exe = GetModuleHandle(NULL);
    if (!exe) return NULL;
    return (void *)GetProcAddress(exe, sym);
#else
    (void)sym;
    return NULL;
#endif
}

/* ── Method 3: Walk in-memory PE export table ────────────────────────────── */
/*
 * Given the image base pointer (from /proc/self/maps), parse the PE
 * optional header's DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] and
 * walk the name/address arrays to find 'sym'.
 *
 * This works for any Windows PE binary regardless of export-table
 * generation flags, because we are reading the PE in our own address
 * space after the loader has already mapped it.
 */
static void *pe_find_export(uintptr_t base, const char *sym) {
#ifdef __CYGWIN__
    __try {
        /* Validate MZ signature */
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;

        IMAGE_NT_HEADERS *nt =
            (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;

        IMAGE_DATA_DIRECTORY *expdir =
            &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (expdir->VirtualAddress == 0) return NULL;

        IMAGE_EXPORT_DIRECTORY *exp =
            (IMAGE_EXPORT_DIRECTORY *)(base + expdir->VirtualAddress);

        DWORD  *names   = (DWORD  *)(base + exp->AddressOfNames);
        WORD   *ordinals= (WORD   *)(base + exp->AddressOfNameOrdinals);
        DWORD  *funcs   = (DWORD  *)(base + exp->AddressOfFunctions);

        for (DWORD i = 0; i < exp->NumberOfNames; i++) {
            const char *name = (const char *)(base + names[i]);
            if (strcmp(name, sym) == 0) {
                DWORD rva = funcs[ordinals[i]];
                return (void *)(base + rva);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
#else
    (void)base; (void)sym;
#endif
    return NULL;
}

/* ── /proc/self/maps parser ─────────────────────────────────────────────── */
/*
 * Find the mapping entry for XWin.exe (the main executable) and return
 * its load base address.
 *
 * /proc/self/maps lines look like:
 *   100000000-100001000 r--p 00000000 08:01 12345  /usr/bin/XWin.exe
 */
static uintptr_t find_xwin_base(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;

    char line[512];
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        /* We want the first executable mapping that ends in XWin.exe
         * or XWin (without .exe on some Cygwin setups).               */
        if (strstr(line, "XWin") && strstr(line, " r")) {
            base = (uintptr_t)strtoull(line, NULL, 16);
            break;
        }
    }
    fclose(f);
    return base;
}

/* ── Method 4: image_base + known RVA ────────────────────────────────────── */

static void *try_rva(uintptr_t base, uint64_t rva) {
    if (!base || !rva) return NULL;
    return (void *)(base + rva);
}

/* ── Master resolver ─────────────────────────────────────────────────────── */

static void *resolve_symbol(const char *sym, uint64_t fallback_rva) {
    void *fn;

    /* 1 */ fn = try_dlsym(sym);
    if (fn) { fprintf(stderr, "[CyXV] %s via dlsym @ %p\n", sym, fn); return fn; }

    /* 2 */ fn = try_getprocaddr(sym);
    if (fn) { fprintf(stderr, "[CyXV] %s via GetProcAddress @ %p\n", sym, fn); return fn; }

    uintptr_t base = find_xwin_base();
    if (base) {
        /* 3 */ fn = pe_find_export(base, sym);
        if (fn) { fprintf(stderr, "[CyXV] %s via PE export @ %p\n", sym, fn); return fn; }

        /* 4 */ fn = try_rva(base, fallback_rva);
        if (fn) { fprintf(stderr, "[CyXV] %s via base(0x%llx)+RVA(0x%llx) @ %p\n",
                          sym, (unsigned long long)base,
                          (unsigned long long)fallback_rva, fn);
                 return fn; }
    } else {
        fprintf(stderr, "[CyXV] WARNING: could not find XWin.exe in /proc/self/maps\n");
    }

    fprintf(stderr, "[CyXV] FATAL: could not resolve %s\n", sym);
    return NULL;
}

/* ── Delayed init thread ─────────────────────────────────────────────────── */

static void *init_thread(void *arg) {
    (void)arg;

    /*
     * Wait for XWin's InitExtensions() to finish.
     * XWin prints "Initializing built-in extension XFIXES" etc. during
     * startup; AddExtension is callable only after that point.
     * 2 s is conservative; 500 ms usually suffices.
     */
    sleep(2);

    /* ── Resolve symbols ────────────────────────────────────────────── */

    AddExtensionFn add_ext =
        resolve_symbol("AddExtension", CYXV_ADDEXT_RVA);
    if (!add_ext) {
        fprintf(stderr, "[CyXV] Cannot find AddExtension — giving up\n");
        return NULL;
    }

    WriteToClientFn wtc =
        resolve_symbol("WriteToClient", CYXV_WRITETOCLIENT_RVA);
    if (wtc)
        cyxv_set_write_fn(wtc);
    else
        fprintf(stderr, "[CyXV] WARNING: WriteToClient not found — replies won't work\n");

    /* ── Open Display for rendering ─────────────────────────────────── */

    Display *dpy = XOpenDisplay(":0");
    if (dpy)
        cyxv_set_display(dpy, DefaultScreen(dpy));
    else
        fprintf(stderr, "[CyXV] WARNING: XOpenDisplay(:0) failed\n");

    /* ── Register XVideo extension ──────────────────────────────────── */

    void *entry = add_ext(
        "XVideo",
        0,                      /* NumEvents  */
        0,                      /* NumErrors  */
        cyxv_dispatch,
        cyxv_dispatch_swapped,
        NULL,                   /* CloseDownProc */
        cyxv_minor_opcode
    );

    if (entry)
        fprintf(stderr, "[CyXV] XVideo registered (entry=%p). "
                "Test: DISPLAY=:0 xdpyinfo | grep XVideo\n", entry);
    else
        fprintf(stderr, "[CyXV] AddExtension returned NULL "
                "(already registered, or server not ready yet)\n");

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
