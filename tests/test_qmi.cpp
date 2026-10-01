// Unit tests for the C QMI codec (lib/qmi): exact wire bytes + malformed input.
#include <gtest/gtest.h>

#include <vector>

#include "qmi.h"

namespace {
std::vector<uint8_t> build_ctl_alloc(uint8_t svc, uint8_t txn) {
    uint8_t buf[64];
    qmi_builder b;
    qmi_build_begin(&b, buf, sizeof buf, QMI_SVC_CTL, 0, QMI_MSG_REQUEST, txn, QMI_CTL_ALLOCATE_CID);
    qmi_build_u8(&b, 0x01, svc);
    ssize_t n = qmi_build_finish(&b);
    return {buf, buf + n};
}
}  // namespace

TEST(QmiEncode, CtlAllocateCidExactBytes) {
    // marker, len=15 LE, flags=host, svc=CTL, cid=0 | sdu flags=req, txn(1 byte), msg 0x0022, tlvlen 4 | TLV 01 len 1 val 02
    std::vector<uint8_t> want = {0x01, 0x0F, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x07, 0x22, 0x00, 0x04, 0x00,
                                 0x01, 0x01, 0x00, 0x02};
    EXPECT_EQ(build_ctl_alloc(QMI_SVC_DMS, 7), want);
}

TEST(QmiEncode, ServiceRequestUsesTwoByteTxn) {
    uint8_t buf[64];
    qmi_builder b;
    qmi_build_begin(&b, buf, sizeof buf, QMI_SVC_DMS, 1, QMI_MSG_REQUEST, 0x0102, QMI_DMS_GET_REVISION);
    ssize_t n = qmi_build_finish(&b);
    std::vector<uint8_t> want = {0x01, 0x0C, 0x00, 0x00, 0x02, 0x01,
                                 0x00, 0x02, 0x01, 0x23, 0x00, 0x00, 0x00};
    EXPECT_EQ(std::vector<uint8_t>(buf, buf + n), want);
}

TEST(QmiEncode, ResponseFlagsDifferForCtlAndServices) {
    uint8_t buf[64];
    qmi_builder b;
    qmi_build_begin(&b, buf, sizeof buf, QMI_SVC_CTL, 0, QMI_MSG_RESPONSE, 1, QMI_CTL_ALLOCATE_CID);
    qmi_build_finish(&b);
    EXPECT_EQ(buf[3], QMUX_FLAG_FROM_SERVICE);
    EXPECT_EQ(buf[6], 0x01);  // CTL response
    qmi_build_begin(&b, buf, sizeof buf, QMI_SVC_NAS, 1, QMI_MSG_INDICATION, 0, QMI_NAS_EVENT_REPORT);
    qmi_build_finish(&b);
    EXPECT_EQ(buf[6], 0x04);  // service indication
}

TEST(QmiEncode, OverflowIsReportedNotWritten) {
    uint8_t buf[14];
    qmi_builder b;
    qmi_build_begin(&b, buf, sizeof buf, QMI_SVC_DMS, 1, QMI_MSG_REQUEST, 1, 1);
    qmi_build_u8(&b, 0x10, 1);  // 13 + 4 > 14
    EXPECT_EQ(qmi_build_finish(&b), QMI_E_SHORT);
}

TEST(QmiDecode, RoundTripWithResultAndString) {
    uint8_t buf[128];
    qmi_builder b;
    qmi_build_begin(&b, buf, sizeof buf, QMI_SVC_DMS, 5, QMI_MSG_RESPONSE, 0xBEEF, QMI_DMS_GET_REVISION);
    qmi_build_result(&b, 0, 0);
    qmi_build_tlv(&b, 0x01, "REV1", 4);
    ssize_t n = qmi_build_finish(&b);
    ASSERT_GT(n, 0);

    qmi_msg m;
    ASSERT_EQ(qmi_parse(buf, (size_t)n, &m), QMI_OK);
    EXPECT_EQ(m.service, QMI_SVC_DMS);
    EXPECT_EQ(m.client_id, 5);
    EXPECT_EQ(m.type, QMI_MSG_RESPONSE);
    EXPECT_EQ(m.txn_id, 0xBEEF);
    EXPECT_EQ(m.msg_id, QMI_DMS_GET_REVISION);
    uint16_t res = 9, err = 9;
    ASSERT_EQ(qmi_get_result(&m, &res, &err), QMI_OK);
    EXPECT_EQ(res, 0);
    EXPECT_EQ(err, 0);
    char s[16];
    ASSERT_EQ(qmi_tlv_string(&m, 0x01, s, sizeof s), QMI_OK);
    EXPECT_STREQ(s, "REV1");
    EXPECT_EQ(qmi_tlv_string(&m, 0x01, s, 4), QMI_E_SHORT);  // no room for NUL
    qmi_tlv t;
    EXPECT_EQ(qmi_find_tlv(&m, 0x33, &t), QMI_E_NOTFOUND);
}

TEST(QmiDecode, FrameLengthNeedsThreeBytes) {
    auto f = build_ctl_alloc(2, 1);
    EXPECT_EQ(qmi_frame_length(f.data(), 0), 0);
    EXPECT_EQ(qmi_frame_length(f.data(), 2), 0);
    EXPECT_EQ(qmi_frame_length(f.data(), 3), (ssize_t)f.size());
}

TEST(QmiDecode, RejectsMalformed) {
    qmi_msg m;
    auto f = build_ctl_alloc(2, 1);

    auto bad = f; bad[0] = 0x02;
    EXPECT_EQ(qmi_parse(bad.data(), bad.size(), &m), QMI_E_MARKER);

    // QMUX length disagrees with the buffer
    EXPECT_EQ(qmi_parse(f.data(), f.size() - 1, &m), QMI_E_LENGTH);

    // SDU TLV-length disagrees with QMUX length
    bad = f; bad[10] = 0x05;
    EXPECT_EQ(qmi_parse(bad.data(), bad.size(), &m), QMI_E_LENGTH);

    // TLV claims more bytes than exist
    bad = f; bad[13] = 0x09;
    EXPECT_EQ(qmi_parse(bad.data(), bad.size(), &m), QMI_E_TLV);

    // unknown SDU flags
    bad = f; bad[6] = 0x7F;
    EXPECT_EQ(qmi_parse(bad.data(), bad.size(), &m), QMI_E_FLAGS);

    // absurd QMUX length
    uint8_t huge[3] = {0x01, 0xFF, 0xFF};
    EXPECT_EQ(qmi_frame_length(huge, 3), QMI_E_LENGTH);
}
