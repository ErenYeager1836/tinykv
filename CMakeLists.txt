cmake_minimum_required(VERSION 3.20)
project(tinykv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for language servers
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build configuration options
option(ENABLE_SANITIZERS "Enable Address and UndefinedBehavior sanitizers" OFF)
option(ENABLE_WERROR "Treat warnings as errors" ON)
option(BUILD_TESTS "Build test suite" ON)

# Strict warnings and language dialect flags
set(TINYKV_COMMON_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -fno-exceptions
    -fno-rtti
)

if(ENABLE_WERROR)
    list(APPEND TINYKV_COMMON_FLAGS -Werror)
endif()

if(ENABLE_SANITIZERS)
    list(APPEND TINYKV_COMMON_FLAGS
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    set(TINYKV_LINK_FLAGS -fsanitize=address,undefined)
endif()

# Core server executable
add_executable(tinykv
    src/main.cpp
    src/server.cpp
)

target_compile_options(tinykv PRIVATE ${TINYKV_COMMON_FLAGS})
if(TINYKV_LINK_FLAGS)
    target_link_options(tinykv PRIVATE ${TINYKV_LINK_FLAGS})
endif()
target_include_directories(tinykv PRIVATE src)

# Tests
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
