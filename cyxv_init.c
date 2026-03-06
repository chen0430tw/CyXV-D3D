/*
 * cyxv_init.c — DLL constructor: find AddExtension, register XV extension
 *
 * Build as a shared library:
 *   gcc -O2 -shared -fPIC -o cyxv.dll cyxv_init.c cyxv_dispatch.c \
 *       -lX11 -lpthread -lXext
 *
 * Inject into XWin.exe at startup via LD_PRELOAD:
 *   LD_PRELOAD=/path/to/cyxv.dll XWin :0 -multiwindow -listen tcp &
 *
 * What this does:
 *   1. The __attribute__((constructor)) fires when the DLL loads.
 *   2. It spawns a delayed-init thread so we don't race with XWin startup.
 *   3. The thread waits until AddExtension is callable (XWin's extension
 *      table is initialized), then calls it to register "XVideo".
 *   4. It resolves WriteToClient so we can send replies to XV clients.
 *   5. It opens a private Display connection for rendering.
 *
 * Finding AddExtension:
 *   XWin.exe compiled by GCC on Cygwin exports its global symbols via
 *   the Cygwin PE loader.  We use dlopen(NULL) + dlsym("AddExtension")
 *   (which on Cygwin resolves against the main executable's symbol table)
 *   to find the function at runtime — no hardcoded address needed.
 *
 *   If dlsym fails (non-exported symbol), fall back to the address found
 *   via GDB: CYXV_ADDEXT_FALLBACK_ADDR (update per your XWin build).
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
#include <X11/Xlib.h>

/* ── Fallback hardcoded address (update from GDB when needed) ─────────────
 * From: (gdb) info address AddExtension
 *   Symbol "AddExtension" is at 0x10048a4e0
 *
 * This is the ASLR-disabled base address; XWin.exe on Cygwin typically
 * loads at 0x100000000 (4 GB), so offset = 0x10048a4e0 - 0x100000000 = 0x48a4e0
 */
#define CYXV_ADDEXT_FALLBACK_ADDR  ((void *)0x10048a4e0ULL)

/* ── AddExtension signature ────────────────────────────────────────────── */

typedef void * (*AddExtensionFn)(
    const char *name,
    int NumEvents,
    int NumErrors,
    int  (*MainProc)(void *client),
    int  (*SwappedMainProc)(void *client),
    void (*CloseDownProc)(void *ext),
    unsigned short (*MinorOpcodeProc)(void *client)
);

/* ── WriteToClient signature ──────────────────────────────────────────── */

typedef void (*WriteToClientFn)(void *client, int len, const void *data);

/* ── Delayed init thread ────────────────────────────────────────────────── */

static void *init_thread(void *arg) {
    (void)arg;

    /* Give XWin.exe time to initialize its extension table.
     * XWin prints "Initializing built-in extension ..." lines during
     * startup; AddExtension is safe to call after InitExtensions().
     * 2 seconds is conservative; 500 ms usually works.                    */
    sleep(2);

    /* ── 1. Resolve AddExtension ──────────────────────────────────────── */
    void *exe = dlopen(NULL, RTLD_NOW | RTLD_GLOBAL);
    AddExtensionFn add_ext = NULL;

    if (exe) {
        add_ext = (AddExtensionFn)dlsym(exe, "AddExtension");
        if (!add_ext)
            fprintf(stderr, "[CyXV] dlsym(AddExtension) failed: %s\n", dlerror());
    }

    if (!add_ext) {
        fprintf(stderr, "[CyXV] Falling back to hardcoded address %p\n",
                CYXV_ADDEXT_FALLBACK_ADDR);
        add_ext = (AddExtensionFn)CYXV_ADDEXT_FALLBACK_ADDR;
    }

    /* ── 2. Resolve WriteToClient ─────────────────────────────────────── */
    WriteToClientFn wtc = NULL;
    if (exe) {
        wtc = (WriteToClientFn)dlsym(exe, "WriteToClient");
        if (!wtc)
            fprintf(stderr, "[CyXV] dlsym(WriteToClient) failed: %s\n", dlerror());
    }
    if (wtc)
        cyxv_set_write_fn(wtc);
    else
        fprintf(stderr, "[CyXV] WARNING: WriteToClient not found — replies disabled\n");

    if (exe) dlclose(exe);

    /* ── 3. Open private Display for rendering ────────────────────────── */
    Display *dpy = XOpenDisplay(":0");
    if (dpy) {
        cyxv_set_display(dpy, DefaultScreen(dpy));
        fprintf(stderr, "[CyXV] Display :0 opened (screen %d)\n",
                DefaultScreen(dpy));
    } else {
        fprintf(stderr, "[CyXV] WARNING: XOpenDisplay(:0) failed — rendering disabled\n");
    }

    /* ── 4. Register the XVideo extension ────────────────────────────── */
    void *entry = add_ext(
        "XVideo",
        0,              /* NumEvents  — no XV events in this MVP */
        0,              /* NumErrors */
        cyxv_dispatch,
        cyxv_dispatch_swapped,
        NULL,           /* CloseDownProc — nothing to free on reset */
        cyxv_minor_opcode
    );

    if (entry) {
        fprintf(stderr, "[CyXV] XVideo extension registered (ExtensionEntry=%p)\n",
                entry);
        fprintf(stderr, "[CyXV] Run: DISPLAY=:0 xdpyinfo | grep XVideo\n");
    } else {
        fprintf(stderr, "[CyXV] AddExtension returned NULL — already registered "
                "or server not ready\n");
    }

    return NULL;
}

/* ── DLL constructor ────────────────────────────────────────────────────── */

__attribute__((constructor))
static void cyxv_ctor(void) {
    fprintf(stderr, "[CyXV] libcyxv loaded — starting init thread\n");
    pthread_t tid;
    pthread_create(&tid, NULL, init_thread, NULL);
    pthread_detach(tid);
}
