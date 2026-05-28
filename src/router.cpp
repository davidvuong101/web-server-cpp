#include "router.hpp"
#include "response.hpp"

void Router::add_route(std::string method, std::string path, HandlerFn handler)
{
    routes_.push_back({std::move(method), std::move(path), std::move(handler)});
}

bool Router::dispatch(const ParsedRequest &req, Connection &conn) const noexcept
{
    for (const auto &route : routes_)
    {
        if (route.method == req.method && route.path == req.path)
        {
            route.handler(req, conn);
            return true;
        }
    }

    // No route matched -> write 404
    static constexpr std::string_view body = "{\"error\":\"not found\"}";
    conn.write_len = static_cast<uint32_t>(
        build_response({conn.write_buf, sizeof(conn.write_buf)},
                       status_code::k404, "application/json", body, conn.keep_alive));
    return false;
}
