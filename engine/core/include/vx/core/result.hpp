#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <utility>

namespace vx {

// Why an operation failed, in words a person can act on.
struct Error {
    std::string message;
};

// Either a value or an Error. Engine code reports failure this way because
// exceptions are off (RFC 0000). It stands in for std::expected, which needs
// C++23 (ADR 0001), and keeps to a subset of that interface so the switch is
// mechanical: construct from a value or an Error, test it, then read one side.
template <class T> class [[nodiscard]] Result {
public:
    // Implicit, like std::expected, so a function can `return value;` or
    // `return Error{"..."};`.
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & {
        assert(value_.has_value());
        return *value_;
    }
    [[nodiscard]] const T& value() const& {
        assert(value_.has_value());
        return *value_;
    }
    [[nodiscard]] T&& value() && {
        assert(value_.has_value());
        return std::move(*value_);
    }

    [[nodiscard]] const Error& error() const& {
        assert(!has_value());
        return error_;
    }

private:
    std::optional<T> value_;
    Error error_;
};

// Success with no value, or an Error, like std::expected<void, E>: a function
// can `return {};` or `return Error{"..."};`.
template <> class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)), failed_(true) {}

    [[nodiscard]] bool has_value() const noexcept { return !failed_; }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const Error& error() const& {
        assert(failed_);
        return error_;
    }

private:
    Error error_;
    bool failed_ = false;
};

} // namespace vx
