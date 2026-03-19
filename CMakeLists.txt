cmake_minimum_required(VERSION 3.20)
project(Termind VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# nlohmann/json (header-only, via FetchContent)
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# readline
find_library(READLINE_LIB NAMES readline REQUIRED)
find_path(READLINE_INCLUDE_DIR NAMES readline/readline.h REQUIRED)

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/config.cpp
    src/utils.cpp
    src/skill_manager.cpp
    src/tool_registry.cpp
    src/ai_client.cpp
    src/context_manager.cpp
    src/repl.cpp
)

add_executable(termind ${SOURCES})

target_include_directories(termind PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${READLINE_INCLUDE_DIR}
)

target_link_libraries(termind PRIVATE
    CURL::libcurl
    Threads::Threads
    nlohmann_json::nlohmann_json
    ${READLINE_LIB}
)

target_compile_options(termind PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)

install(TARGETS termind DESTINATION bin)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
