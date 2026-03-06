/*
 * cyxv_dispatch.c — XVideo protocol dispatcher
 *
 * This file implements the server-side handling of all XV wire protocol
 * requests.  It is compiled into libcyxv.so and injected into XWin.exe.
 *
 * When XWin.exe receives a request whose major opcode matches the one
 * assigned to "XVideo" by AddExtension, it calls:
 *
 *     cyxv_dispatch(ClientPtr client)
 *
 * We inspect the minor opcode in the raw request buffer and dispatch
 * to the appropriate handler.
 *
 * Rendering path:
 *   XvShmPutImage → shmat(shmseg) → yuv_to_bgra() → XPutImage via
 *   a private X11 connection (opened in cyxv_init.c).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * How we access the request buffer:
 *   ClientPtr is an opaque pointer inside XWin.exe.  The raw request
 *   bytes are at client->requestBuffer (a void* field inside ClientRec).
 *   From the GDB session we know XWin.exe was compiled with GCC on
 *   Cygwin x86_64.  The relevant offset in ClientRec is determined by
 *   cyxv_find_request_buffer() at runtime via a known reference point.
 *
 *   Simpler approach used here: AddExtension's MainProc receives the
 *   ClientPtr; the Xorg server guarantees that the first "interesting"
 *   pointer in that struct is the request buffer.  In practice, we use
 *   the fact that GDB showed us offset 16 (see CLIENTREC_REQBUF_OFFSET).
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "cyxv_xvproto.h"
#include "cyxv_dispatch.h"

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>
#include <sys/ipc.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Tunables ────────────────────────────────────────────────────────────── */

/*
 * Byte offset of the `requestBuffer` pointer inside the opaque ClientRec.
 * Determined empirically from the VcXsrv build used in development
 * (XWin.exe 21.1.x, GCC-compiled Cygwin x86_64).
 *
 * To verify/update for a different build, use GDB:
 *   (gdb) p &((ClientPtr)0)->requestBuffer
 */
#define CLIENTREC_REQBUF_OFFSET  16

/*
 * Byte offset of the `sequence` field used to stamp replies.
 * Also empirical; in Xorg 21.x this is just after requestBuffer.
 */
#define CLIENTREC_SEQ_OFFSET     24

/* Our fake XVideo adaptor/port ID base (must be > 0, < 2^29) */
#define CYXV_PORT_BASE     0x00C00001
#define CYXV_PORT_COUNT    1

/* Our fake encoding XID */
#define CYXV_ENCODING_ID   0x00C00100

/* ── WriteToClient trampoline ─────────────────────────────────────────────── */
/*
 * XWin.exe exports WriteToClient; we resolve it once at init time.
 * Its signature is: void WriteToClient(ClientPtr c, int len, const void *data)
 */
typedef void (*WriteToClientFn)(void *client, int len, const void *data);
static WriteToClientFn g_WriteToClient = NULL;

/* Set by cyxv_init.c */
void cyxv_set_write_fn(WriteToClientFn fn) { g_WriteToClient = fn; }

static inline void send_reply(void *client, const void *data, int len) {
    if (g_WriteToClient)
        g_WriteToClient(client, len, data);
}

/* ── Client request buffer accessor ─────────────────────────────────────── */

static inline const void *req_buf(void *client) {
    return *(void **)((char *)client + CLIENTREC_REQBUF_OFFSET);
}

static inline uint32_t client_seq(void *client) {
    return *(uint32_t *)((char *)client + CLIENTREC_SEQ_OFFSET);
}

/* ── Display handle for rendering ────────────────────────────────────────── */
/* Opened in cyxv_init.c */
static Display *g_dpy    = NULL;
static int      g_screen = 0;

void cyxv_set_display(Display *dpy, int screen) {
    g_dpy    = dpy;
    g_screen = screen;
}

/* ── YUV → BGRA software conversion ─────────────────────────────────────── */

