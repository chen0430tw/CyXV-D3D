/*
 * libcyxv.c — CyXV-D3D Ghost Renderer (XWin.exe injection shim)
 *
 * Runs as a background thread inside XWin.exe.
 * Listens on TCP :9999 for incoming BGRA frames and renders them
 * via XPutImage to a specified X11 Window ID.
 *
 * Protocol (per connection, persistent):
 *   Client sends frames in a loop:
 *     [Window XID : uint32_t][width : uint32_t][height : uint32_t]
 *     [pixels : width * height * 4 bytes  BGRA]
 *   Send header[3] == 0 to signal end of stream.
 *
 * FakeXvDispatch() is registered with XWin's AddExtension as the
 * XVideo major-opcode handler. On first call it ignites the ghost
 * server thread; subsequent calls return Success immediately so the
 * X server dispatch loop never blocks.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int engine_started = 0;

/* recv_all: keep reading until 'size' bytes are filled or connection drops */
static int recv_all(int sock, unsigned char *buf, size_t size) {
    size_t total = 0;
    while (total < size) {
        ssize_t n = recv(sock, buf + total, size - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

/* handle_client: serve one persistent connection until it closes */
static void handle_client(int sock, Display *d, int screen) {
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    unsigned char *pixels = NULL;
    size_t pixels_cap = 0;

    while (1) {
        uint32_t header[3];
        if (recv_all(sock, (unsigned char *)header, 12) < 0)
            break;

        uint32_t win = header[0];
        uint32_t w   = header[1];
        uint32_t h   = header[2];

        /* sentinel: client signals end of stream */
        if (win == 0 && w == 0 && h == 0)
            break;

        if (w == 0 || h == 0 || w > 8192 || h > 8192)
            break;

        size_t sz = (size_t)w * h * 4;

        /* grow pixel buffer only when necessary */
        if (sz > pixels_cap) {
            free(pixels);
            pixels = malloc(sz);
            if (!pixels) break;
            pixels_cap = sz;
        }

        if (recv_all(sock, pixels, sz) < 0)
            break;

        GC gc = XCreateGC(d, (Window)win, 0, NULL);
        XImage *img = XCreateImage(
            d,
            DefaultVisual(d, screen),
            DefaultDepth(d, screen),
            ZPixmap, 0,
            (char *)pixels,   /* XCreateImage takes ownership only if we let it */
            w, h, 32, 0
        );
        if (img) {
            XPutImage(d, (Window)win, gc, img, 0, 0, 0, 0, w, h);
            XFlush(d);
            img->data = NULL;   /* reclaim pointer before XDestroyImage frees it */
            XDestroyImage(img);
        }
        XFreeGC(d, gc);
    }

    free(pixels);
    close(sock);
}

/* phoenix_server: background accept loop, runs for the lifetime of XWin */
static void *phoenix_server(void *arg) {
    (void)arg;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return NULL;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port        = htons(9999);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(server_fd);
        return NULL;
    }
    listen(server_fd, 5);

    /* Open a fresh client connection to the running X server.
     * This is safe here because we are in a detached thread —
     * not on the X server's dispatch thread. */
    Display *d = XOpenDisplay(":0");
    if (!d) {
        close(server_fd);
        return NULL;
    }
    int screen = DefaultScreen(d);

    fprintf(stderr, "[CyXV] Ghost server listening on :9999 (screen=%d)\n", screen);

    while (1) {
        int sock = accept(server_fd, NULL, NULL);
        if (sock < 0) continue;
        /* Each client handled synchronously in this thread.
         * For multiple simultaneous senders, spawn per-client threads here. */
        handle_client(sock, d, screen);
    }

    /* never reached */
    XCloseDisplay(d);
    close(server_fd);
    return NULL;
}

/*
 * FakeXvDispatch — registered as the XVideo major-opcode handler via
 * AddExtension("XVideo", 0, 0, FakeXvDispatch, FakeXvDispatch, NULL, ...)
 *
 * The real ClientPtr type is defined in <dix/client.h> inside the Xorg
 * server SDK; casting to void* avoids the dependency when compiling
 * outside the server tree.
 */
int FakeXvDispatch(void *client) {
    (void)client;
    if (!engine_started) {
        engine_started = 1;
        pthread_t tid;
        pthread_create(&tid, NULL, phoenix_server, NULL);
        pthread_detach(tid);
    }
    return 0;   /* Success — never block the server dispatch loop */
}
