#include <X11/X.h>
#include <X11/Xproto.h>
#include <stdio.h>
#include <dlfcn.h>

/* * Xserver Extension Entry Point Structure 
 * Based on X.Org Server source: xserver/include/extnsionst.h
 */
typedef void (*AddExtensionProc)(
    const char *name, 
    int events, 
    int errors,
    int (*proc)(void*), 
    int (*swapped_proc)(void*),
    void (*reset_proc)(void*), 
    int (*minor_opcode_proc)(void*)
);

/* Fake Dispatcher for XVideo requests */
static int 
FakeXvDispatch(void *client) 
{
    /* * Future: Add D3D/GDI backend forwarding here.
     * Currently returns Success to prevent client crash.
     */
    return 0; 
}

/* * Constructor: Triggered when the library is loaded via LD_PRELOAD.
 */
void __attribute__((constructor)) 
cyxv_extension_init(void) 
{
    void *handle;
    AddExtensionProc add_ext;

    fprintf(stderr, "CyXV-Status: Starting injection into XWin...\n");

    /* Get handle of the current process (XWin.exe) */
    handle = dlopen(NULL, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "CyXV-Error: dlopen failed: %s\n", dlerror());
        return;
    }

    /* * Locate 'AddExtension' symbol. 
     * In X.Org server, this is the core function to register extensions.
     */
    add_ext = (AddExtensionProc)dlsym(handle, "AddExtension");
    
    if (add_ext) {
        /* * Force register "XVideo" extension name.
         * Events/Errors set to 0 for initial naming test.
         */
        add_ext("XVideo", 0, 0, FakeXvDispatch, FakeXvDispatch, NULL, NULL);
        fprintf(stderr, "CyXV-Status: XVideo extension successfully registered in memory.\n");
    } else {
        fprintf(stderr, "CyXV-Error: Could not find AddExtension symbol. Symbol may be stripped.\n");
    }

    dlclose(handle);
}
