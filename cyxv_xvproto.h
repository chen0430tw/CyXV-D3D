/*
 * cyxv_xvproto.h — XVideo wire protocol definitions
 *
 * Field sizes and struct layouts validated against the canonical sources:
 *   /usr/include/X11/extensions/Xvproto.h  (sz_xvImageFormatInfo = 128)
 *   /usr/share/xcb/xv.xml                  (XCB machine-readable spec)
 *
 * XV Protocol version 2.2
 * Major opcodes are assigned dynamically by AddExtension.
 * Minor opcodes are fixed per the spec below.
 */

#ifndef CYXV_XVPROTO_H
#define CYXV_XVPROTO_H

#include <stdint.h>

/* ── Minor opcodes (Xvproto.h §Requests) ──────────────────────────────── */

#define X_XvQueryExtension         0
#define X_XvQueryAdaptors          1
#define X_XvQueryEncodings         2
#define X_XvGrabPort               3
#define X_XvUngrabPort             4
#define X_XvPutVideo               5
#define X_XvPutStill               6
#define X_XvGetVideo               7
#define X_XvGetStill               8
#define X_XvStopVideo              9
#define X_XvSelectVideoNotify      10
#define X_XvSelectPortNotify       11
#define X_XvQueryBestSize          12
#define X_XvSetPortAttribute       13
#define X_XvGetPortAttribute       14
#define X_XvQueryPortAttributes    15
#define X_XvListImageFormats       16
#define X_XvQueryImageAttributes   17
#define X_XvPutImage               18
#define X_XvShmPutImage            19

/* ── Adaptor type flags ────────────────────────────────────────────────── */

#define XvInputMask    (1 << 0)
#define XvOutputMask   (1 << 1)
#define XvVideoMask    (1 << 2)
#define XvStillMask    (1 << 3)
#define XvImageMask    (1 << 4)

/* ── Pixel format FOURCC codes ─────────────────────────────────────────── */

#define FOURCC_YV12  0x32315659  /* YV12 - planar 4:2:0  Y+V+U          */
#define FOURCC_I420  0x30323449  /* I420 - planar 4:2:0  Y+U+V          */
#define FOURCC_YUY2  0x32595559  /* YUY2 - packed 4:2:2  YUYV           */
#define FOURCC_UYVY  0x59565955  /* UYVY - packed 4:2:2  UYVY           */
#define FOURCC_NV12  0x3231564E  /* NV12 - semi-planar 4:2:0 Y+interUV  */

/* ── Grab status ───────────────────────────────────────────────────────── */

#define XvSuccess            0
#define XvBadExtension       1
#define XvAlreadyGrabbed     2
#define XvInvalidTime        3
#define XvBadReply           4
#define XvBadAlloc           5

/* ── Wire structures ───────────────────────────────────────────────────── */
/* All multi-byte fields are in the client's native byte order.
 * XWin handles byte-swapping via SwappedProcVector.
 * #pragma pack(push,1) ensures no compiler padding — field order controls
 * layout entirely; do not reorder.                                        */

#pragma pack(push, 1)

/* Generic X11 request header */
typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;
    uint16_t length;        /* in 4-byte units, including header */
} xGenericReq;

/* ── Replies (all 32 bytes) ─────────────────────────────────────────── */

typedef struct {
    uint8_t  type;           /* X_Reply = 1 */
    uint8_t  pad1;
    uint16_t sequenceNumber;
    uint32_t length;         /* 0 */
    uint16_t version;        /* 2 */
    uint16_t revision;       /* 2 */
    uint8_t  pad2[20];
} xXvQueryExtensionReply;    /* 32 bytes */

typedef struct {
    uint8_t  type;
    uint8_t  pad1;
    uint16_t sequenceNumber;
    uint32_t length;
    uint16_t num_adaptors;
    uint16_t pad2;
    uint8_t  pad3[20];
} xXvQueryAdaptorsReply;     /* 32 bytes */

typedef struct {
    uint8_t  type;
    uint8_t  pad1;
    uint16_t sequenceNumber;
    uint32_t length;
    uint16_t num_encodings;
    uint16_t pad2;
    uint8_t  pad3[20];
} xXvQueryEncodingsReply;    /* 32 bytes */

