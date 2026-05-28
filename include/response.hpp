#pragma once

#include <span>
#include <string_view>

// build HTTP/1.1 response into buf
size_t build_response(
    std::span<char> buf,
    int status,
    std::string_view content_type,
    std::string_view body,
    bool keep_alive = true) noexcept;

// HTTP status codes
namespace status_code
{
    inline constexpr int k200 = 200;
    inline constexpr int k400 = 400;
    inline constexpr int k404 = 404;
    inline constexpr int k500 = 500;
}