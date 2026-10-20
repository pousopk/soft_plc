#include "st/string_pool.hpp"
#include <algorithm>
#include <string>

namespace softplc::st {

std::int64_t StringPool::add(std::size_t capacity) {
    buffers_.push_back(Buffer{chars_.size(), capacity, 0});
    chars_.resize(chars_.size() + capacity);
    return static_cast<std::int64_t>(buffers_.size() - 1);
}

void StringPool::assign(std::int64_t handle, std::u16string_view text) {
    auto& b = buffer(handle);
    const auto length = std::min(text.size(), b.capacity);
    // move, not copy: the text may overlap the buffer it's copied into.
    std::char_traits<char16_t>::move(chars_.data() + b.offset, text.data(), length);
    b.length = length;
}

} // namespace softplc::st
