cmake_minimum_required(VERSION 3.14)
project(web_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)

# for JSON parsing
find_package(nlohmann_json 3.2.0 REQUIRED)

add_executable(server
    src/main.cpp
    src/http_parser.cpp
    src/response.cpp
    src/router.cpp
)
target_include_directories(server PRIVATE include)
target_link_libraries(server PRIVATE nlohmann_json::nlohmann_json)