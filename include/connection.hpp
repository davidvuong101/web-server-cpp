#pragma once

#include <cstdint>
#include <cstring>

static constexpr int READ_BUF_SZ = 4096;  // 4KB
static constexpr int WRITE_BUF_SZ = 2048; // 2KB

struct Connection
{
    // client socket fd
    int fd{-1};

    // incoming request buffer
    char read_buf[READ_BUF_SZ]{};
    uint32_t bytes_read{0};

    // outgoing response buffer
    char write_buf[WRITE_BUF_SZ]{};
    uint32_t write_len{0};
    uint32_t write_offset{0};

    bool keep_alive{false};

    // track registered event mask (EPOLLIN/EPOLLOUT) for this fd
    uint32_t epoll_events{0};

    void reset() noexcept
    {
        fd = -1;
        bytes_read = 0;
        write_len = 0;
        write_offset = 0;
        keep_alive = false;
    }
};