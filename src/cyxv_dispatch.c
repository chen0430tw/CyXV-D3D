/*
 * cyxv_dispatch.c — XVideo protocol dispatcher
 *
 * CRITICAL ARCHITECTURE CONSTRAINT
 * ─────────────────────────────────
 * cyxv_dispatch() is called on the X server's single-threaded dispatch loop.
 * That loop is not re-entrant and cannot block.  Violations kill XWin.exe:
 *
 *   ✗ shmat() / shmdt()      — syscalls with unpredictable latency
 *   ✗ malloc() / free()      — heap locks can contend with server allocator
 *   ✗ XPutImage() or any     — X11 client call from inside the server =
 *     Xlib call                re-entrant socket write → instant deadlock
 *   ✗ YUV→RGB conversion     — CPU-bound, stalls the event loop
 *   ✗ any mutex that the     — obvious deadlock
 *     render thread holds
 *
 * Safe operations in dispatch():
 *   ✓ Read from client->requestBuffer   (already in our address space)
 *   ✓ memcpy of fixed-size metadata     (< ~128 bytes, no allocation)
 *   ✓ Non-blocking enqueue to lock-free ring buffer
 *   ✓ WriteToClient() for short replies (server does this everywhere)
 *
 * The render thread (started by cyxv_init.c) owns its own Display*,
 * does all shmat/shmdt/YUV/XPutImage, and never touches server internals.
 *
 *   dispatch thread                render thread
 *   ───────────────                ─────────────
 *   parse request header     →     ring_enqueue(frame_meta)
 *   send XV reply            ←     ring_dequeue → shmat → yuv2bgra → XPutImage
 */

#include "cyxv_xvproto.h"
#include "cyxv_dispatch.h"

#include <X11/Xlib.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <pthread.h>
#include <semaphore.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── ClientRec field offsets (VcXsrv GCC/Cygwin x86_64 build) ────────────
 * Verify with GDB: p &((ClientPtr)client)->requestBuffer
 */
#define CLIENTREC_REQBUF_OFFSET  16
#define CLIENTREC_SEQ_OFFSET     24

static inline const void *req_buf(void *client) {
    return *(void **)((char *)client + CLIENTREC_REQBUF_OFFSET);
}
static inline uint32_t client_seq(void *client) {
    return *(uint32_t *)((char *)client + CLIENTREC_SEQ_OFFSET);
}

/* ── WriteToClient trampoline (set by cyxv_init.c) ───────────────────── */

typedef void (*WriteToClientFn)(void *client, int len, const void *data);
static WriteToClientFn g_wtc = NULL;

void cyxv_set_write_fn(WriteToClientFn fn) { g_wtc = fn; }

/* ── MIT-SHM segment XID → kernel shmid resolution ───────────────────── */

static DixLookupFn   g_dix_lookup    = NULL;
static unsigned long *g_shm_seg_type = NULL;  /* pointer to ShmSegType global */

void cyxv_set_shm_lookup(DixLookupFn fn, unsigned long *seg_type_ptr) {
    g_dix_lookup    = fn;
    g_shm_seg_type  = seg_type_ptr;
    fprintf(stderr, "[CyXV] SHM lookup: dixLookupResourceByType=%p ShmSegType@%p=0x%lx\n",
            (void *)fn, (void *)seg_type_ptr,
            seg_type_ptr ? *seg_type_ptr : 0UL);
}

/*
 * Resolve an MIT-SHM segment XID to its kernel shmid.
 *
 * Primary path: call dixLookupResourceByType → get ShmDescRec pointer →
 *   read shmid at offset 8 (x86_64 layout, matches xserver/Xext/shmint.h).
 *
 * Fallback: on Cygwin/XWin the resource allocator historically uses the
 *   kernel shmid as the low 24 bits of the XID.  Unreliable but better
 *   than returning 0 if the primary path fails.
 */
