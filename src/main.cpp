#include "connection.hpp"
#include "http_parser.hpp"
#include "response.hpp"
#include "router.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cerrno>

static constexpr int PORT = 8080;
static constexpr int BACKLOG = 1024;   // pending connections queued before accept()
static constexpr int MAX_EVENTS = 256; // max events per epoll_wait()

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

// Register `events` for this fd, but only if it differs from what's already registered
static void arm(Server &srv, int fd, Connection &conn, uint32_t events) noexcept
{
    if (conn.epoll_events == events)
        return;
    conn.epoll_events = events;
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(srv.epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

// Attempt to send bytes in conn.write_buf without having to transition to EPOLLOUT
static void try_write(Server &srv, int fd, Connection &conn) noexcept
{
    while (conn.write_offset < conn.write_len)
    {
        ssize_t sent = send(fd,
                            conn.write_buf + conn.write_offset,
                            conn.write_len - conn.write_offset,
                            MSG_NOSIGNAL); // return EPIPE instead of raising SIGPIPE
        if (sent < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                arm(srv, fd, conn, EPOLLOUT); // finish later when writable
                return;
            }
            close_connection(srv, fd);
            return;
        }
        conn.write_offset += static_cast<uint32_t>(sent);
    }

    // full response flushed
    if (conn.keep_alive)
    {
        conn.reset();
        conn.fd = fd;
        arm(srv, fd, conn, EPOLLIN); // no-op if we never left read mode
    }
    else
    {
        close_connection(srv, fd);
    }
}

static void on_write(Server &srv, int fd, Connection &conn) noexcept
{
    try_write(srv, fd, conn);
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

    // try to send response without leaving EPOLLIN
    try_write(srv, fd, conn);
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

        // disable Nagle -> send small responses immediately
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        // set the new fd to EPOLLIN
        Connection &conn = srv.conns[fd];
        conn.fd = fd;
        conn.epoll_events = EPOLLIN;

        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(srv.epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    }
}

// Create a non-blocking listening socket bound to PORT, with SO_REUSEPORT
static int make_listener() noexcept
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, BACKLOG) < 0)
    {
        perror("listen");
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

// One worker = one listening socket + one epoll loop + its own connection map
static void run_worker()
{
    Server srv;

    srv.listen_fd = make_listener();
    if (srv.listen_fd < 0)
        return;

    srv.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv.epoll_fd < 0)
    {
        perror("epoll_create1");
        close(srv.listen_fd);
        return;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = srv.listen_fd;
    epoll_ctl(srv.epoll_fd, EPOLL_CTL_ADD, srv.listen_fd, &ev);

    srv.router.add_route("GET", "/ping", handle_ping);
    srv.router.add_route("POST", "/echo", handle_echo);

    epoll_event events[MAX_EVENTS];

    while (true)
    {
        int n = epoll_wait(srv.epoll_fd, events, MAX_EVENTS, -1);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
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
}

int main()
{
    // Worker count: $WORKERS, else one per hardware thread
    unsigned workers = 0;
    if (const char *env = std::getenv("WORKERS"))
        workers = static_cast<unsigned>(std::strtoul(env, nullptr, 10));
    if (workers == 0)
        workers = std::thread::hardware_concurrency();
    if (workers == 0)
        workers = 1;

    printf("Listening on port %d with %u worker threads\n", PORT, workers);
    printf("Routes: GET /ping  POST /echo\n");

    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned i = 0; i < workers; i++)
        pool.emplace_back(run_worker);
    for (auto &t : pool)
        t.join();

    return 0;
}
