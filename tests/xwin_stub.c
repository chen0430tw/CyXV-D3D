/*
 * xwin_stub.c — Minimal XWin simulator for testing cyxv hook mechanism.
 *
 * Simulates what XWin.exe does:
 *   1. Loads as the main executable (LD_PRELOAD injects cyxv.dll)
 *   2. Exposes InitExtensions() and AddExtension() as global symbols
 *   3. Calls InitExtensions() from main() after a short delay
 *
 * The cyxv constructor installs a hook on InitExtensions; when we call it
 * here, the hook should fire, call the real InitExtensions, then register
 * the CyXV extension via AddExtension.
 *
 * Build: see tests/Makefile or run: make -C tests
 * Run:   LD_PRELOAD=../cyxv.dll ./xwin_stub
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

/* ── Fake ExtensionEntry ────────────────────────────────────────────── */

typedef struct {
    int   index;
    char *name;
    short base;        /* assigned opcode */
    short eventBase;
    short errorBase;
} ExtensionEntry;

/* Extension table */
#define MAX_EXTENSIONS 128
static ExtensionEntry  g_ext_table[MAX_EXTENSIONS];
static int             g_num_extensions = 0;

/* ── AddExtension — the symbol cyxv hook uses ───────────────────────── */

ExtensionEntry *AddExtension(
    const char *name,
    int NumEvents,
    int NumErrors,
    int  (*MainProc)(void *),
    int  (*SwappedProc)(void *),
    void (*CloseDown)(ExtensionEntry *),
    unsigned short (*MinorOpcode)(void *))
{
    (void)NumEvents; (void)NumErrors;
    (void)CloseDown; (void)MinorOpcode;

    if (g_num_extensions >= MAX_EXTENSIONS) {
        fprintf(stderr, "[stub] AddExtension: table full\n");
        return NULL;
    }
    ExtensionEntry *e = &g_ext_table[g_num_extensions++];
    e->index      = g_num_extensions - 1;
    e->name       = strdup(name);
    e->base       = (short)(100 + g_num_extensions); /* fake opcode */
    e->eventBase  = 0;
    e->errorBase  = 0;

    fprintf(stderr,
            "[stub] AddExtension(\"%s\") → opcode=%d"
            " MainProc=%p SwappedProc=%p\n",
            name, (int)(unsigned short)e->base,
            (void *)(uintptr_t)MainProc,
            (void *)(uintptr_t)SwappedProc);
    return e;
}

/* ── WriteToClient stub ─────────────────────────────────────────────── */

void WriteToClient(void *client, int len, const void *data) {
    (void)client; (void)len; (void)data;
    fprintf(stderr, "[stub] WriteToClient(len=%d)\n", len);
}

/* ── Built-in extensions registered by InitExtensions ─────────────── */

static void register_builtins(void) {
    const char *builtins[] = {
        "RANDR", "RENDER", "MIT-SHM", "XFIXES",
        "DAMAGE", "COMPOSITE", "RECORD", "GLX",
        NULL
    };
    for (int i = 0; builtins[i]; i++) {
        AddExtension(builtins[i], 0, 0, NULL, NULL, NULL, NULL);
    }
}

/* ── InitExtensions — the hook target ──────────────────────────────── */

void InitExtensions(void) {
    fprintf(stderr, "[stub] InitExtensions() — registering built-ins\n");
    register_builtins();
    fprintf(stderr, "[stub] InitExtensions() — done (%d built-ins)\n",
            g_num_extensions);
}

/* ── Dump registered extensions ─────────────────────────────────────── */

static void dump_extensions(void) {
    fprintf(stderr, "\n[stub] === Registered extensions (%d) ===\n",
            g_num_extensions);
    for (int i = 0; i < g_num_extensions; i++)
        fprintf(stderr, "[stub]   [%2d] opcode=%3d  \"%s\"\n",
                i, (int)(unsigned short)g_ext_table[i].base,
                g_ext_table[i].name ? g_ext_table[i].name : "(null)");
    fprintf(stderr, "[stub] =========================================\n\n");
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    fprintf(stderr,
            "[stub] XWin stub started (pid=%d)\n"
            "[stub] cyxv constructor should have run already.\n"
            "[stub] Sleeping 1s to simulate XWin startup…\n",
            (int)getpid());
    sleep(1);

    fprintf(stderr, "[stub] Calling InitExtensions()…\n");
    InitExtensions();   /* cyxv hook fires here if rva_init_extensions set */

    fprintf(stderr, "[stub] InitExtensions() returned.\n");
    sleep(3);           /* let cyxv init thread finish */
    dump_extensions();

    /* Check that XVideo (or CyXV-D3D) was registered */
    int found = 0;
    for (int i = 0; i < g_num_extensions; i++) {
        const char *n = g_ext_table[i].name;
        if (n && (strcmp(n, "XVideo") == 0 || strcmp(n, "CyXV-D3D") == 0)) {
            fprintf(stderr,
                    "[stub] PASS: \"%s\" extension registered (opcode=%d)\n",
                    n, (int)(unsigned short)g_ext_table[i].base);
            found = 1;
        }
    }
    if (!found) {
        fprintf(stderr, "[stub] FAIL: XVideo/CyXV-D3D not registered\n");
        return 1;
    }
    return 0;
}