static int shmseg_to_shmid(uint32_t xid, void *client) {
    if (g_dix_lookup && g_shm_seg_type) {
        void *desc = NULL;
        /* DixReadAccess = 1<<1 = 2 (dix.h) */
        int rc = g_dix_lookup(&desc, (unsigned long)xid,
                              *g_shm_seg_type, client, 2);
        if (rc == 0 && desc) {
            /* ShmDescRec x86_64:  next(8) + shmid(4) at byte offset 8 */
            int id = *(int *)((char *)desc + 8);
            if (id > 0) return id;
        }
        fprintf(stderr, "[CyXV] dixLookup shmseg 0x%x → rc=%d desc=%p\n",
                xid, rc, desc);
    }
    /* Fallback */
    int id = (int)(xid & 0x00FFFFFF);
    return id ? id : (int)xid;
}

static inline void send_reply(void *client, const void *data, int len) {
    if (g_wtc) g_wtc(client, len, data);
}

/* ── Frame-metadata ring buffer ──────────────────────────────────────────
 *
 * The dispatch thread writes FrameMeta entries; the render thread reads
 * them.  Ring is power-of-2 sized so index arithmetic wraps cheaply.
 * We use a lock-free single-producer / single-consumer design:
 *   - writer increments head after storing
 *   - reader increments tail after consuming
 * Both indices are atomic to avoid torn reads on x86_64.
 *
 * If the ring is full (render thread can't keep up), new frames are
 * silently dropped — we prefer losing frames over stalling dispatch.
 */

#define RING_SIZE  16   /* must be power of 2 */
#define RING_MASK  (RING_SIZE - 1)

typedef struct {
    uint32_t fourcc;
    uint32_t shmseg;     /* XV SHM segment XID  */
    int      shmid;      /* POSIX shmid for shmat */
    uint32_t shm_offset; /* byte offset into segment */
    uint16_t width, height;
    uint32_t drawable;
    int16_t  drw_x, drw_y;
    uint16_t drw_w, drw_h;
    /* For inline PutImage (non-SHM): pixel data copied here */
    uint8_t *inline_pixels; /* NULL for SHM frames */
    size_t   inline_size;
} FrameMeta;

typedef struct {
    FrameMeta slots[RING_SIZE];
    _Atomic uint32_t head;  /* written by dispatch thread */
    _Atomic uint32_t tail;  /* written by render thread   */
} FrameRing;

static FrameRing g_ring;
static sem_t     g_sem;   /* render thread sleeps on this */

static int ring_enqueue(const FrameMeta *m) {
    uint32_t head = __atomic_load_n(&g_ring.head, __ATOMIC_RELAXED);
    uint32_t tail = __atomic_load_n(&g_ring.tail, __ATOMIC_ACQUIRE);
    if (head - tail >= RING_SIZE)
        return -1;  /* full — drop frame */
    g_ring.slots[head & RING_MASK] = *m;
    __atomic_store_n(&g_ring.head, head + 1, __ATOMIC_RELEASE);
    sem_post(&g_sem);
    return 0;
}

static int ring_dequeue(FrameMeta *out) {
    uint32_t tail = __atomic_load_n(&g_ring.tail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&g_ring.head, __ATOMIC_ACQUIRE);
    if (head == tail) return -1;  /* empty */
    *out = g_ring.slots[tail & RING_MASK];
    __atomic_store_n(&g_ring.tail, tail + 1, __ATOMIC_RELEASE);
    return 0;
}

/* ── Display handle (set by cyxv_init.c) ─────────────────────────────── */

static Display *g_dpy    = NULL;
static int      g_screen = 0;

/* Visual ID cached at init time (init_thread, not dispatch thread).
 * Default 0x21 is the typical TrueColor visual on 24-bit displays;
 * cyxv_set_visual_id() overrides this once XOpenDisplay succeeds.    */
static uint32_t g_visual_id = 0x21;

void cyxv_set_display(Display *dpy, int screen) {
    g_dpy    = dpy;
    g_screen = screen;
}

void cyxv_set_visual_id(uint32_t visual_id) {
    g_visual_id = visual_id;
}

