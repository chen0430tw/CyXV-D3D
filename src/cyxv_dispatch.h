#ifndef CYXV_DISPATCH_H
#define CYXV_DISPATCH_H

#include <stdint.h>
#include <X11/Xlib.h>

/* Set by cyxv_init.c after opening Display */
void cyxv_set_display(Display *dpy, int screen);

/* Cache the default visual ID so h_QueryAdaptors never needs to call
 * XVisualIDFromVisual() from the dispatch (server main) thread.
 * Must be called from init_thread after XOpenDisplay succeeds.       */
void cyxv_set_visual_id(uint32_t visual_id);

/* WriteToClient function pointer resolved from XWin.exe */
typedef void (*WriteToClientFn)(void *client, int len, const void *data);
void cyxv_set_write_fn(WriteToClientFn fn);

/*
 * MIT-SHM segment XID → kernel shmid resolution.
 *
 * XvShmPutImage carries the shmseg XID, not the kernel shmid.
 * The kernel shmid was registered by the client via MIT-SHM Attach and
 * stored in XWin's resource table as a ShmDescRec.
 *
 * cyxv_init.c resolves dixLookupResourceByType (server API) and the
 * ShmSegType resource-type variable, then calls this to wire them in.
 *
 * DixLookupFn signature (matches xserver dix.h):
 *   int dixLookupResourceByType(void **value, XID id, RESTYPE rtype,
 *                               ClientPtr client, Mask access);
 * ShmDesc layout (xserver/Xext/shmint.h, x86_64):
 *   offset  0: next ptr (8 bytes)
 *   offset  8: shmid    (4 bytes)  ← we read this
 *   offset 12: refcnt   (4 bytes)
 *   offset 16: addr ptr (8 bytes)
 *   ...
 */
typedef int (*DixLookupFn)(void **value, unsigned long id,
                            unsigned long rtype, void *client,
                            unsigned int access);
void cyxv_set_shm_lookup(DixLookupFn fn, unsigned long *seg_type_ptr);

/* Start the render thread (call once, after set_display) */
void cyxv_start_render_thread(void);

/* Registered with AddExtension */
int            cyxv_dispatch(void *client);
int            cyxv_dispatch_swapped(void *client);
unsigned short cyxv_minor_opcode(void *client);

#endif