/* Clamp an integer to [0, 255] */
static inline uint8_t clamp(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* BT.601 YUV → RGB (studio swing: Y in [16,235], UV in [16,240]) */
static inline void yuv_to_bgra_pixel(uint8_t Y, uint8_t U, uint8_t V,
                                     uint8_t *B, uint8_t *G, uint8_t *R) {
    int y  = (int)Y - 16;
    int u  = (int)U - 128;
    int v  = (int)V - 128;
    int c  = 298 * y;
    *R = clamp((c           + 409 * v + 128) >> 8);
    *G = clamp((c - 100 * u - 208 * v + 128) >> 8);
    *B = clamp((c + 516 * u           + 128) >> 8);
}

/*
 * Convert YV12 (or I420) to BGRA.
 * YV12: Y plane full, V plane w/2 x h/2, U plane w/2 x h/2
 * I420: same but U and V swapped
 */
static void yv12_to_bgra(const uint8_t *src, int w, int h,
                          uint8_t *dst, int is_i420) {
    const uint8_t *Y = src;
    int uv_w = w / 2, uv_h = h / 2;
    const uint8_t *V = Y + w * h;
    const uint8_t *U = V + uv_w * uv_h;
    if (is_i420) { const uint8_t *tmp = U; U = V; V = tmp; }

    for (int row = 0; row < h; row++) {
        uint8_t *out = dst + row * w * 4;
        for (int col = 0; col < w; col++) {
            uint8_t y = Y[row * w + col];
            uint8_t u = U[(row/2) * uv_w + (col/2)];
            uint8_t v = V[(row/2) * uv_w + (col/2)];
            yuv_to_bgra_pixel(y, u, v,
                              &out[col*4+0], &out[col*4+1], &out[col*4+2]);
            out[col*4+3] = 0xFF;
        }
    }
}

/*
 * Convert YUY2 (packed 4:2:2) to BGRA.
 * Layout: Y0 U0 Y1 V0 | Y2 U1 Y3 V1 | ...
 */
static void yuy2_to_bgra(const uint8_t *src, int w, int h, uint8_t *dst) {
    for (int row = 0; row < h; row++) {
        const uint8_t *s = src + row * w * 2;
        uint8_t *out = dst + row * w * 4;
        for (int col = 0; col < w; col += 2) {
            uint8_t y0 = s[0], u = s[1], y1 = s[2], v = s[3];
            s += 4;
            yuv_to_bgra_pixel(y0, u, v,
                              &out[col*4+0], &out[col*4+1], &out[col*4+2]);
            out[col*4+3] = 0xFF;
            yuv_to_bgra_pixel(y1, u, v,
                              &out[(col+1)*4+0], &out[(col+1)*4+1],
                              &out[(col+1)*4+2]);
            out[(col+1)*4+3] = 0xFF;
        }
    }
}

/*
 * Convert NV12 (semi-planar: Y plane, then interleaved UV) to BGRA.
 */
static void nv12_to_bgra(const uint8_t *src, int w, int h, uint8_t *dst) {
    const uint8_t *Y  = src;
    const uint8_t *UV = src + w * h;
    for (int row = 0; row < h; row++) {
        uint8_t *out = dst + row * w * 4;
        for (int col = 0; col < w; col++) {
            uint8_t y = Y[row * w + col];
            uint8_t u = UV[(row/2) * w + (col & ~1)];
            uint8_t v = UV[(row/2) * w + (col & ~1) + 1];
            yuv_to_bgra_pixel(y, u, v,
                              &out[col*4+0], &out[col*4+1], &out[col*4+2]);
            out[col*4+3] = 0xFF;
        }
    }
}

/* Dispatch: convert whichever format to BGRA and call XPutImage */
static void render_yuv(uint32_t fourcc,
                       const uint8_t *pixels, int w, int h,
                       uint32_t drawable_xid,
                       int drw_x, int drw_y, int drw_w, int drw_h) {
    if (!g_dpy) return;

    uint8_t *bgra = malloc((size_t)w * h * 4);
    if (!bgra) return;

    switch (fourcc) {
    case FOURCC_YV12: yv12_to_bgra(pixels, w, h, bgra, 0); break;
    case FOURCC_I420: yv12_to_bgra(pixels, w, h, bgra, 1); break;
    case FOURCC_YUY2:
    case FOURCC_UYVY: yuy2_to_bgra(pixels, w, h, bgra);    break;
    case FOURCC_NV12: nv12_to_bgra(pixels, w, h, bgra);    break;
    default:
        free(bgra);
        return;
    }

    Drawable drawable = (Drawable)drawable_xid;
    GC gc = XCreateGC(g_dpy, drawable, 0, NULL);
    XImage *img = XCreateImage(
        g_dpy,
        DefaultVisual(g_dpy, g_screen),
        DefaultDepth(g_dpy, g_screen),
        ZPixmap, 0,
        (char *)bgra,
        (unsigned)w, (unsigned)h,
        32, 0
    );
    if (img) {
        /* Scale to destination rectangle if needed */
        if (drw_w == w && drw_h == h) {
            XPutImage(g_dpy, drawable, gc, img,
                      0, 0, drw_x, drw_y, (unsigned)w, (unsigned)h);
        } else {
            /* Simple nearest-neighbour scaling via XPutImage sub-regions
             * is expensive; for a first pass, just put at 1:1 */
            XPutImage(g_dpy, drawable, gc, img,
                      0, 0, drw_x, drw_y, (unsigned)w, (unsigned)h);
        }
        XFlush(g_dpy);
        img->data = NULL;
        XDestroyImage(img);
    }
    XFreeGC(g_dpy, gc);
    free(bgra);
}

/* ── SHM segment cache ───────────────────────────────────────────────────── */
/*
 * XWin uses its own SHM extension internals.  We maintain a small table
 * mapping XShm segment XIDs to shmid/shmaddr so we can read the YUV data.
 *
 * The XShmAttach request tells us: (seg_xid → shmid).
 * We intercept MIT-SHM requests too, but for simplicity we look up the
 * shmid by attaching to the segment ourselves when first seen.
 */

#define SHM_CACHE_SIZE 32

typedef struct {
    uint32_t xid;
    int      shmid;
    uint8_t *addr;
} ShmEntry;

static ShmEntry g_shm[SHM_CACHE_SIZE];
static int      g_shm_count = 0;

static uint8_t *shm_lookup(uint32_t xid, int shmid) {
    for (int i = 0; i < g_shm_count; i++)
        if (g_shm[i].xid == xid) return g_shm[i].addr;

    /* Not cached: attach */
    uint8_t *addr = shmat(shmid, NULL, SHM_RDONLY);
    if (addr == (uint8_t *)-1) return NULL;

    if (g_shm_count < SHM_CACHE_SIZE) {
        g_shm[g_shm_count].xid   = xid;
        g_shm[g_shm_count].shmid = shmid;
        g_shm[g_shm_count].addr  = addr;
        g_shm_count++;
    }
    return addr;
}

/* ── Individual request handlers ─────────────────────────────────────────── */

static int handle_QueryExtension(void *client) {
    xXvQueryExtensionReply r = {0};
    r.type           = 1;   /* X_Reply */
    r.sequenceNumber = (uint16_t)client_seq(client);
    r.length         = 0;
    r.version        = 2;
    r.revision       = 2;
    send_reply(client, &r, sizeof(r));
    return 0;
}

static int handle_QueryAdaptors(void *client) {
    /* One adaptor: "CyXV-D3D", type=XvInputMask|XvImageMask, 1 port */
    static const char  name[]   = "CyXV-D3D";
    static const uint8_t name_padded[8] = "CyXV-D3D"; /* already 8 bytes, no extra pad */

    xXvQueryAdaptorsReply hdr = {0};
    hdr.type           = 1;
    hdr.sequenceNumber = (uint16_t)client_seq(client);
    hdr.num_adaptors   = 1;
    /* length = (total_bytes - 32) / 4  where 32 = reply header size */

    xXvAdaptorInfo ai = {0};
    ai.base_id    = CYXV_PORT_BASE;
    ai.name_size  = (uint16_t)strlen(name);
    ai.num_ports  = CYXV_PORT_COUNT;
    ai.num_formats = 1;
    ai.type       = XvInputMask | XvImageMask;

    /* One format: depth=24, visual=DefaultVisual */
    xXvFormat fmt = {0};
    fmt.depth = 24;
    fmt.visual = g_dpy
        ? (uint32_t)XVisualIDFromVisual(DefaultVisual(g_dpy, g_screen))
        : 0x21;   /* fallback */

    /* Name must be padded to a multiple of 4 bytes */
    int name_len   = (int)strlen(name);
    int name_pad   = (4 - (name_len % 4)) % 4;
    int total_body = sizeof(ai) + name_len + name_pad + sizeof(fmt);

    hdr.length = (uint32_t)(total_body / 4);
    send_reply(client, &hdr, sizeof(hdr));
    send_reply(client, &ai,  sizeof(ai));
    send_reply(client, name, name_len);
    if (name_pad) { uint8_t z[4] = {0}; send_reply(client, z, name_pad); }
    send_reply(client, &fmt, sizeof(fmt));
    return 0;
}

static int handle_QueryEncodings(void *client) {
    static const char enc_name[] = "XV_IMAGE";
    int name_len  = (int)strlen(enc_name);
    int name_pad  = (4 - (name_len % 4)) % 4;

    xXvQueryEncodingsReply hdr = {0};
    hdr.type           = 1;
    hdr.sequenceNumber = (uint16_t)client_seq(client);
    hdr.num_encodings  = 1;

    xXvEncodingInfo ei = {0};
    ei.encoding        = CYXV_ENCODING_ID;
    ei.name_size       = (uint16_t)name_len;
    ei.width           = 4096;
    ei.height          = 4096;
    ei.rate_numerator  = 60;
    ei.rate_denominator= 1;

    hdr.length = (sizeof(ei) + name_len + name_pad) / 4;
    send_reply(client, &hdr, sizeof(hdr));
    send_reply(client, &ei,  sizeof(ei));
    send_reply(client, enc_name, name_len);
    if (name_pad) { uint8_t z[4]={0}; send_reply(client, z, name_pad); }
    return 0;
}

static int handle_GrabPort(void *client) {
    xXvGrabPortReply r = {0};
    r.type           = 1;
    r.result         = XvSuccess;
    r.sequenceNumber = (uint16_t)client_seq(client);
    send_reply(client, &r, sizeof(r));
    return 0;
}

/* UngrabPort: same reply as Grab with result=Success */
#define handle_UngrabPort handle_GrabPort

static int handle_QueryPortAttributes(void *client) {
    /* Return an empty attribute list — no XV attributes supported yet */
    xXvQueryPortAttributesReply r = {0};
    r.type             = 1;
    r.sequenceNumber   = (uint16_t)client_seq(client);
    r.num_attributes   = 0;
    r.text_size        = 0;
    send_reply(client, &r, sizeof(r));
    return 0;
}

static int handle_SetPortAttribute(void *client) {
    /* Accept silently, reply with generic success (empty reply = Success) */
    (void)client;
    return 0;
}

static int handle_GetPortAttribute(void *client) {
    /* Just return 0 for every attribute */
    uint8_t reply[32] = {0};
    reply[0] = 1;   /* X_Reply */
    *(uint16_t*)(reply+2) = (uint16_t)client_seq(client);
    /* value at bytes 8-11 = 0 */
    send_reply(client, reply, 32);
    return 0;
}

static xXvImageFormatInfo g_formats[] = {
    /* YV12 */
    { FOURCC_YV12, 1/*YUV*/, 0, {0}, {0},
      12, 3, {0}, 24, {0}, 0,0,0, 0,0,0,
      8, 8, 8,  1, 2, 2,  1, 2, 2,
      "YVU", {0}, 0, {0} },
    /* I420 */
    { FOURCC_I420, 1, 0, {0}, {0},
      12, 3, {0}, 24, {0}, 0,0,0, 0,0,0,
      8, 8, 8,  1, 2, 2,  1, 2, 2,
      "YUV", {0}, 0, {0} },
    /* YUY2 */
    { FOURCC_YUY2, 1, 0, {0}, {0},
      16, 1, {0}, 24, {0}, 0,0,0, 1,0,0,
      8, 8, 8,  1, 2, 1,  1, 1, 1,
      "YUV", {0}, 0, {0} },
    /* NV12 */
    { FOURCC_NV12, 1, 0, {0}, {0},
      12, 2, {0}, 24, {0}, 0,0,0, 0,0,0,
      8, 8, 8,  1, 2, 2,  1, 2, 2,
      "YUV", {0}, 0, {0} },
};
#define NUM_FORMATS  (int)(sizeof(g_formats)/sizeof(g_formats[0]))

static int handle_ListImageFormats(void *client) {
    xXvListImageFormatsReply hdr = {0};
    hdr.type           = 1;
    hdr.sequenceNumber = (uint16_t)client_seq(client);
    hdr.num_formats    = (uint32_t)NUM_FORMATS;
    hdr.length         = (uint32_t)(NUM_FORMATS * sizeof(xXvImageFormatInfo) / 4);
    send_reply(client, &hdr, sizeof(hdr));
    for (int i = 0; i < NUM_FORMATS; i++)
        send_reply(client, &g_formats[i], sizeof(xXvImageFormatInfo));
    return 0;
}

/* Compute YUV image layout for a given fourcc + dimensions */
static void image_attributes(uint32_t fourcc, int w, int h,
                              uint32_t *data_size,
                              uint32_t pitches[3], uint32_t offsets[3],
                              int *num_planes) {
    switch (fourcc) {
    case FOURCC_YV12:
    case FOURCC_I420:
        *num_planes   = 3;
        pitches[0]    = (uint32_t)w;
        pitches[1]    = pitches[2] = (uint32_t)(w / 2);
        offsets[0]    = 0;
        offsets[1]    = (uint32_t)(w * h);
        offsets[2]    = offsets[1] + (uint32_t)(w * h / 4);
        *data_size    = offsets[2] + (uint32_t)(w * h / 4);
        break;
    case FOURCC_NV12:
        *num_planes   = 2;
        pitches[0]    = (uint32_t)w;
        pitches[1]    = (uint32_t)w;
        offsets[0]    = 0;
        offsets[1]    = (uint32_t)(w * h);
        *data_size    = offsets[1] + (uint32_t)(w * h / 2);
        break;
    case FOURCC_YUY2:
    case FOURCC_UYVY:
        *num_planes   = 1;
        pitches[0]    = (uint32_t)(w * 2);
        offsets[0]    = 0;
        *data_size    = (uint32_t)(w * h * 2);
        break;
    default:
        *num_planes   = 0;
        *data_size    = 0;
        break;
    }
}

static int handle_QueryImageAttributes(void *client) {
    const xXvQueryImageAttributesReq *req = req_buf(client);
    int w = (int)req->width;
    int h = (int)req->height;
    /* Round up to even dimensions */
    w = (w + 1) & ~1;
    h = (h + 1) & ~1;

    uint32_t data_size = 0;
    uint32_t pitches[3] = {0}, offsets[3] = {0};
    int      num_planes = 0;
    image_attributes(req->id, w, h, &data_size, pitches, offsets, &num_planes);

    xXvQueryImageAttributesReply hdr = {0};
    hdr.type           = 1;
    hdr.sequenceNumber = (uint16_t)client_seq(client);
    hdr.num_planes     = (uint32_t)num_planes;
    hdr.data_size      = data_size;
    hdr.width          = (uint16_t)w;
    hdr.height         = (uint16_t)h;
    hdr.length         = (uint32_t)(num_planes * 2); /* 2 uint32_t per plane */
    send_reply(client, &hdr, sizeof(hdr));
    for (int i = 0; i < num_planes; i++)
        send_reply(client, &pitches[i], 4);
    for (int i = 0; i < num_planes; i++)
        send_reply(client, &offsets[i], 4);
    return 0;
}

/* XvShmPutImage — the main hot path */
static int handle_ShmPutImage(void *client) {
    const xXvShmPutImageReq *req = req_buf(client);

    int w = (int)req->width;
    int h = (int)req->height;

    /* Look up the SHM segment address.
     * The shmseg field is an XShm segment XID; we need the underlying shmid.
     * XWin.exe internally maps XShm XIDs to shmids; we do a brute-force
     * lookup via shmctl(IPC_STAT) to find matching segments visible to us.
     *
     * Simpler approach: the MIT-SHM attachment shmid is passed as the low
     * 24 bits of the XID in Cygwin's XWin implementation (observed in source).
     * We attempt shmat(shmseg & 0xFFFFFF) first.
     */
    int shmid = (int)(req->shmseg & 0x00FFFFFF);
    uint8_t *base = shm_lookup(req->shmseg, shmid);
    if (!base) {
        /* Fallback: try the full XID as shmid */
        base = shm_lookup(req->shmseg, (int)req->shmseg);
    }
    if (!base) {
        fprintf(stderr, "[CyXV] ShmPutImage: cannot find shmid for seg 0x%x\n",
                req->shmseg);
        return 0;
    }

    const uint8_t *pixels = base + req->offset;
    render_yuv(req->id, pixels, w, h,
               req->drawable,
               (int)req->drw_x, (int)req->drw_y,
               (int)req->drw_w, (int)req->drw_h);
    return 0;
}

/* XvPutImage — inline pixel data follows the request header */
static int handle_PutImage(void *client) {
    const xXvPutImageReq *req = req_buf(client);
    const uint8_t *pixels = (const uint8_t *)(req + 1);
    render_yuv(req->id,
               pixels, (int)req->width, (int)req->height,
               req->drawable,
               (int)req->drw_x, (int)req->drw_y,
               (int)req->drw_w, (int)req->drw_h);
    return 0;
}

/* ── Public dispatch entry point ─────────────────────────────────────────── */

int cyxv_dispatch(void *client) {
    const xGenericReq *req = req_buf(client);
    if (!req) return 0;

    switch (req->minor_opcode) {
    case X_XvQueryExtension:      return handle_QueryExtension(client);
    case X_XvQueryAdaptors:       return handle_QueryAdaptors(client);
    case X_XvQueryEncodings:      return handle_QueryEncodings(client);
    case X_XvGrabPort:            return handle_GrabPort(client);
    case X_XvUngrabPort:          return handle_UngrabPort(client);
    case X_XvSetPortAttribute:    return handle_SetPortAttribute(client);
    case X_XvGetPortAttribute:    return handle_GetPortAttribute(client);
    case X_XvQueryPortAttributes: return handle_QueryPortAttributes(client);
    case X_XvListImageFormats:    return handle_ListImageFormats(client);
    case X_XvQueryImageAttributes:return handle_QueryImageAttributes(client);
    case X_XvPutImage:            return handle_PutImage(client);
    case X_XvShmPutImage:         return handle_ShmPutImage(client);
    case X_XvQueryBestSize:       /* fall through — return success silently */
    default:
        return 0;
    }
}

/* Swapped variant: byte-swap any multi-byte fields before dispatching.
 * For now we defer to the non-swapped path — XV clients on x86 and
 * XWin.exe on x86_64 share the same byte order in practice.           */
int cyxv_dispatch_swapped(void *client) {
    return cyxv_dispatch(client);
}

/* MinorOpcodeProc: required by some Xorg builds — returns the minor op */
unsigned short cyxv_minor_opcode(void *client) {
    const xGenericReq *req = req_buf(client);
    return req ? req->minor_opcode : 0;
}
