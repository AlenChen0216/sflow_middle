cmake_minimum_required(VERSION 3.10)
project(SmartNIC-Sflow-Decoder LANGUAGES CXX)

enable_testing()

find_package(yaml-cpp REQUIRED)

set(SPDLOG_DEBUG_POSTFIX "" CACHE STRING "" FORCE)
add_subdirectory(./lib)
add_subdirectory(./lib2/json)

find_package(fmt REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

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
target_link_libraries(${EXEC_NAME} PRIVATE Threads::Threads Boost::system yaml-cpp spdlog::spdlog fmt::fmt nlohmann_json::nlohmann_json ${NETRONOME_LIBS})
set_target_properties(${EXEC_NAME} PROPERTIES
	BUILD_RPATH "${NETRONOME_ROOT}/lib"
	INSTALL_RPATH "${NETRONOME_ROOT}/lib"
)
target_compile_options(${EXEC_NAME} PRIVATE -Wall -Wextra -fdiagnostics-color=always -O3)

add_executable(nfp_reader_test test/main.cpp)
target_include_directories(nfp_reader_test PRIVATE "${NETRONOME_INCLUDE_DIR}")
target_link_libraries(nfp_reader_test PRIVATE ${NETRONOME_LIBS})
set_target_properties(nfp_reader_test PROPERTIES
	OUTPUT_NAME "test"
	BUILD_RPATH "${NETRONOME_ROOT}/lib"
	INSTALL_RPATH "${NETRONOME_ROOT}/lib"
)
target_compile_options(nfp_reader_test PRIVATE -Wall -Wextra -fdiagnostics-color=always -O3)

add_executable(time_calibration_test
	test/time_calibration_test.cpp
	src/Time.cpp
	src/MacClockReader.cpp
	src/ClockSampler.cpp
)
target_include_directories(time_calibration_test PRIVATE
	include/
	"${NETRONOME_INCLUDE_DIR}"
)
target_link_libraries(time_calibration_test PRIVATE ${NETRONOME_LIBS})
set_target_properties(time_calibration_test PROPERTIES
	BUILD_RPATH "${NETRONOME_ROOT}/lib"
	INSTALL_RPATH "${NETRONOME_ROOT}/lib"
)
target_compile_options(time_calibration_test PRIVATE
	-Wall -Wextra -fdiagnostics-color=always -O2
)
add_test(NAME time_calibration COMMAND time_calibration_test)

add_executable(clock_sampler_test
	test/clock_sampler_test.cpp
	src/ClockSampler.cpp
)
target_include_directories(clock_sampler_test PRIVATE include/)
target_compile_options(clock_sampler_test PRIVATE
	-Wall -Wextra -fdiagnostics-color=always -O2
)
add_test(NAME clock_sampler COMMAND clock_sampler_test)

add_executable(raw_clock_probe
	tools/raw_clock_probe.cpp
	src/Device.cpp
	src/MacClockReader.cpp
	src/ClockSampler.cpp
)
target_include_directories(raw_clock_probe PRIVATE
	include/
	"${NETRONOME_INCLUDE_DIR}"
)
target_link_libraries(raw_clock_probe PRIVATE ${NETRONOME_LIBS})
set_target_properties(raw_clock_probe PROPERTIES
	BUILD_RPATH "${NETRONOME_ROOT}/lib"
	INSTALL_RPATH "${NETRONOME_ROOT}/lib"
)
target_compile_options(raw_clock_probe PRIVATE
	-Wall -Wextra -fdiagnostics-color=always -O2
)
