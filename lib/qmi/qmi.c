/* qmi.c -- see qmi.h for the wire format diagram. */
#include "qmi.h"

#include <string.h>

/* SDU header size differs by service: CTL uses a 1-byte transaction id. */
static size_t sdu_hdr_len(uint8_t service) { return service == QMI_SVC_CTL ? 6u : 7u; }

/* The SDU "flags" byte encodes request/response/indication, and the bit
 * values are DIFFERENT for CTL vs every other service. Classic gotcha. */
static uint8_t sdu_flags_for(uint8_t service, qmi_msg_type t)
{
    if (service == QMI_SVC_CTL)
        return t == QMI_MSG_REQUEST ? 0x00 : t == QMI_MSG_RESPONSE ? 0x01 : 0x02;
    return t == QMI_MSG_REQUEST ? 0x00 : t == QMI_MSG_RESPONSE ? 0x02 : 0x04;
}

static int sdu_type_from(uint8_t service, uint8_t flags, qmi_msg_type *t)
{
    if (service == QMI_SVC_CTL) {
        switch (flags) {
        case 0x00: *t = QMI_MSG_REQUEST; return QMI_OK;
        case 0x01: *t = QMI_MSG_RESPONSE; return QMI_OK;
        case 0x02: *t = QMI_MSG_INDICATION; return QMI_OK;
        default: return QMI_E_FLAGS;
        }
    }
    switch (flags) {
    case 0x00: *t = QMI_MSG_REQUEST; return QMI_OK;
    case 0x02: *t = QMI_MSG_RESPONSE; return QMI_OK;
    case 0x04: *t = QMI_MSG_INDICATION; return QMI_OK;
    default: return QMI_E_FLAGS;
    }
}

static void put(qmi_builder *b, const void *src, size_t n)
{
    if (b->err)
        return;
    if (b->len + n > b->cap) {
        b->err = QMI_E_SHORT;
        return;
    }
    memcpy(b->buf + b->len, src, n);
    b->len += n;
}

void qmi_build_begin(qmi_builder *b, uint8_t *buf, size_t cap, uint8_t service,
                     uint8_t client_id, qmi_msg_type type, uint16_t txn_id, uint16_t msg_id)
{
    memset(b, 0, sizeof(*b));
    b->buf = buf;
    b->cap = cap;
    /* Requests come from the host (0x00); responses/indications from the modem (0x80). */
    uint8_t qmux[QMUX_HDR_LEN] = {QMUX_IF_TYPE, 0, 0,
                                  (uint8_t)(type == QMI_MSG_REQUEST ? 0x00 : QMUX_FLAG_FROM_SERVICE),
                                  service, client_id};
    put(b, qmux, sizeof qmux);

    uint8_t sdu[7];
    size_t n = 0;
    sdu[n++] = sdu_flags_for(service, type);
    if (service == QMI_SVC_CTL) {
        sdu[n++] = (uint8_t)txn_id;           /* CTL: 1-byte txn id */
    } else {
        qmi_put_le16(&sdu[n], txn_id);        /* others: 2-byte txn id */
        n += 2;
    }
    qmi_put_le16(&sdu[n], msg_id);
    n += 2;
    b->sdu_len_off = b->len + n;              /* patched in finish() */
    sdu[n++] = 0;
    sdu[n++] = 0;
    put(b, sdu, n);
    b->tlv_start = b->len;
}

void qmi_build_tlv(qmi_builder *b, uint8_t type, const void *val, uint16_t len)
{
    uint8_t hdr[3] = {type, (uint8_t)len, (uint8_t)(len >> 8)};
    put(b, hdr, 3);
    if (len)
        put(b, val, len);
}

void qmi_build_u8(qmi_builder *b, uint8_t type, uint8_t v) { qmi_build_tlv(b, type, &v, 1); }

void qmi_build_result(qmi_builder *b, uint16_t result, uint16_t error)
{
    uint8_t v[4];
    qmi_put_le16(v, result);
    qmi_put_le16(v + 2, error);
    qmi_build_tlv(b, QMI_TLV_RESULT, v, sizeof v);
}

ssize_t qmi_build_finish(qmi_builder *b)
{
    if (b->err)
        return b->err;
    /* QMUX length excludes only the 1-byte marker. */
    qmi_put_le16(b->buf + 1, (uint16_t)(b->len - 1));
    qmi_put_le16(b->buf + b->sdu_len_off, (uint16_t)(b->len - b->tlv_start));
    return (ssize_t)b->len;
}

