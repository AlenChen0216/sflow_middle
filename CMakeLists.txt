cmake_minimum_required(VERSION 3.10)
project(SmartNIC-Sflow-Decoder LANGUAGES CXX)

find_package(yaml-cpp REQUIRED)

set(SPDLOG_DEBUG_POSTFIX "" CACHE STRING "" FORCE)
add_subdirectory(./lib/spdlog)

find_package(fmt REQUIRED)
find_package(Threads REQUIRED)

set(EXEC_NAME "MID")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)

add_executable(${EXEC_NAME} ${SOURCES})
target_include_directories(${EXEC_NAME} PRIVATE include/)
target_link_libraries(${EXEC_NAME} PRIVATE Threads::Threads yaml-cpp spdlog::spdlog fmt::fmt )
target_compile_options(${EXEC_NAME} PRIVATE -Wall -Wextra -fdiagnostics-color=always -O3)