/* ── SHM segment address cache (render thread only) ──────────────────── */

#define SHM_CACHE 32
typedef struct { uint32_t xid; int shmid; uint8_t *addr; } ShmEntry;
static ShmEntry g_shm[SHM_CACHE];
static int      g_shm_n = 0;

static uint8_t *shm_attach(uint32_t xid, int shmid) {
    for (int i = 0; i < g_shm_n; i++)
        if (g_shm[i].xid == xid) return g_shm[i].addr;
    uint8_t *a = shmat(shmid, NULL, SHM_RDONLY);
    if (a == (uint8_t *)-1) return NULL;
    if (g_shm_n < SHM_CACHE) {
        g_shm[g_shm_n++] = (ShmEntry){ xid, shmid, a };
    }
    return a;
}

/* ── YUV → BGRA conversion (render thread only) ──────────────────────── */

static inline uint8_t clamp8(int v) {
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}
static inline void yuv_pixel(uint8_t Y, uint8_t U, uint8_t V,
                              uint8_t *B, uint8_t *G, uint8_t *R) {
    int c = 298 * ((int)Y - 16);
    int u = (int)U - 128, v = (int)V - 128;
    *R = clamp8((c           + 409*v + 128) >> 8);
    *G = clamp8((c - 100*u - 208*v + 128) >> 8);
    *B = clamp8((c + 516*u           + 128) >> 8);
}

static void yv12_to_bgra(const uint8_t *s, int w, int h,
                          uint8_t *dst, int i420) {
    const uint8_t *Y = s;
    const uint8_t *V = Y + w*h, *U = V + (w/2)*(h/2);
    if (i420) { const uint8_t *t = U; U = V; V = t; }
    for (int r = 0; r < h; r++) {
        uint8_t *o = dst + r * w * 4;
        for (int c = 0; c < w; c++, o += 4) {
            yuv_pixel(Y[r*w+c], U[(r/2)*(w/2)+(c/2)], V[(r/2)*(w/2)+(c/2)],
                      &o[0], &o[1], &o[2]);
            o[3] = 0xFF;
        }
    }
}
static void yuy2_to_bgra(const uint8_t *s, int w, int h, uint8_t *dst) {
    for (int r = 0; r < h; r++) {
        const uint8_t *p = s + r * w * 2;
        uint8_t *o = dst + r * w * 4;
        for (int c = 0; c < w; c += 2, p += 4, o += 8) {
            yuv_pixel(p[0], p[1], p[3], &o[0], &o[1], &o[2]); o[3] = 0xFF;
            yuv_pixel(p[2], p[1], p[3], &o[4], &o[5], &o[6]); o[7] = 0xFF;
        }
    }
}
static void nv12_to_bgra(const uint8_t *s, int w, int h, uint8_t *dst) {
    const uint8_t *Y = s, *UV = s + w*h;
    for (int r = 0; r < h; r++) {
        uint8_t *o = dst + r * w * 4;
        for (int c = 0; c < w; c++, o += 4) {
            yuv_pixel(Y[r*w+c], UV[(r/2)*w+(c&~1)], UV[(r/2)*w+(c&~1)+1],
                      &o[0], &o[1], &o[2]);
            o[3] = 0xFF;
        }
    }
}

