# Web Server

A minimal single-threaded HTTP/1.1 server in C++20 using Linux `epoll`.

## Build

**Dependencies:** CMake 3.14+, a C++20 compiler, `nlohmann-json` 3.2+

```bash
# Install nlohmann-json on Ubuntu
sudo apt install nlohmann-json3-dev

cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/server
# Listening on port 8080
```

### Routes

| Method | Path    | Description                          |
| ------ | ------- | ------------------------------------ |
| GET    | `/ping` | Returns `pong`                       |
| POST   | `/echo` | Echoes the request body back as JSON |

```bash
curl http://localhost:8080/ping
curl -X POST http://localhost:8080/echo -d '{"hello":"world"}'
```
