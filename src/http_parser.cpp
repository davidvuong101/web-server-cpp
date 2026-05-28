#include "http_parser.hpp"

#include <string>
#include <strings.h>

static void parse_headers(
    std::string_view request_line,
    std::string_view headers,
    uint32_t &content_length,
    bool &keep_alive) noexcept
{
    keep_alive = request_line.find("HTTP/1.1") != std::string_view::npos;
    content_length = 0;

    // loop through each header and look for connection and content-length
    while (!headers.empty())
    {
        size_t crlf_pos = headers.find("\r\n");
        std::string_view line = headers.substr(0, crlf_pos);

        if (line.size() > 15 && strncasecmp(line.data(), "content-length:", 15) == 0)
        {
            std::string_view val = line.substr(15);
            while (!val.empty() && val.front() == ' ')
            {
                val.remove_prefix(1);
            }
            for (char c : val)
            {
                if (!std::isdigit(static_cast<unsigned char>(c)))
                    break;
                content_length = content_length * 10 + static_cast<uint32_t>(c - '0');
            }
        }
        else if (line.size() > 11 && strncasecmp(line.data(), "connection:", 11) == 0)
        {
            std::string_view val = line.substr(11);
            while (!val.empty() && val.front() == ' ')
            {
                val.remove_prefix(1);
            }
            keep_alive = !(val.size() >= 5 &&
                           std::tolower(static_cast<unsigned char>(val[0])) == 'c' &&
                           std::tolower(static_cast<unsigned char>(val[1])) == 'l' &&
                           std::tolower(static_cast<unsigned char>(val[2])) == 'o' &&
                           std::tolower(static_cast<unsigned char>(val[3])) == 's' &&
                           std::tolower(static_cast<unsigned char>(val[4])) == 'e');
        }

        if (crlf_pos == std::string_view::npos)
            break;
        headers.remove_prefix(crlf_pos + 2); // skip past \r\n
    }
}

ParseResult HttpParser::parse(
    std::span<const char> data,
    ParsedRequest &out) noexcept
{
    std::string_view sv(data.data(), data.size());

    // Find header end - at \r\n\r\n
    size_t hdr_end = sv.find("\r\n\r\n");
    if (hdr_end == std::string::npos)
    {
        return ParseResult::kNeedMore;
    }

    // Get request line, e.g. POST /hello HTTP/1.1
    std::string_view request_line = sv.substr(0, sv.find("\r\n"));
    size_t sp1 = request_line.find(' ');
    if (sp1 == std::string_view::npos)
    {
        return ParseResult::kError;
    }
    size_t sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos)
    {
        return ParseResult::kError;
    }
    out.method = request_line.substr(0, sp1);
    out.path = request_line.substr(sp1 + 1, sp2 - (sp1 + 1));

    // Parse request headers, e.g. Connection: close
    std::string_view headers = sv.substr(
        request_line.size() + 2,
        hdr_end - (request_line.size() + 2));
    parse_headers(
        request_line, headers,
        out.content_length, out.keep_alive);

    // Parse HTTP body
    std::string_view body = sv.substr(hdr_end + 4);
    if (body.size() < out.content_length)
    {
        return ParseResult::kNeedMore;
    }

    out.body = body.substr(0, out.content_length);
    return ParseResult::kComplete;
}