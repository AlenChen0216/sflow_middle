cmake_minimum_required(VERSION 3.10)
project(SmartNIC-Sflow-Decoder LANGUAGES CXX)

find_package(yaml-cpp REQUIRED)

set(SPDLOG_DEBUG_POSTFIX "" CACHE STRING "" FORCE)
add_subdirectory(./lib)
add_subdirectory(./lib2/json)

find_package(fmt REQUIRED)
find_package(Threads REQUIRED)

set(EXEC_NAME "MID")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)

set(NETRONOME_ROOT "/opt/netronome" CACHE PATH "Path to Netronome SDK root")

find_path(NETRONOME_INCLUDE_DIR
	NAMES nfp.h
	PATHS "${NETRONOME_ROOT}/include"
	REQUIRED
)

find_library(NETRONOME_NFP_LIB
	NAMES nfp
	PATHS "${NETRONOME_ROOT}/lib"
	REQUIRED
)

# Optional: some SDK builds split symbols into this companion library.
find_library(NETRONOME_NFP_COMMON_LIB
	NAMES nfp_common
	PATHS "${NETRONOME_ROOT}/lib"
)

set(NETRONOME_LIBS "${NETRONOME_NFP_LIB}")
if(NETRONOME_NFP_COMMON_LIB)
	list(APPEND NETRONOME_LIBS "${NETRONOME_NFP_COMMON_LIB}")
endif()

add_executable(${EXEC_NAME} ${SOURCES})
target_include_directories(${EXEC_NAME} PRIVATE include/)
target_include_directories(${EXEC_NAME} PRIVATE "${NETRONOME_INCLUDE_DIR}")
target_link_libraries(${EXEC_NAME} PRIVATE Threads::Threads yaml-cpp spdlog::spdlog fmt::fmt nlohmann_json::nlohmann_json ${NETRONOME_LIBS})
set_target_properties(${EXEC_NAME} PROPERTIES
	BUILD_RPATH "${NETRONOME_ROOT}/lib"
	INSTALL_RPATH "${NETRONOME_ROOT}/lib"
)
target_compile_options(${EXEC_NAME} PRIVATE -Wall -Wextra -fdiagnostics-color=always -O3)