ssize_t qmi_frame_length(const uint8_t *buf, size_t len)
{
    if (len < 1)
        return 0;
    if (buf[0] != QMUX_IF_TYPE)
        return QMI_E_MARKER;
    if (len < 3)
        return 0; /* need the u16 length before we know anything */
    size_t total = (size_t)qmi_get_le16(buf + 1) + 1;
    /* Smallest legal frame: QMUX header + CTL SDU header with zero TLVs. */
    if (total < QMUX_HDR_LEN + 6 || total > QMI_MAX_FRAME)
        return QMI_E_LENGTH;
    return (ssize_t)total;
}

int qmi_parse(const uint8_t *buf, size_t len, qmi_msg *out)
{
    ssize_t fl = qmi_frame_length(buf, len);
    if (fl < 0)
        return (int)fl;
    if (fl == 0 || (size_t)fl != len)
        return QMI_E_LENGTH;

    memset(out, 0, sizeof(*out));
    out->qmux_flags = buf[3];
    out->service = buf[4];
    out->client_id = buf[5];

    size_t hl = sdu_hdr_len(out->service);
    if (len < QMUX_HDR_LEN + hl)
        return QMI_E_SHORT;
    const uint8_t *s = buf + QMUX_HDR_LEN;
    int rc = sdu_type_from(out->service, s[0], &out->type);
    if (rc)
        return rc;
    size_t off;
    if (out->service == QMI_SVC_CTL) {
        out->txn_id = s[1];
        off = 2;
    } else {
        out->txn_id = qmi_get_le16(s + 1);
        off = 3;
    }
    out->msg_id = qmi_get_le16(s + off);
    out->tlv_len = qmi_get_le16(s + off + 2);
    out->tlvs = buf + QMUX_HDR_LEN + hl;

    /* The two independent length fields must agree exactly. */
    if (QMUX_HDR_LEN + hl + out->tlv_len != len)
        return QMI_E_LENGTH;

    /* Validate the whole TLV area up front so later lookups can't overrun. */
    size_t p = 0;
    while (p < out->tlv_len) {
        if (out->tlv_len - p < 3)
            return QMI_E_TLV;
        size_t vl = qmi_get_le16(out->tlvs + p + 1);
        if (vl > out->tlv_len - p - 3)
            return QMI_E_TLV;
        p += 3 + vl;
    }
    return QMI_OK;
}

int qmi_find_tlv(const qmi_msg *m, uint8_t type, qmi_tlv *out)
{
    size_t p = 0;
    while (p + 3 <= m->tlv_len) {
        uint16_t vl = qmi_get_le16(m->tlvs + p + 1);
        if (m->tlvs[p] == type) {
            out->type = type;
            out->len = vl;
            out->val = m->tlvs + p + 3;
            return QMI_OK;
        }
        p += 3u + vl;
    }
    return QMI_E_NOTFOUND;
}

int qmi_get_result(const qmi_msg *m, uint16_t *result, uint16_t *error)
{
    qmi_tlv t;
    if (qmi_find_tlv(m, QMI_TLV_RESULT, &t) || t.len != 4)
        return QMI_E_NOTFOUND;
    *result = qmi_get_le16(t.val);
    *error = qmi_get_le16(t.val + 2);
    return QMI_OK;
}

int qmi_tlv_string(const qmi_msg *m, uint8_t type, char *dst, size_t dst_cap)
{
    qmi_tlv t;
    if (dst_cap == 0)
        return QMI_E_SHORT;
    if (qmi_find_tlv(m, type, &t))
        return QMI_E_NOTFOUND;
    if ((size_t)t.len + 1 > dst_cap)
        return QMI_E_SHORT;
    memcpy(dst, t.val, t.len);
    dst[t.len] = '\0';
    return QMI_OK;
}

const char *qmi_strerror(int rc)
{
    switch (rc) {
    case QMI_OK: return "ok";
    case QMI_E_SHORT: return "buffer too short";
    case QMI_E_MARKER: return "bad QMUX marker";
    case QMI_E_LENGTH: return "inconsistent length";
    case QMI_E_TLV: return "malformed TLV area";
    case QMI_E_NOTFOUND: return "TLV not found";
    case QMI_E_FLAGS: return "unknown SDU flags";
    default: return "unknown error";
    }
}