static void render_frame(const FrameMeta *m, const uint8_t *pixels) {
    if (!g_dpy || !pixels) return;

    int w = m->width, h = m->height;
    uint8_t *bgra = malloc((size_t)w * h * 4);
    if (!bgra) return;

    switch (m->fourcc) {
    case FOURCC_YV12: yv12_to_bgra(pixels, w, h, bgra, 0); break;
    case FOURCC_I420: yv12_to_bgra(pixels, w, h, bgra, 1); break;
    case FOURCC_YUY2:
    case FOURCC_UYVY: yuy2_to_bgra(pixels, w, h, bgra);    break;
    case FOURCC_NV12: nv12_to_bgra(pixels, w, h, bgra);    break;
    default: free(bgra); return;
    }

    Drawable drw = (Drawable)m->drawable;
    GC gc = XCreateGC(g_dpy, drw, 0, NULL);
    XImage *img = XCreateImage(g_dpy,
                               DefaultVisual(g_dpy, g_screen),
                               (unsigned)DefaultDepth(g_dpy, g_screen),
                               ZPixmap, 0, (char *)bgra,
                               (unsigned)w, (unsigned)h, 32, 0);
    if (img) {
        XPutImage(g_dpy, drw, gc, img, 0, 0,
                  m->drw_x, m->drw_y, (unsigned)w, (unsigned)h);
        XFlush(g_dpy);
        img->data = NULL;   /* bgra freed separately below */
        XFree(img);         /* XDestroyImage macro unreliable on Cygwin; data already nulled */
    }
    XFreeGC(g_dpy, gc);
    free(bgra);
}

/* ── Render thread ───────────────────────────────────────────────────────
 *
 * Runs independently of the X server dispatch loop.
 * Owns all blocking operations: shmat, YUV conversion, XPutImage.
 */
static void *render_thread(void *arg) {
    (void)arg;
    for (;;) {
        sem_wait(&g_sem);   /* block until dispatch enqueues a frame */

        FrameMeta m;
        if (ring_dequeue(&m) < 0) continue;

        if (m.inline_pixels) {
            /* PutImage: caller malloc'd the pixel buffer for us */
            render_frame(&m, m.inline_pixels);
            free(m.inline_pixels);
        } else {
            /* ShmPutImage: attach SHM segment (cached after first attach) */
            uint8_t *base = shm_attach(m.shmseg, m.shmid);
            if (base) render_frame(&m, base + m.shm_offset);
            else      fprintf(stderr, "[CyXV] shm_attach failed for seg 0x%x\n",
                              m.shmseg);
        }
    }
    return NULL;
}

/* Called by cyxv_init.c after resolving symbols */
void cyxv_start_render_thread(void) {
    sem_init(&g_sem, 0, 0);
    pthread_t tid;
    pthread_create(&tid, NULL, render_thread, NULL);
    pthread_detach(tid);
    fprintf(stderr, "[CyXV] render thread started\n");
}

/* ── XV reply helpers (dispatch thread — no blocking allowed) ─────────── */

/*
 * g_formats[] — image formats advertised to XV clients.
 *
 * Fields use designated initializers so the mapping to xXvImageFormatInfo
 * is explicit and survives struct-layout changes.
 *
 * type:   0=RGB  1=YUV   (xv.xml ImageFormatInfoType)
 * format: 0=Packed  1=Planar
 * bpp:    bits per pixel for the luma plane (or packed pixel)
 * *_sample_bits: meaningful bits per sample per channel
 * horz/vert_*_period: subsampling periods (1=full, 2=half)
 * comp_order: channel ordering string (up to 32 chars)
 */
