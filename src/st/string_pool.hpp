#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// Where the text of STRING and WSTRING values lives. Every string variable
// and every temporary result has a buffer of a fixed size here, created
// before the program runs; a string's Value is the handle of its buffer.
// During a cycle text is only copied between buffers, never allocated, like
// on a real PLC where a STRING[20] always takes the same 22 bytes.
//
// Characters are 16-bit code units for both kinds: a STRING's are 0..255.
namespace softplc::st {

class StringPool {
public:
    // A new, empty buffer for at most `capacity` characters. Call this only
    // while setting up: it can move the existing text in memory.
    std::int64_t add(std::size_t capacity);

    std::size_t capacity(std::int64_t handle) const { return buffer(handle).capacity; }

    // The current text. It stays valid until the buffer is changed.
    std::u16string_view view(std::int64_t handle) const {
        const auto& b = buffer(handle);
        return {chars_.data() + b.offset, b.length};
    }

    // Replaces the text, cut to the capacity. `text` may be (part of) this
    // same buffer, as in s := MID(s, 3, 2).
    void assign(std::int64_t handle, std::u16string_view text);

    // Changes one character (0-based, inside the current text).
    void setChar(std::int64_t handle, std::size_t index, char16_t c) { chars_[buffer(handle).offset + index] = c; }

    void clear(std::int64_t handle) { buffer(handle).length = 0; }

private:
    struct Buffer {
        std::size_t offset = 0;
        std::size_t capacity = 0;
        std::size_t length = 0;
    };

    Buffer& buffer(std::int64_t handle) { return buffers_[static_cast<std::size_t>(handle)]; }
    const Buffer& buffer(std::int64_t handle) const { return buffers_[static_cast<std::size_t>(handle)]; }

    std::vector<char16_t> chars_;
    std::vector<Buffer> buffers_;
};

} // namespace softplc::st
