#pragma once

#include "connection.hpp"
#include "http_parser.hpp"

#include <functional>
#include <string>
#include <vector>

// handler receives ParsedRequest and writes response directly
// to Connection's write buffer
using HandlerFn = std::function<void(const ParsedRequest &, Connection &)>;

class Router
{
public:
    void add_route(std::string method, std::string path, HandlerFn handler);
    bool dispatch(const ParsedRequest &req, Connection &conn) const noexcept;

private:
    struct Route
    {
        std::string method;
        std::string path;
        HandlerFn handler;
    };

    // does a linear scan of routes (not as scalable)
    std::vector<Route> routes_;
};