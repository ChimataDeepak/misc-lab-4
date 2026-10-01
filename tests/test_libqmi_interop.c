/*
 * Interoperability check against UPSTREAM libqmi (Debian libqmi-glib 1.32).
 *
 * We encode messages with OUR codec, then hand the raw bytes to
 * qmi_message_new_from_raw() -- the same parser ModemManager/qmicli use on
 * real hardware -- and check libqmi reads back the same fields. Then we take a
 * message built by libqmi (qmi_message_new + TLV writer) and parse it with OUR
 * decoder. Message ids are written as raw hex here on purpose: libqmi keeps its
 * QMI_MESSAGE_* enums private, and spelling the spec numbers out keeps this
 * test independent of our own qmi.h constants. If both directions agree, our wire format is the real one.
 */
#include <libqmi-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qmi.h"
#include "sim_core.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
                         else printf("ok   %s\n", #cond); } while (0)

static QmiMessage *to_libqmi(const uint8_t *buf, ssize_t n)
{
    GByteArray *raw = g_byte_array_sized_new((guint)n);
    g_byte_array_append(raw, buf, (guint)n);
    GError *err = NULL;
    QmiMessage *m = qmi_message_new_from_raw(raw, &err); /* consumes bytes from raw */
    if (!m) {
        fprintf(stderr, "libqmi rejected our bytes: %s\n", err ? err->message : "?");
        g_clear_error(&err);
    }
    CHECK(raw->len == 0); /* libqmi consumed exactly one whole frame */
    g_byte_array_unref(raw);
    return m;
}

/* libqmi's generated pretty-printer knows the real DMS/NAS message
 * definitions, so its output proves semantic agreement too. */
static int printable_has(QmiMessage *m, const char *needle)
{
    gchar *p = qmi_message_get_printable_full(m, NULL, "");
    int ok = strstr(p, needle) != NULL;
    if (!ok || getenv("MLAB_SHOW_PRINTABLE"))
        fprintf(stderr, "---- libqmi printable ----\n%s\n--------------------------\n", p);
    g_free(p);
    return ok;
}

static void ctl_allocate_request(void)
{
    uint8_t buf[64];
    qmi_builder b;
    qmi_build_begin(&b, buf, sizeof buf, QMI_SVC_CTL, 0, QMI_MSG_REQUEST, 0x2A, QMI_CTL_ALLOCATE_CID);
    qmi_build_u8(&b, 0x01, QMI_SVC_NAS);
    QmiMessage *m = to_libqmi(buf, qmi_build_finish(&b));
    if (!m) { failures++; return; }
    CHECK(qmi_message_get_service(m) == QMI_SERVICE_CTL);
    CHECK(qmi_message_is_request(m));
    CHECK(qmi_message_get_transaction_id(m) == 0x2A);
    CHECK(qmi_message_get_message_id(m) == 0x0022);
    guint8 svc = 0;
    gsize off = 0, init = qmi_message_tlv_read_init(m, 0x01, NULL, NULL);
    CHECK(init != 0 && qmi_message_tlv_read_guint8(m, init, &off, &svc, NULL) && svc == QMI_SERVICE_NAS);
    qmi_message_unref(m);
}

static void dms_revision_response(void)
{
    /* Use the SIMULATOR's real handler to produce the response bytes. */
    sim_state s;
    sim_init(&s);
    uint8_t rq[64], rs[256];
    qmi_builder b;
    qmi_build_begin(&b, rq, sizeof rq, QMI_SVC_CTL, 0, QMI_MSG_REQUEST, 1, QMI_CTL_ALLOCATE_CID);
    qmi_build_u8(&b, 0x01, QMI_SVC_DMS);
    qmi_msg req;
    qmi_parse(rq, (size_t)qmi_build_finish(&b), &req);
    sim_handle(&s, &req, rs, sizeof rs);

    qmi_build_begin(&b, rq, sizeof rq, QMI_SVC_DMS, 1, QMI_MSG_REQUEST, 0x1234, QMI_DMS_GET_REVISION);
    qmi_parse(rq, (size_t)qmi_build_finish(&b), &req);
    ssize_t n = sim_handle(&s, &req, rs, sizeof rs);

    QmiMessage *m = to_libqmi(rs, n);
    if (!m) { failures++; return; }
    CHECK(qmi_message_get_service(m) == QMI_SERVICE_DMS);
    CHECK(qmi_message_is_response(m));
    CHECK(qmi_message_get_client_id(m) == 1);
    CHECK(qmi_message_get_transaction_id(m) == 0x1234);
    CHECK(qmi_message_get_message_id(m) == 0x0023);
    gchar *rev = NULL;
    gsize off = 0, init = qmi_message_tlv_read_init(m, 0x01, NULL, NULL);
    CHECK(init != 0 && qmi_message_tlv_read_string(m, init, &off, 0, 0, &rev, NULL));
    CHECK(rev && strcmp(rev, SIM_REVISION) == 0);
    g_free(rev);
    CHECK(printable_has(m, SIM_REVISION));
    CHECK(printable_has(m, "\"Get Revision\" (0x0023)"));
    qmi_message_unref(m);
}

static void dms_ids_and_nas(void)
{
    uint8_t buf[256];
    qmi_builder b;
    qmi_build_begin(&b, buf, sizeof buf, QMI_SVC_DMS, 3, QMI_MSG_RESPONSE, 9, QMI_DMS_GET_IDS);
    qmi_build_result(&b, 0, 0);
    qmi_build_tlv(&b, 0x11, SIM_IMEI, (uint16_t)strlen(SIM_IMEI));
    QmiMessage *m = to_libqmi(buf, qmi_build_finish(&b));
    if (m) {
        CHECK(qmi_message_get_message_id(m) == 0x0025);
        /* libqmi hides personal info (IMEI shows as '###') in printables, so
         * check the NAME it decoded, and read the value with the TLV reader. */
        CHECK(printable_has(m, "\"Get IDs\" (0x0025)"));
        CHECK(printable_has(m, "\"Imei\" (0x11)"));
        gchar *imei = NULL;
        gsize off = 0, init = qmi_message_tlv_read_init(m, 0x11, NULL, NULL);
        CHECK(init != 0 && qmi_message_tlv_read_string(m, init, &off, 0, 0, &imei, NULL));
        CHECK(imei && strcmp(imei, SIM_IMEI) == 0);
        g_free(imei);
        qmi_message_unref(m);
    } else failures++;

    qmi_build_begin(&b, buf, sizeof buf, QMI_SVC_NAS, 2, QMI_MSG_INDICATION, 0, QMI_NAS_EVENT_REPORT);
    uint8_t sig[2] = {(uint8_t)-77, QMI_NAS_RADIO_LTE};
    qmi_build_tlv(&b, 0x10, sig, 2);
    m = to_libqmi(buf, qmi_build_finish(&b));
    if (m) {
        CHECK(qmi_message_is_indication(m));
        CHECK(qmi_message_get_service(m) == QMI_SERVICE_NAS);
        gint8 dbm = 0;
        gsize off = 0, init = qmi_message_tlv_read_init(m, 0x10, NULL, NULL);
        CHECK(init != 0 && qmi_message_tlv_read_gint8(m, init, &off, &dbm, NULL) && dbm == -77);
        CHECK(printable_has(m, "strength = '-77' radio_interface = 'lte'"));
        qmi_message_unref(m);
    } else failures++;
}

static void libqmi_built_parsed_by_us(void)
{
    /* Reverse direction: libqmi encodes, our decoder parses. */
    GError *err = NULL;
    QmiMessage *m = qmi_message_new(QMI_SERVICE_NAS, 5, 0xABCD, 0x0020);
    gsize init = qmi_message_tlv_write_init(m, 0x10, &err);
    CHECK(init != 0 && qmi_message_tlv_write_guint16(m, QMI_ENDIAN_LITTLE, 0x0001, &err));
    CHECK(qmi_message_tlv_write_complete(m, init, &err));
    gsize len = 0;
    const guint8 *raw = qmi_message_get_raw(m, &len, &err);
    qmi_msg ours;
    CHECK(raw && qmi_parse(raw, len, &ours) == QMI_OK);
    CHECK(ours.service == QMI_SVC_NAS && ours.client_id == 5);
    CHECK(ours.txn_id == 0xABCD && ours.msg_id == QMI_NAS_GET_SIGNAL_STRENGTH);
    CHECK(ours.type == QMI_MSG_REQUEST);
    qmi_tlv t;
    CHECK(qmi_find_tlv(&ours, 0x10, &t) == QMI_OK && t.len == 2 && qmi_get_le16(t.val) == 1);
    qmi_message_unref(m);

    /* libqmi CTL message: 1-byte txn id on the wire */
    m = qmi_message_new(QMI_SERVICE_CTL, 0, 0x07, 0x0022);
    raw = qmi_message_get_raw(m, &len, &err);
    CHECK(raw && qmi_parse(raw, len, &ours) == QMI_OK && ours.txn_id == 7 && len == 12);
    qmi_message_unref(m);
    g_clear_error(&err);
}

int main(void)
{
    printf("libqmi-glib version %d.%d.%d\n", QMI_MAJOR_VERSION, QMI_MINOR_VERSION, QMI_MICRO_VERSION);
    ctl_allocate_request();
    dms_revision_response();
    dms_ids_and_nas();
    libqmi_built_parsed_by_us();
    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
