#include "sim_core.h"

#include <string.h>

void sim_init(sim_state *s)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 4; i++)
        s->next_cid[i] = 1;   /* cid 0 is reserved (CTL uses it) */
    s->signal_dbm = -70;
}

static int svc_ok(uint8_t service) { return service == QMI_SVC_DMS || service == QMI_SVC_NAS; }

int sim_cid_allocated(const sim_state *s, uint8_t service, uint8_t cid)
{
    if (service >= 4)
        return 0;
    return (s->allocated[service][cid / 8] >> (cid % 8)) & 1;
}

/* Starts a response that echoes the request's service/client/txn/msg ids:
 * that echo is exactly what the host uses to match responses to requests. */
static void begin_rsp(qmi_builder *b, const qmi_msg *req, uint8_t *out, size_t cap)
{
    qmi_build_begin(b, out, cap, req->service, req->client_id, QMI_MSG_RESPONSE, req->txn_id, req->msg_id);
}

static ssize_t fail(const qmi_msg *req, uint16_t err, uint8_t *out, size_t cap)
{
    qmi_builder b;
    begin_rsp(&b, req, out, cap);
    qmi_build_result(&b, 1, err);
    return qmi_build_finish(&b);
}

static ssize_t handle_ctl(sim_state *s, const qmi_msg *req, uint8_t *out, size_t cap)
{
    if (req->msg_id != QMI_CTL_ALLOCATE_CID)
        return fail(req, QMI_ERR_INVALID_QMI_COMMAND, out, cap);
    qmi_tlv t;
    if (qmi_find_tlv(req, 0x01, &t) || t.len != 1)
        return fail(req, QMI_ERR_MALFORMED_MESSAGE, out, cap);
    uint8_t svc = t.val[0];
    if (!svc_ok(svc))
        return fail(req, QMI_ERR_INVALID_QMI_COMMAND, out, cap);
    if (s->next_cid[svc] == 0)  /* wrapped past 255 */
        return fail(req, QMI_ERR_CLIENT_IDS_EXHAUSTED, out, cap);
    uint8_t cid = s->next_cid[svc]++;
    s->allocated[svc][cid / 8] |= (uint8_t)(1u << (cid % 8));

    qmi_builder b;
    begin_rsp(&b, req, out, cap);
    qmi_build_result(&b, 0, QMI_ERR_NONE);
    uint8_t info[2] = {svc, cid};   /* "Allocation Info" TLV: service, cid */
    qmi_build_tlv(&b, 0x01, info, sizeof info);
    return qmi_build_finish(&b);
}

ssize_t sim_handle(sim_state *s, const qmi_msg *req, uint8_t *out, size_t cap)
{
    if (req->type != QMI_MSG_REQUEST)
        return 0;  /* modems don't answer responses/indications */
    if (req->service == QMI_SVC_CTL)
        return handle_ctl(s, req, out, cap);
    if (!svc_ok(req->service))
        return fail(req, QMI_ERR_INVALID_QMI_COMMAND, out, cap);
    if (!sim_cid_allocated(s, req->service, req->client_id))
        return fail(req, QMI_ERR_INVALID_CLIENT_ID, out, cap);

    qmi_builder b;
    begin_rsp(&b, req, out, cap);
    if (req->service == QMI_SVC_DMS && req->msg_id == QMI_DMS_GET_REVISION) {
        qmi_build_result(&b, 0, 0);
        /* QMI strings are NOT NUL-terminated: the TLV length is the length. */
        qmi_build_tlv(&b, 0x01, SIM_REVISION, (uint16_t)strlen(SIM_REVISION));
    } else if (req->service == QMI_SVC_DMS && req->msg_id == QMI_DMS_GET_IDS) {
        qmi_build_result(&b, 0, 0);
        qmi_build_tlv(&b, 0x10, SIM_ESN, (uint16_t)strlen(SIM_ESN));
        qmi_build_tlv(&b, 0x11, SIM_IMEI, (uint16_t)strlen(SIM_IMEI));
        qmi_build_tlv(&b, 0x12, SIM_MEID, (uint16_t)strlen(SIM_MEID));
    } else if (req->service == QMI_SVC_NAS && req->msg_id == QMI_NAS_GET_SIGNAL_STRENGTH) {
        qmi_build_result(&b, 0, 0);
        uint8_t sig[2] = {(uint8_t)s->signal_dbm, QMI_NAS_RADIO_LTE};
        qmi_build_tlv(&b, 0x01, sig, sizeof sig);
    } else {
        return fail(req, QMI_ERR_INVALID_QMI_COMMAND, out, cap);
    }
    return qmi_build_finish(&b);
}

ssize_t sim_build_signal_indication(sim_state *s, uint8_t nas_cid, uint8_t *out, size_t cap)
{
    /* Wander the signal between -60 and -100 dBm so successive indications differ. */
    s->signal_dbm = (int8_t)(s->signal_dbm <= -100 ? -60 : s->signal_dbm - 5);
    qmi_builder b;
    qmi_build_begin(&b, out, cap, QMI_SVC_NAS, nas_cid, QMI_MSG_INDICATION, 0, QMI_NAS_EVENT_REPORT);
    uint8_t sig[2] = {(uint8_t)s->signal_dbm, QMI_NAS_RADIO_LTE};
    qmi_build_tlv(&b, 0x10, sig, sizeof sig);
    s->ind_txn++;
    return qmi_build_finish(&b);
}
