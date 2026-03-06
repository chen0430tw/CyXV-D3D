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
 *   1. dlsym(RTLD_DEFAULT, name)   — Cygwin POSIX linker
 *   2. GetProcAddress(exe, name)   — Windows PE export table (Cygwin only)
 *   3. /proc/self/maps base + RVA  — config-supplied per-build offset
 *
 * Loader Lock:
 *   On Cygwin/Windows, pthread_create() inside a DLL constructor (called
 *   while the OS Loader Lock is held) causes a deadlock: the new thread
 *   tries to acquire the Loader Lock for TLS init, but the calling
 *   constructor already holds it.
 *
 *   Fix: set rva_init_extensions in ~/.config/cyxv.conf.  The constructor
 *   then writes a 12-byte absolute JMP at InitExtensions() instead of
 *   spawning a thread.  When XWin calls InitExtensions() after releasing
 *   the Loader Lock, our hook restores the original bytes, calls the real
 *   InitExtensions(), then safely spawns the init thread.
 *
 *   How to find rva_init_extensions for your XWin build:
 *     1. Start XWin once with cyxv.dll; the log prints image_base.
 *     2. gdb -p $(pgrep XWin) -ex "p InitExtensions" -batch
 *          → $1 = {void (void)} 0x10041fab0 <InitExtensions>
 *     3. RVA = VA - image_base  (e.g. 0x10041fab0 - 0x100400000 = 0x1fab0)
 *     4. echo "rva_init_extensions = 1fab0" >> ~/.config/cyxv.conf
 *
 * Build:
 *   gcc -O2 -shared -fPIC -o cyxv.dll \
 *       cyxv_init.c cyxv_dispatch.c cyxv_config.c -lX11 -lpthread
 */

#define _GNU_SOURCE
#include "cyxv_config.h"
#include "cyxv_dispatch.h"
#include "cyxv_xvproto.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <X11/Xlib.h>

#ifdef __CYGWIN__
# undef Status        /* X11/Xlib.h defines Status as int; conflicts with w32api */
# undef ControlMask  /* X11/X.h defines ControlMask as (1<<2); conflicts with processthreadsapi.h */
# include <windows.h>
#endif

/* ── Stable log handle ────────────────────────────────────────────────────
 * XWin redirects fd 2 early in main().  cyxv_ctor() dup()s fd 2 before
 * that happens, so all [CyXV] output still reaches the terminal.
 */
FILE *g_log = NULL;

