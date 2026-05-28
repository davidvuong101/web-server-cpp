#include "connection.hpp"
#include "http_parser.hpp"
#include "response.hpp"
#include "router.hpp"

#include <nlohmann/json.hpp>

#include <unordered_map>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cerrno>

static constexpr int PORT = 8080;
static constexpr int BACKLOG = 128;   // pending connections queued before accept()
static constexpr int MAX_EVENTS = 64; // max events per epoll_wait()

struct Server
{
    int epoll_fd;
    int listen_fd;
    Router router;
    std::unordered_map<int, Connection> conns;
};

// -- Route handlers -------------------------------------------------------

static void handle_ping(const ParsedRequest &, Connection &conn)
{
    static constexpr std::string_view body = "pong";
    conn.write_len = static_cast<uint32_t>(
        build_response({conn.write_buf, sizeof(conn.write_buf)},
                       status_code::k200, "text/plain", body, conn.keep_alive));
}

static void handle_echo(const ParsedRequest &req, Connection &conn)
{
    nlohmann::json resp;
    try
    {
        resp["echo"] = nlohmann::json::parse(req.body);
    }
    catch (...)
    {
        resp["echo"] = std::string(req.body);
    }
    std::string body = resp.dump();
    conn.write_len = static_cast<uint32_t>(
        build_response({conn.write_buf, sizeof(conn.write_buf)},
                       status_code::k200, "application/json", body, conn.keep_alive));
}

// -- Event handlers -------------------------------------------------------

static void close_connection(Server &srv, int fd) noexcept
{
    epoll_ctl(srv.epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    srv.conns.erase(fd);
}

static void on_write(Server &srv, int fd, Connection &conn) noexcept
{
    ssize_t sent = send(fd,
                        conn.write_buf + conn.write_offset,
                        conn.write_len - conn.write_offset,
                        MSG_NOSIGNAL); // return EPIPE instead of raising SIGPIPE
    if (sent < 0)
    {
        close_connection(srv, fd);
        return;
    }
    conn.write_offset += static_cast<uint32_t>(sent);

    if (conn.write_offset < conn.write_len)
    {
        return; // partial send -- epoll will fire EPOLLOUT again
    }

    if (conn.keep_alive)
    {
        conn.reset();
        conn.fd = fd;
        // Switch back to read mode
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(srv.epoll_fd, EPOLL_CTL_MOD, fd, &ev);
    }
    else
    {
        close_connection(srv, fd);
    }
}

static void on_read(Server &srv, int fd, Connection &conn) noexcept
{
    ssize_t n = recv(fd,
                     conn.read_buf + conn.bytes_read,
                     sizeof(conn.read_buf) - conn.bytes_read, 0);
    if (n <= 0)
    {
        close_connection(srv, fd);
        return;
    }
    conn.bytes_read += static_cast<uint32_t>(n);

    ParsedRequest req;
    ParseResult result = HttpParser::parse({conn.read_buf, conn.bytes_read}, req);
    conn.keep_alive = req.keep_alive;

    if (result == ParseResult::kNeedMore)
    {
        return; // stay in EPOLLIN mode -- wait for the rest of the request
    }

    if (result == ParseResult::kComplete)
    {
        printf("%.*s %.*s\n",
               static_cast<int>(req.method.size()), req.method.data(),
               static_cast<int>(req.path.size()), req.path.data());
        srv.router.dispatch(req, conn);
    }
    else
    {
        static constexpr std::string_view body = "{\"error\":\"bad request\"}";
        conn.write_len = static_cast<uint32_t>(
            build_response({conn.write_buf, sizeof(conn.write_buf)},
                           status_code::k400, "application/json", body, false));
        conn.keep_alive = false;
    }

    // Switch to write mode to send response
    epoll_event ev{};
    ev.events = EPOLLOUT;
    ev.data.fd = fd;
    epoll_ctl(srv.epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

static void on_accept(Server &srv) noexcept
{
    while (true)
    {
        int fd = accept(srv.listen_fd, nullptr, nullptr);
        if (fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            perror("accept");
            break;
        }

        int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        srv.conns[fd].fd = fd;

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(srv.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }
}

int main()
{
    Server srv;

    // --- Listen socket ---------------------------------------------------

    srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv.listen_fd < 0)
    {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(srv.listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        perror("bind");
        return 1;
    }
    if (listen(srv.listen_fd, BACKLOG) < 0)
    {
        perror("listen");
        return 1;
    }

    int flags = fcntl(srv.listen_fd, F_GETFL);
    fcntl(srv.listen_fd, F_SETFL, flags | O_NONBLOCK);

    // --- epoll setup -----------------------------------------------------

    srv.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv.epoll_fd < 0)
    {
        perror("epoll_create1");
        return 1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = srv.listen_fd;
    epoll_ctl(srv.epoll_fd, EPOLL_CTL_ADD, srv.listen_fd, &ev);

    // --- Route table -----------------------------------------------------

    srv.router.add_route("GET", "/ping", handle_ping);
    srv.router.add_route("POST", "/echo", handle_echo);

    printf("Listening on port %d\n", PORT);
    printf("Routes: GET /ping  POST /echo\n");

    // --- Event loop ------------------------------------------------------

    epoll_event events[MAX_EVENTS];

    while (true)
    {
        int n = epoll_wait(srv.epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0)
        {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            if (fd == srv.listen_fd)
            {
                on_accept(srv);
            }
            else if (events[i].events & EPOLLIN)
            {
                on_read(srv, fd, srv.conns[fd]);
            }
            else if (events[i].events & EPOLLOUT)
            {
                on_write(srv, fd, srv.conns[fd]);
            }
        }
    }

    close(srv.epoll_fd);
    close(srv.listen_fd);
    return 0;
}
