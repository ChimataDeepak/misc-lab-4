/*
 * qmi.h -- QMUX + QMI SDU framing and TLV encode/decode (plain C, no deps).
 *
 * Wire layout of one QMI message as it travels over a QMUX transport
 * (e.g. /dev/cdc-wdm0 on a USB modem, or our Unix socket to the simulator):
 *
 *   QMUX header (6 bytes)
 *     u8   if_type    always 0x01 (the "marker")
 *     u16  length     little-endian; counts EVERYTHING after the marker
 *                     (i.e. total_frame_bytes - 1)
 *     u8   ctl_flags  0x00 = sent by host/control point, 0x80 = sent by service (modem)
 *     u8   service    0=CTL, 2=DMS, 3=NAS, ...
 *     u8   client_id  per-service client id handed out by CTL Allocate CID
 *
 *   QMI SDU header
 *     CTL service (service 0):           other services:
 *       u8  flags (0 req,1 rsp,2 ind)      u8  flags (0 req,2 rsp,4 ind)
 *       u8  transaction id                 u16 transaction id (LE)
 *     u16 message id (LE)
 *     u16 TLV-area length (LE)
 *
 *   TLVs, repeated:  u8 type | u16 length (LE) | value[length]
 *
 * Every response carries the mandatory "result" TLV 0x02:
 *     u16 result (0 = success, 1 = failure) | u16 error code
 */
#ifndef MLAB_QMI_H
#define MLAB_QMI_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QMUX_IF_TYPE            0x01
#define QMUX_HDR_LEN            6
#define QMUX_FLAG_FROM_SERVICE  0x80
#define QMI_MAX_FRAME           2048   /* our own sanity cap, real stacks use ~4-8K */

/* Services */
#define QMI_SVC_CTL 0x00
#define QMI_SVC_DMS 0x02
#define QMI_SVC_NAS 0x03

/* Messages */
#define QMI_CTL_ALLOCATE_CID          0x0022
#define QMI_DMS_GET_REVISION          0x0023
#define QMI_DMS_GET_IDS               0x0025  /* "Get Device Serial Numbers" */
#define QMI_NAS_EVENT_REPORT          0x0002  /* used here as an indication */
#define QMI_NAS_GET_SIGNAL_STRENGTH   0x0020

/* TLV types */
#define QMI_TLV_RESULT 0x02

/* QMI protocol error codes (subset, values match the QMI spec / libqmi) */
#define QMI_ERR_NONE                0x0000
#define QMI_ERR_MALFORMED_MESSAGE   0x0001
#define QMI_ERR_INTERNAL            0x0003
#define QMI_ERR_CLIENT_IDS_EXHAUSTED 0x0005
#define QMI_ERR_INVALID_CLIENT_ID   0x0007
#define QMI_ERR_INVALID_QMI_COMMAND 0x0047

/* NAS radio interface enum value for LTE (as in libqmi QmiNasRadioInterface) */
#define QMI_NAS_RADIO_LTE 0x08

typedef enum {
    QMI_MSG_REQUEST = 0,
    QMI_MSG_RESPONSE = 1,
    QMI_MSG_INDICATION = 2
} qmi_msg_type;

/* Return codes of this library (negative = error) */
enum {
    QMI_OK = 0,
    QMI_E_SHORT = -1,     /* buffer too small / truncated */
    QMI_E_MARKER = -2,    /* first byte is not 0x01 */
    QMI_E_LENGTH = -3,    /* length fields inconsistent */
    QMI_E_TLV = -4,       /* TLV walk overran the TLV area */
    QMI_E_NOTFOUND = -5,  /* TLV not present */
    QMI_E_FLAGS = -6      /* unknown SDU flags */
};

typedef struct {
    uint8_t type;
    uint16_t len;
    const uint8_t *val;   /* points INTO the parsed buffer: zero-copy */
} qmi_tlv;

typedef struct {
    uint8_t qmux_flags;
    uint8_t service;
    uint8_t client_id;
    qmi_msg_type type;
    uint16_t txn_id;        /* 8-bit for CTL, 16-bit otherwise */
    uint16_t msg_id;
    const uint8_t *tlvs;    /* points into the parsed buffer */
    uint16_t tlv_len;
} qmi_msg;

/* ---- Encoding ----------------------------------------------------------- */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    size_t sdu_len_off;   /* where the u16 TLV-area length lives */
    size_t tlv_start;
    int err;              /* sticky: once set, all adds become no-ops */
} qmi_builder;

void qmi_build_begin(qmi_builder *b, uint8_t *buf, size_t cap,
                     uint8_t service, uint8_t client_id, qmi_msg_type type,
                     uint16_t txn_id, uint16_t msg_id);
void qmi_build_tlv(qmi_builder *b, uint8_t type, const void *val, uint16_t len);
void qmi_build_u8(qmi_builder *b, uint8_t type, uint8_t v);
void qmi_build_result(qmi_builder *b, uint16_t result, uint16_t error);
/* Patches the two length fields; returns total frame size or a negative QMI_E_*. */
ssize_t qmi_build_finish(qmi_builder *b);

/* ---- Decoding ----------------------------------------------------------- */
/* Framing helper for stream transports. Looks at the first bytes of `buf`:
 *   returns 0          -> not enough bytes yet to know (need >= 3)
 *   returns  > 0       -> total size of the frame that starts at buf[0]
 *   returns  < 0       -> garbage (bad marker or absurd length)          */
ssize_t qmi_frame_length(const uint8_t *buf, size_t len);

/* Parse one complete frame (len must equal its frame length). Zero-copy. */
int qmi_parse(const uint8_t *buf, size_t len, qmi_msg *out);

int qmi_find_tlv(const qmi_msg *m, uint8_t type, qmi_tlv *out);
int qmi_get_result(const qmi_msg *m, uint16_t *result, uint16_t *error);
/* Copies a string TLV (not NUL-terminated on the wire) into dst (always NUL-terminated). */
int qmi_tlv_string(const qmi_msg *m, uint8_t type, char *dst, size_t dst_cap);

const char *qmi_strerror(int rc);

static inline uint16_t qmi_get_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline void qmi_put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

#ifdef __cplusplus
}
#endif
#endif
