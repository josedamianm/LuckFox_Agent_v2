cmake_minimum_required(VERSION 3.16)
project(luckfox_executables C)

set(CMAKE_C_STANDARD 99)

# SDK paths — matches /workspace/get_frame/Makefile in luckfox-crossdev container
set(SDK_MEDIA "/sdk/output/out/media_out" CACHE PATH "Path to media_out SDK directory")

set(SDK_INCLUDE "${SDK_MEDIA}/include")
set(SDK_LIB_DIR "${SDK_MEDIA}/lib")

message(STATUS "SDK_MEDIA    : ${SDK_MEDIA}")
message(STATUS "SDK_INCLUDE  : ${SDK_INCLUDE}")
message(STATUS "SDK_LIB_DIR  : ${SDK_LIB_DIR}")

if(NOT EXISTS "${SDK_INCLUDE}/rk_mpi_vi.h")
    message(FATAL_ERROR
        "Cannot find rk_mpi_vi.h at ${SDK_INCLUDE}.\n"
        "Pass the correct path: cmake -DSDK_MEDIA=/sdk/output/out/media_out ..."
    )
endif()

# ---- camera_daemon ----
add_executable(camera_daemon camera_daemon.c)

target_compile_definitions(camera_daemon PRIVATE RKAIQ)

target_include_directories(camera_daemon PRIVATE
    ${SDK_INCLUDE}
    ${SDK_INCLUDE}/rkaiq
    ${SDK_INCLUDE}/rkaiq/common
    ${SDK_INCLUDE}/rkaiq/uAPI2
    ${SDK_INCLUDE}/rkaiq/xcore
    ${SDK_INCLUDE}/rkaiq/algos
    ${SDK_INCLUDE}/rkaiq/iq_parser_v2
    ${SDK_INCLUDE}/rkaiq/iq_parser
    ${SDK_INCLUDE}/rockchip
)

target_link_options(camera_daemon PRIVATE
    -L${SDK_LIB_DIR}
    -Wl,-rpath-link,${SDK_LIB_DIR}
)

target_link_libraries(camera_daemon PRIVATE
    ${SDK_LIB_DIR}/librockit.so
    ${SDK_LIB_DIR}/librkaiq.so
    pthread
    m
    dl
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(camera_daemon PRIVATE -s)
endif()

install(TARGETS camera_daemon
    RUNTIME DESTINATION /root/Executables
)
