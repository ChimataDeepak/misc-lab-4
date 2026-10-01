// FrameBuffer: reassembling frames from arbitrary chunking of a byte stream.
#include <gtest/gtest.h>

#include "frame_buffer.hpp"
#include "qmi.h"

using mlab::FrameBuffer;

namespace {
std::vector<uint8_t> frame(uint16_t txn, const char* payload) {
    uint8_t buf[128];
    qmi_builder b;
    qmi_build_begin(&b, buf, sizeof buf, QMI_SVC_DMS, 1, QMI_MSG_RESPONSE, txn, QMI_DMS_GET_REVISION);
    qmi_build_result(&b, 0, 0);
    qmi_build_tlv(&b, 0x01, payload, (uint16_t)strlen(payload));
    ssize_t n = qmi_build_finish(&b);
    return {buf, buf + n};
}
}  // namespace

TEST(FrameBuffer, ByteByByte) {
    auto f = frame(1, "hello");
    FrameBuffer fb;
    for (size_t i = 0; i + 1 < f.size(); i++) {
        fb.append(&f[i], 1);
        EXPECT_FALSE(fb.next().has_value()) << "frame returned early at byte " << i;
    }
    fb.append(&f.back(), 1);
    auto got = fb.next();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, f);
    EXPECT_EQ(fb.buffered(), 0u);
}

TEST(FrameBuffer, TwoAndAHalfFramesInOneRead) {
    auto a = frame(1, "a"), b = frame(2, "bb"), c = frame(3, "ccc");
    std::vector<uint8_t> stream = a;
    stream.insert(stream.end(), b.begin(), b.end());
    stream.insert(stream.end(), c.begin(), c.begin() + 4);
    FrameBuffer fb;
    fb.append(stream.data(), stream.size());
    EXPECT_EQ(*fb.next(), a);
    EXPECT_EQ(*fb.next(), b);
    EXPECT_FALSE(fb.next().has_value());
    fb.append(c.data() + 4, c.size() - 4);
    EXPECT_EQ(*fb.next(), c);
}

TEST(FrameBuffer, ResyncsAfterGarbage) {
    auto a = frame(7, "ok");
    std::vector<uint8_t> stream = {0xAA, 0xBB, 0xCC};
    stream.insert(stream.end(), a.begin(), a.end());
    FrameBuffer fb;
    fb.append(stream.data(), stream.size());
    EXPECT_EQ(*fb.next(), a);
    EXPECT_EQ(fb.resync_bytes(), 3u);
}
