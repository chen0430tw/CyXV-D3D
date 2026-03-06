#ifndef CYXV_DISPATCH_H
#define CYXV_DISPATCH_H

#include <X11/Xlib.h>

/* Set by cyxv_init.c after opening Display */
void cyxv_set_display(Display *dpy, int screen);

/* WriteToClient function pointer resolved from XWin.exe */
typedef void (*WriteToClientFn)(void *client, int len, const void *data);
void cyxv_set_write_fn(WriteToClientFn fn);

/* Start the render thread (call once, after set_display) */
void cyxv_start_render_thread(void);

/* Registered with AddExtension */
int            cyxv_dispatch(void *client);
int            cyxv_dispatch_swapped(void *client);
unsigned short cyxv_minor_opcode(void *client);

#endif
