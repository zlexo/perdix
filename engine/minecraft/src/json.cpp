#include "json.hpp"

#include <yyjson.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace vx::minecraft {

JsonRead read_json(std::string_view text) {
    JsonRead read;
    // yyjson only writes into the text with YYJSON_READ_INSITU, so it may be
    // handed a pointer to const text.
    yyjson_read_err error{};
    read.document.reset(yyjson_read_opts(const_cast<char*>(text.data()), text.size(),
                                         YYJSON_READ_NOFLAG, nullptr, &error));
    if (read.document != nullptr) {
        return read;
    }
    // Text with no value at all, which yyjson takes for a mistaken call when
    // it has no bytes, is a problem of the whole text.
    if (text.empty() || error.code == YYJSON_READ_ERROR_EMPTY_CONTENT) {
        read.problem = "the text is empty";
        return read;
    }
    read.problem = error.msg != nullptr ? error.msg : "not JSON";
    std::size_t line = 0;
    std::size_t column = 0;
    std::size_t character = 0;
    if (yyjson_locate_pos(text.data(), text.size(), error.pos, &line, &column, &character) &&
        line <= std::numeric_limits<std::uint32_t>::max() &&
        column <= std::numeric_limits<std::uint32_t>::max()) {
        read.line = static_cast<std::uint32_t>(line);
        read.column = static_cast<std::uint32_t>(column);
    }
    return read;
}

std::optional<std::string_view> string_member(const yyjson_val* object, std::string_view key) {
    const yyjson_val* member = yyjson_obj_getn(object, key.data(), key.size());
    if (!yyjson_is_str(member)) {
        return std::nullopt;
    }
    return std::string_view(yyjson_get_str(member), yyjson_get_len(member));
}

} // namespace vx::minecraft