static const xXvImageFormatInfo g_formats[] = {
    /* YV12 — planar 4:2:0, Y+V+U */
    {
        .id            = FOURCC_YV12,
        .type          = 1,        /* YUV */
        .byte_order    = 0,        /* LSBFirst */
        .bpp           = 12,
        .num_planes    = 3,
        .depth         = 24,
        .format        = 1,        /* Planar */
        .y_sample_bits = 8, .u_sample_bits = 8, .v_sample_bits = 8,
        .horz_y_period = 1, .horz_u_period = 2, .horz_v_period = 2,
        .vert_y_period = 1, .vert_u_period = 2, .vert_v_period = 2,
        .comp_order    = "YVU",
    },
    /* I420 — planar 4:2:0, Y+U+V */
    {
        .id            = FOURCC_I420,
        .type          = 1,
        .byte_order    = 0,
        .bpp           = 12,
        .num_planes    = 3,
        .depth         = 24,
        .format        = 1,
        .y_sample_bits = 8, .u_sample_bits = 8, .v_sample_bits = 8,
        .horz_y_period = 1, .horz_u_period = 2, .horz_v_period = 2,
        .vert_y_period = 1, .vert_u_period = 2, .vert_v_period = 2,
        .comp_order    = "YUV",
    },
    /* YUY2 — packed 4:2:2, YUYV byte order */
    {
        .id            = FOURCC_YUY2,
        .type          = 1,
        .byte_order    = 0,
        .bpp           = 16,
        .num_planes    = 1,
        .depth         = 24,
        .format        = 0,        /* Packed */
        .y_sample_bits = 8, .u_sample_bits = 8, .v_sample_bits = 8,
        .horz_y_period = 1, .horz_u_period = 2, .horz_v_period = 2,
        .vert_y_period = 1, .vert_u_period = 1, .vert_v_period = 1,
        .comp_order    = "YUYV",
    },
    /* UYVY — packed 4:2:2, UYVY byte order */
    {
        .id            = FOURCC_UYVY,
        .type          = 1,
        .byte_order    = 0,
        .bpp           = 16,
        .num_planes    = 1,
        .depth         = 24,
        .format        = 0,
        .y_sample_bits = 8, .u_sample_bits = 8, .v_sample_bits = 8,
        .horz_y_period = 1, .horz_u_period = 2, .horz_v_period = 2,
        .vert_y_period = 1, .vert_u_period = 1, .vert_v_period = 1,
        .comp_order    = "UYVY",
    },
    /* NV12 — semi-planar 4:2:0, Y + interleaved UV */
    {
        .id            = FOURCC_NV12,
        .type          = 1,
        .byte_order    = 0,
        .bpp           = 12,
        .num_planes    = 2,
        .depth         = 24,
        .format        = 1,
        .y_sample_bits = 8, .u_sample_bits = 8, .v_sample_bits = 8,
        .horz_y_period = 1, .horz_u_period = 2, .horz_v_period = 2,
        .vert_y_period = 1, .vert_u_period = 2, .vert_v_period = 2,
        .comp_order    = "YUV",
    },
};
#define NUM_FMT (int)(sizeof(g_formats)/sizeof(g_formats[0]))

static void image_attrs(uint32_t cc, int w, int h,
                         uint32_t *sz, uint32_t p[3], uint32_t o[3], int *np) {
    switch (cc) {
    case FOURCC_YV12: case FOURCC_I420:
        *np=3; p[0]=(uint32_t)w; p[1]=p[2]=(uint32_t)(w/2);
        o[0]=0; o[1]=(uint32_t)(w*h); o[2]=o[1]+(uint32_t)(w*h/4);
        *sz=o[2]+(uint32_t)(w*h/4); break;
    case FOURCC_NV12:
        *np=2; p[0]=p[1]=(uint32_t)w;
        o[0]=0; o[1]=(uint32_t)(w*h); *sz=o[1]+(uint32_t)(w*h/2); break;
    case FOURCC_YUY2: case FOURCC_UYVY:
        *np=1; p[0]=(uint32_t)(w*2); o[0]=0; *sz=(uint32_t)(w*h*2); break;
    default: *np=0; *sz=0; break;
    }
}

/* ── Individual request handlers (dispatch thread — must be O(1)) ─────── */

static int h_QueryExtension(void *c) {
    xXvQueryExtensionReply r = {0};
    r.type=1; r.sequenceNumber=(uint16_t)client_seq(c);
    r.version=2; r.revision=2;
    send_reply(c, &r, sizeof(r));
    return 0;
}

static int h_QueryAdaptors(void *c) {
    static const char name[] = "CyXV-D3D";
    int nl = sizeof(name)-1, np = (4-(nl%4))%4;

    xXvQueryAdaptorsReply hdr = {0};
    hdr.type=1; hdr.sequenceNumber=(uint16_t)client_seq(c);
    hdr.num_adaptors=1;
    hdr.length = (sizeof(xXvAdaptorInfo)+nl+np+sizeof(xXvFormat)) / 4;
    send_reply(c, &hdr, sizeof(hdr));

    xXvAdaptorInfo ai = {0};
    ai.base_id=0x00C00001; ai.name_size=(uint16_t)nl;
    ai.num_ports=1; ai.num_formats=1;
    ai.type = XvInputMask | XvImageMask;
    send_reply(c, &ai, sizeof(ai));
    send_reply(c, name, nl);
    if (np) { uint8_t z[4]={0}; send_reply(c, z, np); }

    xXvFormat fmt = {0};
    fmt.depth=24;
    fmt.visual = g_visual_id;   /* cached by init_thread; never touched here */
    send_reply(c, &fmt, sizeof(fmt));
    return 0;
}

