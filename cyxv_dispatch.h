#ifndef CYXV_DISPATCH_H
#define CYXV_DISPATCH_H

#include <X11/Xlib.h>

/* Called by cyxv_init.c after XOpenDisplay */
void cyxv_set_display(Display *dpy, int screen);

/* Registered as WriteToClient trampoline */
typedef void (*WriteToClientFn)(void *client, int len, const void *data);
void cyxv_set_write_fn(WriteToClientFn fn);

/* The three functions passed to AddExtension */
int           cyxv_dispatch(void *client);
int           cyxv_dispatch_swapped(void *client);
unsigned short cyxv_minor_opcode(void *client);

#endif