typedef struct {
    uint8_t  type;
    uint8_t  result;         /* XvSuccess = 0 */
    uint16_t sequenceNumber;
    uint32_t length;         /* 0 */
    uint8_t  pad[24];
} xXvGrabPortReply;          /* 32 bytes */

typedef struct {
    uint8_t  type;
    uint8_t  pad1;
    uint16_t sequenceNumber;
    uint32_t length;         /* 0 */
    int32_t  value;
    uint8_t  pad2[20];
} xXvGetPortAttributeReply;  /* 32 bytes */

typedef struct {
    uint8_t  type;
    uint8_t  pad1;
    uint16_t sequenceNumber;
    uint32_t length;         /* 0 */
    uint16_t actual_width;
    uint16_t actual_height;
    uint8_t  pad2[20];
} xXvQueryBestSizeReply;     /* 32 bytes */

typedef struct {
    uint8_t  type;
    uint8_t  pad1;
    uint16_t sequenceNumber;
    uint32_t length;
    uint32_t num_attributes;
    uint32_t text_size;
    uint8_t  pad2[16];
} xXvQueryPortAttributesReply; /* 32 bytes */

typedef struct {
    uint8_t  type;
    uint8_t  pad1;
    uint16_t sequenceNumber;
    uint32_t length;
    uint32_t num_formats;
    uint8_t  pad2[20];
} xXvListImageFormatsReply;  /* 32 bytes */

typedef struct {
    uint8_t  type;
    uint8_t  pad1;
    uint16_t sequenceNumber;
    uint32_t length;
    uint32_t num_planes;
    uint32_t data_size;
    uint16_t width;
    uint16_t height;
    uint8_t  pad2[12];
    /* followed by num_planes × uint32_t pitches, then num_planes × uint32_t offsets */
} xXvQueryImageAttributesReply; /* 32 bytes */

/* ── Variable-length bodies ─────────────────────────────────────────── */

/* xvAdaptorInfo: name string + pad + xvFormat[] follow in the stream */
typedef struct {
    uint32_t base_id;        /* first port XID               */
    uint16_t name_size;
    uint16_t num_ports;
    uint16_t num_formats;
    uint8_t  type;
    uint8_t  pad;
} xXvAdaptorInfo;            /* 12 bytes */

typedef struct {
    uint32_t visual;
    uint8_t  depth;
    uint8_t  pad[3];
} xXvFormat;                 /* 8 bytes */

/* xvEncodingInfo: name string + pad follow in the stream */
typedef struct {
    uint32_t encoding;       /* XID */
    uint16_t name_size;
    uint16_t width;
    uint16_t height;
    uint8_t  pad[2];
    uint32_t rate_numerator;
    uint32_t rate_denominator;
} xXvEncodingInfo;           /* 20 bytes */

/*
 * xXvImageFormatInfo — 128 bytes exactly.
 *
 * Field sizes match Xvproto.h (xvImageFormatInfo, sz_xvImageFormatInfo=128):
 *   y_sample_bits .. vert_v_period are CARD32 (uint32_t), NOT uint16_t.
 *
 * Layout (verified byte-by-byte against Xvproto.h):
 *   [  0] id            CARD32
 *   [  4] type          CARD8
 *   [  5] byte_order    CARD8
 *   [  6] pad1          CARD16
 *   [  8] guid[16]      CARD8×16
 *   [ 24] bpp           CARD8
 *   [ 25] num_planes    CARD8
 *   [ 26] pad2          CARD16
 *   [ 28] depth         CARD8
 *   [ 29] pad3          CARD8
 *   [ 30] pad4          CARD16
 *   [ 32] red_mask      CARD32
 *   [ 36] green_mask    CARD32
 *   [ 40] blue_mask     CARD32
 *   [ 44] format        CARD8
 *   [ 45] pad5          CARD8
 *   [ 46] pad6          CARD16
 *   [ 48] y_sample_bits CARD32   ← must be 4 bytes
 *   [ 52] u_sample_bits CARD32
 *   [ 56] v_sample_bits CARD32
 *   [ 60] horz_y_period CARD32
 *   [ 64] horz_u_period CARD32
 *   [ 68] horz_v_period CARD32
 *   [ 72] vert_y_period CARD32
 *   [ 76] vert_u_period CARD32
 *   [ 80] vert_v_period CARD32
 *   [ 84] comp_order[32] CARD8×32
 *   [116] scanline_order CARD8
 *   [117] pad7          CARD8
 *   [118] pad8          CARD16
 *   [120] pad9          CARD32
 *   [124] pad10         CARD32
 *   [128] — end —
 */