static int h_QueryEncodings(void *c) {
    static const char en[] = "XV_IMAGE";
    int nl=sizeof(en)-1, np=(4-(nl%4))%4;
    xXvQueryEncodingsReply hdr={0};
    hdr.type=1; hdr.sequenceNumber=(uint16_t)client_seq(c);
    hdr.num_encodings=1;
    hdr.length=(sizeof(xXvEncodingInfo)+nl+np)/4;
    send_reply(c,&hdr,sizeof(hdr));
    xXvEncodingInfo ei={0};
    ei.encoding=0x00C00100; ei.name_size=(uint16_t)nl;
    ei.width=4096; ei.height=4096;
    ei.rate_numerator=60; ei.rate_denominator=1;
    send_reply(c,&ei,sizeof(ei));
    send_reply(c,en,nl);
    if(np){uint8_t z[4]={0};send_reply(c,z,np);}
    return 0;
}

static int h_GrabPort(void *c) {
    xXvGrabPortReply r={0};
    r.type=1; r.result=XvSuccess;
    r.sequenceNumber=(uint16_t)client_seq(c);
    send_reply(c,&r,sizeof(r));
    return 0;
}

static int h_QueryPortAttributes(void *c) {
    xXvQueryPortAttributesReply r={0};
    r.type=1; r.sequenceNumber=(uint16_t)client_seq(c);
    send_reply(c,&r,sizeof(r));
    return 0;
}

static int h_GetPortAttribute(void *c) {
    uint8_t r[32]={0}; r[0]=1;
    *(uint16_t*)(r+2)=(uint16_t)client_seq(c);
    send_reply(c,r,32);
    return 0;
}

static int h_QueryBestSize(void *c) {
    const xXvQueryBestSizeReq *req = req_buf(c);
    /* Software renderer: any size is fine — echo back destination dimensions */
    xXvQueryBestSizeReply r = {0};
    r.type           = 1;
    r.sequenceNumber = (uint16_t)client_seq(c);
    r.actual_width   = req->drw_w;
    r.actual_height  = req->drw_h;
    send_reply(c, &r, sizeof(r));
    return 0;
}

static int h_ListImageFormats(void *c) {
    xXvListImageFormatsReply hdr={0};
    hdr.type=1; hdr.sequenceNumber=(uint16_t)client_seq(c);
    hdr.num_formats=(uint32_t)NUM_FMT;
    hdr.length=(uint32_t)(NUM_FMT*sizeof(xXvImageFormatInfo)/4);
    send_reply(c,&hdr,sizeof(hdr));
    for(int i=0;i<NUM_FMT;i++)
        send_reply(c,(const void *)&g_formats[i],sizeof(xXvImageFormatInfo));
    return 0;
}

static int h_QueryImageAttributes(void *c) {
    const xXvQueryImageAttributesReq *req = req_buf(c);
    int w = ((int)req->width  + 1) & ~1;
    int h = ((int)req->height + 1) & ~1;
    uint32_t sz=0, p[3]={0}, o[3]={0}; int np=0;
    image_attrs(req->id, w, h, &sz, p, o, &np);
    xXvQueryImageAttributesReply hdr={0};
    hdr.type=1; hdr.sequenceNumber=(uint16_t)client_seq(c);
    hdr.num_planes=(uint32_t)np; hdr.data_size=sz;
    hdr.width=(uint16_t)w; hdr.height=(uint16_t)h;
    hdr.length=(uint32_t)(np*2);
    send_reply(c,&hdr,sizeof(hdr));
    for(int i=0;i<np;i++) send_reply(c,&p[i],4);
    for(int i=0;i<np;i++) send_reply(c,&o[i],4);
    return 0;
}

