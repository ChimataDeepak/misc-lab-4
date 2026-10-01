// frame_buffer.hpp -- turns a BYTE STREAM back into whole QMUX frames.
//
// A stream socket (or a USB/MHI char device) may hand us half a frame, or
// one-and-a-half frames, in a single read(). FrameBuffer accumulates bytes and
// only returns a frame once all of it (as declared by the QMUX length) arrived.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mlab {

class FrameBuffer {
public:
    void append(const uint8_t* data, size_t n);
    // Next complete frame, or nullopt if more bytes are needed.
    std::optional<std::vector<uint8_t>> next();
    size_t buffered() const { return buf_.size() - start_; }
    size_t resync_bytes() const { return resync_bytes_; }

private:
    std::vector<uint8_t> buf_;
    size_t start_ = 0;          // consumed prefix; compacted lazily to avoid O(n^2) erases
    size_t resync_bytes_ = 0;   // garbage bytes skipped while hunting for a 0x01 marker
};

}  // namespace mlab
