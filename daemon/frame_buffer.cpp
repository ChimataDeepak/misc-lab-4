#include "frame_buffer.hpp"

#include "qmi.h"

namespace mlab {

void FrameBuffer::append(const uint8_t* data, size_t n) {
    if (start_ > 0 && start_ >= buf_.size() / 2) {  // compact occasionally
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(start_));
        start_ = 0;
    }
    buf_.insert(buf_.end(), data, data + n);
}

std::optional<std::vector<uint8_t>> FrameBuffer::next() {
    for (;;) {
        const uint8_t* p = buf_.data() + start_;
        size_t avail = buf_.size() - start_;
        ssize_t fl = qmi_frame_length(p, avail);
        if (fl < 0) {
            // Corrupt stream: drop ONE byte and look for the next marker.
            // (Dropping the whole buffer could throw away a good frame.)
            start_++;
            resync_bytes_++;
            continue;
        }
        if (fl == 0 || static_cast<size_t>(fl) > avail) return std::nullopt;  // partial
        std::vector<uint8_t> frame(p, p + fl);
        start_ += static_cast<size_t>(fl);
        return frame;
    }
}

}  // namespace mlab
