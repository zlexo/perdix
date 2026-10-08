#pragma once

#include <yyjson.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vx::minecraft {

struct JsonFree {
    void operator()(yyjson_doc* document) const noexcept { yyjson_doc_free(document); }
};

// A document yyjson has read, which holds all of its values.
using JsonDocument = std::unique_ptr<yyjson_doc, JsonFree>;

// Text read as JSON: the document, or where and why the text is not JSON.
struct JsonRead {
    JsonDocument document;    // null when the text is not JSON
    std::uint32_t line = 0;   // of the problem, from 1; 0 when it is the whole text
    std::uint32_t column = 0; // from 1, in characters
    std::string problem;      // in yyjson's words, such as "unexpected character"
};

// Standard JSON (RFC 8259) in valid UTF-8, read with yyjson.
[[nodiscard]] JsonRead read_json(std::string_view text);

// The member `key` of `object`, when it is a string: nothing when `object` is
// not an object, or has no such member, or one of another kind.
[[nodiscard]] std::optional<std::string_view> string_member(const yyjson_val* object,
                                                            std::string_view key);

} // namespace vx::minecraft
