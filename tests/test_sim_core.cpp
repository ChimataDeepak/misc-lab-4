// Unit tests for the simulated modem's request handler (no sockets).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "sim_core.h"

namespace {
struct Rsp {
    std::vector<uint8_t> buf;
    qmi_msg m{};
    uint16_t result = 0xFFFF, error = 0xFFFF;
};

Rsp call(sim_state* s, uint8_t svc, uint8_t cid, uint16_t msg, std::vector<uint8_t> tlv01 = {}) {
    uint8_t req[128];
    qmi_builder b;
    qmi_build_begin(&b, req, sizeof req, svc, cid, QMI_MSG_REQUEST, 42, msg);
    if (!tlv01.empty()) qmi_build_tlv(&b, 0x01, tlv01.data(), (uint16_t)tlv01.size());
    ssize_t rl = qmi_build_finish(&b);
    qmi_msg rq;
    EXPECT_EQ(qmi_parse(req, (size_t)rl, &rq), QMI_OK);

    Rsp r;
    r.buf.resize(QMI_MAX_FRAME);
    ssize_t n = sim_handle(s, &rq, r.buf.data(), r.buf.size());
    EXPECT_GT(n, 0);
    r.buf.resize((size_t)n);
    EXPECT_EQ(qmi_parse(r.buf.data(), r.buf.size(), &r.m), QMI_OK);
    qmi_get_result(&r.m, &r.result, &r.error);
    return r;
}

std::string str(const Rsp& r, uint8_t t) {
    char s[128] = {0};
    qmi_tlv_string(&r.m, t, s, sizeof s);
    return s;
}
}  // namespace

TEST(SimCore, AllocateCidThenUseIt) {
    sim_state s;
    sim_init(&s);
    auto a = call(&s, QMI_SVC_CTL, 0, QMI_CTL_ALLOCATE_CID, {QMI_SVC_DMS});
    EXPECT_EQ(a.m.type, QMI_MSG_RESPONSE);
    EXPECT_EQ(a.m.txn_id, 42);  // echoed: this is what request/response matching relies on
    EXPECT_EQ(a.result, 0);
    qmi_tlv t;
    ASSERT_EQ(qmi_find_tlv(&a.m, 0x01, &t), QMI_OK);
    ASSERT_EQ(t.len, 2);
    EXPECT_EQ(t.val[0], QMI_SVC_DMS);
    uint8_t cid = t.val[1];
    EXPECT_EQ(cid, 1);

    auto rev = call(&s, QMI_SVC_DMS, cid, QMI_DMS_GET_REVISION);
    EXPECT_EQ(rev.result, 0);
    EXPECT_EQ(str(rev, 0x01), SIM_REVISION);

    auto ids = call(&s, QMI_SVC_DMS, cid, QMI_DMS_GET_IDS);
    EXPECT_EQ(str(ids, 0x11), SIM_IMEI);
    EXPECT_EQ(str(ids, 0x10), SIM_ESN);
    EXPECT_EQ(str(ids, 0x12), SIM_MEID);
}

TEST(SimCore, UnallocatedClientIdIsRejected) {
    sim_state s;
    sim_init(&s);
    auto r = call(&s, QMI_SVC_DMS, 9, QMI_DMS_GET_REVISION);
    EXPECT_EQ(r.result, 1);
    EXPECT_EQ(r.error, QMI_ERR_INVALID_CLIENT_ID);
}

TEST(SimCore, UnknownCommandAndMalformedAlloc) {
    sim_state s;
    sim_init(&s);
    auto r = call(&s, QMI_SVC_CTL, 0, 0x7777, {QMI_SVC_DMS});
    EXPECT_EQ(r.error, QMI_ERR_INVALID_QMI_COMMAND);
    auto m = call(&s, QMI_SVC_CTL, 0, QMI_CTL_ALLOCATE_CID);  // missing TLV 0x01
    EXPECT_EQ(m.error, QMI_ERR_MALFORMED_MESSAGE);
}

TEST(SimCore, SignalStrengthAndIndication) {
    sim_state s;
    sim_init(&s);
    auto a = call(&s, QMI_SVC_CTL, 0, QMI_CTL_ALLOCATE_CID, {QMI_SVC_NAS});
    qmi_tlv t;
    ASSERT_EQ(qmi_find_tlv(&a.m, 0x01, &t), QMI_OK);
    uint8_t nas = t.val[1];
    auto sig = call(&s, QMI_SVC_NAS, nas, QMI_NAS_GET_SIGNAL_STRENGTH);
    ASSERT_EQ(qmi_find_tlv(&sig.m, 0x01, &t), QMI_OK);
    EXPECT_EQ((int8_t)t.val[0], -70);
    EXPECT_EQ(t.val[1], QMI_NAS_RADIO_LTE);

    uint8_t buf[64];
    ssize_t n = sim_build_signal_indication(&s, nas, buf, sizeof buf);
    qmi_msg ind;
    ASSERT_EQ(qmi_parse(buf, (size_t)n, &ind), QMI_OK);
    EXPECT_EQ(ind.type, QMI_MSG_INDICATION);
    EXPECT_EQ(ind.client_id, nas);
    ASSERT_EQ(qmi_find_tlv(&ind, 0x10, &t), QMI_OK);
    EXPECT_EQ((int8_t)t.val[0], -75);
}