/*
 * ShmPutImage dispatch handler.
 *
 * ONLY copies fixed-size metadata into the ring.  NO shmat, NO malloc,
 * NO blocking.  The render thread does all the real work.
 */
static int h_ShmPutImage(void *c) {
    const xXvShmPutImageReq *req = req_buf(c);

    /* Resolve shmseg XID → kernel shmid via XWin's resource table.
     * Falls back to low-24-bits heuristic if dixLookupResourceByType
     * was not resolved at init time.                                  */
    int shmid = shmseg_to_shmid(req->shmseg, c);

    FrameMeta m = {
        .fourcc     = req->id,
        .shmseg     = req->shmseg,
        .shmid      = shmid,
        .shm_offset = req->offset,
        .width      = req->width,
        .height     = req->height,
        .drawable   = req->drawable,
        .drw_x      = req->drw_x,
        .drw_y      = req->drw_y,
        .drw_w      = req->drw_w,
        .drw_h      = req->drw_h,
        .inline_pixels = NULL,
    };

    ring_enqueue(&m);   /* non-blocking; drops silently if ring full */
    return 0;
}

/*
 * PutImage dispatch handler (inline pixel data).
 *
 * Must copy the pixel data out of the request buffer before returning,
 * because the server reuses that buffer for the next request.
 * malloc here is the only allocation on the dispatch path; it is
 * unavoidable for inline frames.  ShmPutImage is the preferred hot path.
 */
static int h_PutImage(void *c) {
    const xXvPutImageReq *req = req_buf(c);

    uint32_t sz=0, p[3]={0}, o[3]={0}; int np=0;
    image_attrs(req->id, req->width, req->height, &sz, p, o, &np);
    if (!sz) return 0;

    uint8_t *buf = malloc(sz);
    if (!buf) return 0;
    memcpy(buf, req + 1, sz);

    FrameMeta m = {
        .fourcc        = req->id,
        .width         = req->width,
        .height        = req->height,
        .drawable      = req->drawable,
        .drw_x         = req->drw_x,
        .drw_y         = req->drw_y,
        .drw_w         = req->drw_w,
        .drw_h         = req->drw_h,
        .inline_pixels = buf,
        .inline_size   = sz,
    };

    if (ring_enqueue(&m) < 0) {
        /* Ring full — free the buffer we just allocated */
        free(buf);
    }
    return 0;
}

/* ── Public dispatch entry point ─────────────────────────────────────── */

int cyxv_dispatch(void *client) {
    const xGenericReq *req = req_buf(client);
    if (!req) return 0;
    switch (req->minor_opcode) {
    case X_XvQueryExtension:       return h_QueryExtension(client);
    case X_XvQueryAdaptors:        return h_QueryAdaptors(client);
    case X_XvQueryEncodings:       return h_QueryEncodings(client);
    case X_XvGrabPort:             return h_GrabPort(client);
    case X_XvUngrabPort:           return h_GrabPort(client);
    case X_XvQueryBestSize:        return h_QueryBestSize(client);
    case X_XvSetPortAttribute:     return 0;
    case X_XvGetPortAttribute:     return h_GetPortAttribute(client);
    case X_XvQueryPortAttributes:  return h_QueryPortAttributes(client);
    case X_XvListImageFormats:     return h_ListImageFormats(client);
    case X_XvQueryImageAttributes: return h_QueryImageAttributes(client);
    case X_XvPutImage:             return h_PutImage(client);
    case X_XvShmPutImage:          return h_ShmPutImage(client);
    default:                       return 0;
    }
}

int cyxv_dispatch_swapped(void *client) {
    return cyxv_dispatch(client);
}

unsigned short cyxv_minor_opcode(void *client) {
    const xGenericReq *req = req_buf(client);
    return req ? req->minor_opcode : 0;
}
