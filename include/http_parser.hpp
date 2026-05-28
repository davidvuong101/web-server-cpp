#pragma once

#include <string_view>
#include <span>
#include <cstdint>

enum class ParseResult : uint8_t
{
    kNeedMore, // incomplete request
    kComplete, // full request parsed
    kError     // malformed
};

struct ParsedRequest
{
    std::string_view method;
    std::string_view path;
    std::string_view body;
    uint32_t content_length{0};
    bool keep_alive{false};
};

class HttpParser
{
public:
    [[nodiscard]] static ParseResult parse(
        std::span<const char> data,
        ParsedRequest &out) noexcept;
};