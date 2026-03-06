/*
 * cyxv_xvproto.h — XVideo wire protocol definitions
 *
 * Extracted from <X11/extensions/Xvproto.h> and the XV protocol spec.
 * Used by cyxv_dispatch.c to parse raw X11 request packets.
 *
 * XV Protocol version 2.2
 * Major opcodes are assigned dynamically by AddExtension.
 * Minor opcodes are fixed per the spec below.
 */

#ifndef CYXV_XVPROTO_H
#define CYXV_XVPROTO_H

#include <stdint.h>

/* ── Minor opcodes ─────────────────────────────────────────────────────────── */

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

/* ── Adaptor type flags ────────────────────────────────────────────────────── */

#define XvInputMask    (1 << 0)
#define XvOutputMask   (1 << 1)
#define XvVideoMask    (1 << 2)
#define XvStillMask    (1 << 3)
#define XvImageMask    (1 << 4)

/* ── Pixel format fourcc codes ─────────────────────────────────────────────── */

#define FOURCC_YV12  0x32315659  /* YV12 - planar YUV 4:2:0, Y+V+U planes */
#define FOURCC_I420  0x30323449  /* I420 - planar YUV 4:2:0, Y+U+V planes */
#define FOURCC_YUY2  0x32595559  /* YUY2 - packed YUV 4:2:2 */
#define FOURCC_UYVY  0x59565955  /* UYVY - packed YUV 4:2:2 */
#define FOURCC_NV12  0x3231564E  /* NV12 - semi-planar YUV 4:2:0 */

/* ── Grab status ───────────────────────────────────────────────────────────── */

#define XvSuccess            0
#define XvBadExtension       1
#define XvAlreadyGrabbed     2
#define XvInvalidTime        3
#define XvBadReply           4
#define XvBadAlloc           5

/* ── Wire request structures ───────────────────────────────────────────────── */
/* All multi-byte fields are in the client's native byte order.
 * XWin handles byte swapping via SwappedProcVector.             */

#pragma pack(push, 1)

/* Generic X11 request header */
typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* For extension requests: the minor op */
    uint16_t length;        /* In 4-byte units, including header */
} xGenericReq;

/* XvQueryExtension reply */
typedef struct {
    uint8_t  type;          /* X_Reply = 1 */
    uint8_t  pad;
    uint16_t sequenceNumber;
    uint32_t length;        /* 0 */
    uint16_t version;       /* 2 */
    uint16_t revision;      /* 2 */
    uint8_t  pad2[20];
} xXvQueryExtensionReply;

/* XvQueryAdaptors request */
typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 1 */
    uint16_t length;        /* 2 */
    uint32_t window;
} xXvQueryAdaptorsReq;

/* XvAdaptorInfo wire structure (variable-length, name follows) */
typedef struct {
    uint32_t base_id;       /* First port XID */
    uint16_t name_size;
    uint16_t num_ports;
    uint16_t num_formats;
    uint8_t  type;
    uint8_t  pad;
} xXvAdaptorInfo;

/* XvFormat wire structure */
typedef struct {
    uint32_t visual;
    uint8_t  depth;
    uint8_t  pad[3];
} xXvFormat;

/* XvQueryAdaptors reply header */
typedef struct {
    uint8_t  type;
    uint8_t  pad;
    uint16_t sequenceNumber;
    uint32_t length;
    uint32_t num_adaptors;
    uint8_t  pad2[20];
} xXvQueryAdaptorsReply;

/* XvGrabPort request */
typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 3 */
    uint16_t length;        /* 3 */
    uint32_t port;
    uint32_t time;
} xXvGrabPortReq;

/* XvGrabPort reply */
typedef struct {
    uint8_t  type;
    uint8_t  result;        /* XvSuccess=0 */
    uint16_t sequenceNumber;
    uint32_t length;        /* 0 */
    uint8_t  pad[24];
} xXvGrabPortReply;

/* XvListImageFormats request */
typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 16 */
    uint16_t length;        /* 2 */
    uint32_t port;
} xXvListImageFormatsReq;

/* XvListImageFormats reply header */
typedef struct {
    uint8_t  type;
    uint8_t  pad;
    uint16_t sequenceNumber;
    uint32_t length;
    uint32_t num_formats;
    uint8_t  pad2[20];
} xXvListImageFormatsReply;

/* XvImageFormatInfo - one per supported format */
typedef struct {
    uint32_t id;            /* fourcc */
    uint8_t  type;          /* 0=RGB 1=YUV */
    uint8_t  byte_order;    /* LSBFirst=0 */
    uint8_t  pad[2];
    uint8_t  guid[16];
    uint8_t  bpp;
    uint8_t  num_planes;
    uint8_t  pad2[2];
    uint8_t  depth;
    uint8_t  pad3[3];
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint8_t  format;        /* 0=Packed 1=Planar */
    uint8_t  pad4[3];
    uint16_t y_sample_bits;
    uint16_t u_sample_bits;
    uint16_t v_sample_bits;
    uint16_t horz_y_period;
    uint16_t horz_u_period;
    uint16_t horz_v_period;
    uint16_t vert_y_period;
    uint16_t vert_u_period;
    uint16_t vert_v_period;
    char     comp_order[32];/* "YVU" etc. */
    uint8_t  scanline_order;/* 0=TopToBottom */
    uint8_t  pad5[11];
} xXvImageFormatInfo;   /* 128 bytes total */

/* XvQueryImageAttributes request */
typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 17 */
    uint16_t length;        /* 4 */
    uint32_t port;
    uint32_t id;            /* fourcc */
    uint16_t width;
    uint16_t height;
} xXvQueryImageAttributesReq;

/* XvQueryImageAttributes reply */
typedef struct {
    uint8_t  type;
    uint8_t  pad;
    uint16_t sequenceNumber;
    uint32_t length;
    uint32_t num_planes;
    uint32_t data_size;     /* total bytes needed */
    uint16_t width;         /* actual (may be rounded up) */
    uint16_t height;
    uint8_t  pad2[12];
    /* followed by num_planes uint32_t pitches, then num_planes uint32_t offsets */
} xXvQueryImageAttributesReply;

/* XvPutImage request */
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
    uint16_t width;         /* image width */
    uint16_t height;        /* image height */
    /* followed by image data */
} xXvPutImageReq;

/* XvShmPutImage request */
typedef struct {
    uint8_t  major_opcode;
    uint8_t  minor_opcode;  /* 19 */
    uint16_t length;
    uint32_t port;
    uint32_t drawable;
    uint32_t gc;
    uint32_t shmseg;        /* SHM segment XID */
    uint32_t id;            /* fourcc */
    uint32_t offset;        /* byte offset into SHM */
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

/* XvQueryPortAttributes reply */
typedef struct {
    uint8_t  type;
    uint8_t  pad;
    uint16_t sequenceNumber;
    uint32_t length;
    uint32_t num_attributes;
    uint32_t text_size;
    uint8_t  pad2[16];
} xXvQueryPortAttributesReply;

/* XvQueryEncodings reply */
typedef struct {
    uint8_t  type;
    uint8_t  pad;
    uint16_t sequenceNumber;
    uint32_t length;
    uint32_t num_encodings;
    uint8_t  pad2[20];
} xXvQueryEncodingsReply;

/* XvEncodingInfo wire struct */
typedef struct {
    uint32_t encoding;      /* XID */
    uint16_t name_size;
    uint16_t width;
    uint16_t height;
    uint8_t  pad[2];
    uint32_t rate_numerator;
    uint32_t rate_denominator;
} xXvEncodingInfo;

#pragma pack(pop)

#endif /* CYXV_XVPROTO_H */