#define CYXV_LOG(fmt, ...) \
    do { if (g_log) { fprintf(g_log, fmt, ##__VA_ARGS__); fflush(g_log); } } while(0)

/* ── Global config (loaded in constructor, read by hook and thread) ─────── */
static CyxvConfig g_cfg;

/* ── Compile-time RVA fallbacks for dlsym-invisible symbols ─────────────
 * These are superseded by rva_* keys in the config file at runtime.
 * dixLookupResourceByType and ShmSegType are Cygwin-exported; no RVA needed.
 */
#define RVA_dixLookupResourceByType   0ULL
#define RVA_ShmSegType                0ULL

/* ── Xorg ExtensionEntry (fields we need) ────────────────────────────── */

typedef struct {
    int   index;
    char *name;
    short base;       /* assigned major opcode */
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
    void *sym = dlsym(RTLD_DEFAULT, name);
    if (sym) { CYXV_LOG("[CyXV] %-22s dlsym       → %p\n", name, sym); return sym; }

#ifdef __CYGWIN__
    sym = (void *)GetProcAddress(GetModuleHandle(NULL), name);
    if (sym) { CYXV_LOG("[CyXV] %-22s GetProcAddr → %p\n", name, sym); return sym; }
#endif

    uintptr_t base = xwin_image_base();
    if (base && rva) {
        sym = (void *)(base + rva);
        CYXV_LOG("[CyXV] %-22s base+RVA    → %p\n", name, sym);
        return sym;
    }

    CYXV_LOG("[CyXV] FATAL: cannot resolve %s\n", name);
    return NULL;
}

/* ── x86-64 / i386 function hook ─────────────────────────────────────────
 *
 * x86-64: 12-byte absolute JMP  (MOV RAX, imm64 ; JMP RAX)
 *         Works regardless of distance between XWin.exe and cyxv.dll.
 * i386:    5-byte relative  JMP  (E9 rel32)
 *         Works within ±2 GB.
 */
#if defined(__x86_64__)
# define HOOK_SIZE 12
#elif defined(__i386__)
# define HOOK_SIZE 5
#else
# define HOOK_SIZE 0
#endif

static uint8_t   g_orig_bytes[HOOK_SIZE > 0 ? HOOK_SIZE : 1];
static uintptr_t g_hook_site;

static int write_hook(uintptr_t site, void *target) {
#if HOOK_SIZE == 0
    (void)site; (void)target;
    return -1;  /* unsupported arch */
#else
    long pgsz = sysconf(_SC_PAGESIZE);
    uintptr_t page = site & ~(uintptr_t)(pgsz - 1);
    /* Cover two pages in case the hook straddles a boundary */
    if (mprotect((void *)page, (size_t)(pgsz * 2),
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        CYXV_LOG("[CyXV] write_hook: mprotect(%p): %s\n",
                 (void *)page, strerror(errno));
        return -1;
    }
    memcpy(g_orig_bytes, (void *)site, HOOK_SIZE);
    uint8_t *p = (uint8_t *)site;
# ifdef __x86_64__
    p[0] = 0x48; p[1] = 0xB8;                     /* MOV RAX, imm64 */
    *(uint64_t *)(p + 2) = (uint64_t)(uintptr_t)target;
    p[10] = 0xFF; p[11] = 0xE0;                    /* JMP RAX        */
# else /* __i386__ */
    p[0] = 0xE9;                                   /* JMP rel32      */
    *(int32_t *)(p + 1) = (int32_t)((uintptr_t)target - site - 5);
# endif
    __builtin___clear_cache((char *)site, (char *)(site + HOOK_SIZE));
    return 0;
#endif
}

static void remove_hook(void) {
    if (!g_hook_site) return;
    memcpy((void *)g_hook_site, g_orig_bytes, HOOK_SIZE);
    __builtin___clear_cache((char *)g_hook_site,
                            (char *)(g_hook_site + HOOK_SIZE));
    g_hook_site = 0;
}

/* ── Core init: resolve symbols and register the extension ───────────── */

static void do_init(void) {
    const char *extname = cyxv_extension_name(&g_cfg);
    CYXV_LOG("[CyXV] xvcompat=%s → registering as \"%s\"\n",
             g_cfg.xvcompat ? "true" : "false", extname);

    AddExtensionFn  add_ext = resolve("AddExtension",
                                      g_cfg.rva_add_extension);
    WriteToClientFn wtc     = resolve("WriteToClient",
                                      g_cfg.rva_write_to_client);

    if (!add_ext) {
        CYXV_LOG("[CyXV] Cannot find AddExtension — aborting\n");
        return;
    }

    if (wtc)  cyxv_set_write_fn(wtc);
    else      CYXV_LOG("[CyXV] WARNING: WriteToClient not found"
                       " — replies disabled\n");

    DixLookupFn    dix_fn   = resolve("dixLookupResourceByType",
                                      RVA_dixLookupResourceByType);
    unsigned long *shm_type = resolve("ShmSegType", RVA_ShmSegType);
    if (dix_fn && shm_type)
        cyxv_set_shm_lookup(dix_fn, shm_type);
    else
        CYXV_LOG("[CyXV] WARNING: SHM lookup unavailable "
                 "(dixLookup=%p ShmSegType=%p) — falling back to XID mask\n",
                 (void *)dix_fn, (void *)shm_type);

    Display *dpy = XOpenDisplay(g_cfg.display);
    if (dpy) {
        int scr = DefaultScreen(dpy);
        uint32_t vid =
            (uint32_t)XVisualIDFromVisual(DefaultVisual(dpy, scr));
        CYXV_LOG("[CyXV] XOpenDisplay(%s) ok"
                 " — screen=%d depth=%d visual=0x%x\n",
                 g_cfg.display, scr, DefaultDepth(dpy, scr), vid);
        cyxv_set_visual_id(vid);
        cyxv_set_display(dpy, scr);
        cyxv_start_render_thread();
    } else {
        CYXV_LOG("[CyXV] WARNING: XOpenDisplay(%s) failed"
                 " — rendering disabled\n", g_cfg.display);
    }

    ExtensionEntry *entry = add_ext(
        extname,
        0,                    /* NumEvents                              */
        0,                    /* NumErrors                              */
        cyxv_dispatch,
        cyxv_dispatch_swapped,
        NULL,                 /* CloseDownProc                          */
        cyxv_minor_opcode
    );

    if (entry) {
        CYXV_LOG("[CyXV] \"%s\" registered: opcode=%d entry=%p\n"
                 "[CyXV] Verify: DISPLAY=:0 xdpyinfo | grep %s\n",
                 extname, (int)(unsigned short)entry->base, (void *)entry,
                 g_cfg.xvcompat ? "XVideo" : "CyXV-D3D");
    } else {
        CYXV_LOG("[CyXV] AddExtension returned NULL\n"
                 "[CyXV] Possible causes: already registered, called too"
                 " early, or extension table full (max 128).\n");
    }
}

/* ── Thread entry: sleep then init (Linux / no-hook path) ────────────── */

static void *init_thread(void *arg) {
    (void)arg;
    /* Wait for XWin's built-in InitExtensions() to finish so AddExtension
     * is safe to call.  (Not needed on the hook path — see below.)     */
    sleep(2);
    do_init();
    return NULL;
}

/* ── Hook function: replaces InitExtensions() on the Cygwin path ────────
 *
 * Called by XWin instead of its own InitExtensions().  At this point the
 * Loader Lock has been released, so pthread_create() is safe.
 */
static void init_extensions_hook(void) {
    /* Restore original bytes so the real InitExtensions can execute */
    uintptr_t orig_site = g_hook_site;
    remove_hook();  /* clears g_hook_site */

    /* Call the real InitExtensions */
    ((void (*)(void))orig_site)();

    /* All built-in extensions are now registered; AddExtension is safe.
     * Spawn our init thread — no sleep() needed this time.             */
    CYXV_LOG("[CyXV] InitExtensions intercepted — spawning init thread\n");
    pthread_t tid;
    /* Use a simple wrapper that calls do_init() without sleep */
    extern void *cyxv_init_thread_no_sleep(void *);  /* forward decl below */
    pthread_create(&tid, NULL, cyxv_init_thread_no_sleep, NULL);
    pthread_detach(tid);
}

void *cyxv_init_thread_no_sleep(void *arg) {
    (void)arg;
    do_init();
    return NULL;
}

/* ── DLL constructor ─────────────────────────────────────────────────── */

__attribute__((constructor))
static void cyxv_ctor(void) {
    /* 1. Save stderr before XWin redirects fd 2 */
    int saved_fd = dup(2);
    if (saved_fd >= 0) {
        fcntl(saved_fd, F_SETFD, FD_CLOEXEC);
        g_log = fdopen(saved_fd, "w");
    }
    if (!g_log) g_log = stderr;

    /* 2. Load config (needs g_log already set for config path messages) */
    cyxv_config_load(&g_cfg);

    /* 3. XInitThreads must precede any Xlib call */
    int xth = XInitThreads();

    /* 4. Print image base to help user calibrate RVAs */
    uintptr_t base = xwin_image_base();
    CYXV_LOG("[CyXV] libcyxv loaded — XInitThreads()=%d"
             " — image_base=0x%lx\n", xth, (unsigned long)base);

    /* 5. If rva_init_extensions is configured, hook InitExtensions() to
     *    avoid the Cygwin Loader Lock deadlock.
     *    pthread_create() inside a DLL constructor holds the Loader Lock;
     *    the new thread then tries to acquire it for TLS init → deadlock.
     *    The hook fires AFTER the OS releases the lock.                  */
    if (g_cfg.rva_init_extensions && base) {
        g_hook_site = base + g_cfg.rva_init_extensions;
        CYXV_LOG("[CyXV] hooking InitExtensions at %p (RVA 0x%llx)\n",
                 (void *)g_hook_site,
                 (unsigned long long)g_cfg.rva_init_extensions);
        if (write_hook(g_hook_site, init_extensions_hook) == 0) {
            CYXV_LOG("[CyXV] hook installed — thread deferred\n");
            return;   /* thread will be created from inside the hook */
        }
        CYXV_LOG("[CyXV] hook write failed — falling back to thread\n");
        g_hook_site = 0;
    }

    /* 6. Fallback: thread with sleep(2).
     *    Safe on Linux (no Loader Lock).  On Cygwin without rva_init_extensions
     *    configured this may deadlock — set the RVA to fix it.          */
    CYXV_LOG("[CyXV] spawning init thread (sleep-2 path)\n");
    pthread_t tid;
    pthread_create(&tid, NULL, init_thread, NULL);
    pthread_detach(tid);
}
