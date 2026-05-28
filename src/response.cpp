#include "response.hpp"

#include <cstring>
#include <cstdio>

static const char *status_text(int code) noexcept
{
    switch (code)
    {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 500:
        return "Internal Server Error";
    default:
        return "Unknown";
    }
}

// build HTTP response into buf
size_t build_response(
    std::span<char> buf,
    int status,
    std::string_view content_type,
    std::string_view body,
    bool keep_alive) noexcept
{
    int n = std::snprintf(
        buf.data(), buf.size(),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %.*s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status,
        status_text(status),
        static_cast<int>(content_type.size()), content_type.data(),
        body.size(),
        keep_alive ? "keep-alive" : "close");

    if (n <= 0 || static_cast<size_t>(n) >= buf.size())
    {
        return 0;
    }

    // copy the body into the buffer after the headers
    size_t header_len = static_cast<size_t>(n);
    if (header_len + body.size() > buf.size())
    {
        return 0;
    }

    std::memcpy(buf.data() + header_len, body.data(), body.size());
    return header_len + body.size();
}