typedef struct {
    uint32_t id;
    uint8_t  type;
    uint8_t  byte_order;
    uint16_t pad1;
    uint8_t  guid[16];
    uint8_t  bpp;
    uint8_t  num_planes;
    uint16_t pad2;
    uint8_t  depth;
    uint8_t  pad3;
    uint16_t pad4;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint8_t  format;
    uint8_t  pad5;
    uint16_t pad6;
    uint32_t y_sample_bits;   /* CARD32 — do NOT shrink to uint16_t */
    uint32_t u_sample_bits;
    uint32_t v_sample_bits;
    uint32_t horz_y_period;
    uint32_t horz_u_period;
    uint32_t horz_v_period;
    uint32_t vert_y_period;
    uint32_t vert_u_period;
    uint32_t vert_v_period;
    uint8_t  comp_order[32];
    uint8_t  scanline_order;
    uint8_t  pad7;
    uint16_t pad8;
    uint32_t pad9;
    uint32_t pad10;
} xXvImageFormatInfo;        /* 128 bytes — verified against sz_xvImageFormatInfo */

/* Compile-time size check: breaks build if struct layout is wrong */
typedef char _xXvImageFormatInfo_size_check[
    (sizeof(xXvImageFormatInfo) == 128) ? 1 : -1];

/* ── Request structures ──────────────────────────────────────────────── */

typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 1 */
    uint16_t length;        /* 2 */
    uint32_t window;
} xXvQueryAdaptorsReq;

typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 3 */
    uint16_t length;        /* 3 */
    uint32_t port;
    uint32_t time;
} xXvGrabPortReq;

typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 12 */
    uint16_t length;        /* 5 */
    uint32_t port;
    uint16_t vid_w;
    uint16_t vid_h;
    uint16_t drw_w;
    uint16_t drw_h;
    uint8_t  motion;
    uint8_t  pad[3];
} xXvQueryBestSizeReq;

typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 16 */
    uint16_t length;        /* 2 */
    uint32_t port;
} xXvListImageFormatsReq;

typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 17 */
    uint16_t length;        /* 4 */
    uint32_t port;
    uint32_t id;            /* fourcc */
    uint16_t width;
    uint16_t height;
} xXvQueryImageAttributesReq;

typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 18 */
    uint16_t length;
    uint32_t port;
    uint32_t drawable;
    uint32_t gc;
    uint32_t id;            /* fourcc */
    int16_t  src_x;
    int16_t  src_y;
    uint16_t src_w;
    uint16_t src_h;
    int16_t  drw_x;
    int16_t  drw_y;
    uint16_t drw_w;
    uint16_t drw_h;
    uint16_t width;         /* full image width  */
    uint16_t height;        /* full image height */
    /* pixel data follows immediately */
} xXvPutImageReq;

typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 19 */
    uint16_t length;
    uint32_t port;
    uint32_t drawable;
    uint32_t gc;
    uint32_t shmseg;        /* MIT-SHM segment XID */
    uint32_t id;            /* fourcc */
    uint32_t offset;        /* byte offset into SHM segment */
    int16_t  src_x;
    int16_t  src_y;
    uint16_t src_w;
    uint16_t src_h;
    int16_t  drw_x;
    int16_t  drw_y;
    uint16_t drw_w;
    uint16_t drw_h;
    uint16_t width;
    uint16_t height;
    uint8_t  send_event;
    uint8_t  pad[3];
} xXvShmPutImageReq;

#pragma pack(pop)

#endif /* CYXV_XVPROTO_H */
