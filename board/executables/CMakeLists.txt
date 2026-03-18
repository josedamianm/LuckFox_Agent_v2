cmake_minimum_required(VERSION 3.16)
project(luckfox_executables C)

set(CMAKE_C_STANDARD 99)

# ---- Locate Rockchip SDK headers ----
# The luckfox-crossdev container ships the media SDK headers.
# Common locations (try in order):
#   1. LUCKFOX_SDK env var  → ${LUCKFOX_SDK}/sysdrv/source/media/rockit/include
#   2. Sysroot usr/include  → already searched by cmake via CMAKE_SYSROOT
#   3. /luckfox/...         → container-local install

if(DEFINED ENV{LUCKFOX_SDK})
    set(_SDK "$ENV{LUCKFOX_SDK}")
    set(_SDK_MEDIA_CANDIDATES
        "${_SDK}/sysdrv/source/media/rockit/include"
        "${_SDK}/media/include"
        "${_SDK}/sysdrv/source/media/rkaiq/include"
    )
    set(_SDK_RKAIQ_CANDIDATES
        "${_SDK}/sysdrv/source/media/rkaiq/include"
        "${_SDK}/sysdrv/source/media/rkaiq/include/uAPI2"
    )
    set(_SDK_LIB_CANDIDATES
        "${_SDK}/output/out/oem/usr/lib"
        "${_SDK}/output/oem/usr/lib"
    )
else()
    set(_SDK_MEDIA_CANDIDATES
        "/luckfox/media/include"
        "/luckfox/sysdrv/source/media/rockit/include"
        "/opt/luckfox/media/include"
    )
    set(_SDK_RKAIQ_CANDIDATES
        "/luckfox/media/rkaiq/include"
        "/luckfox/sysdrv/source/media/rkaiq/include"
        "/opt/luckfox/rkaiq/include"
    )
    set(_SDK_LIB_CANDIDATES
        "/luckfox/output/oem/usr/lib"
        "/opt/luckfox/lib"
    )
endif()

find_path(SDK_MEDIA_INCLUDE
    NAMES rk_mpi_vi.h
    PATHS ${_SDK_MEDIA_CANDIDATES}
    NO_DEFAULT_PATH
)

find_path(SDK_RKAIQ_INCLUDE
    NAMES rk_aiq_user_api2_sysctl.h
    PATHS ${_SDK_RKAIQ_CANDIDATES}
          "${SDK_MEDIA_INCLUDE}/../rkaiq/include"
          "${SDK_MEDIA_INCLUDE}"
    PATH_SUFFIXES uAPI2
    NO_DEFAULT_PATH
)

find_library(LIB_ROCKIT
    NAMES rockit
    PATHS ${_SDK_LIB_CANDIDATES}
    NO_DEFAULT_PATH
)

find_library(LIB_RKAIQ
    NAMES rkaiq
    PATHS ${_SDK_LIB_CANDIDATES}
    NO_DEFAULT_PATH
)

if(NOT SDK_MEDIA_INCLUDE)
    message(FATAL_ERROR
        "Could not find rk_mpi_vi.h. "
        "Set LUCKFOX_SDK env var to your SDK root, or set SDK_MEDIA_INCLUDE manually.\n"
        "Example: cmake -DSDK_MEDIA_INCLUDE=/luckfox/media/include ..."
    )
endif()

if(NOT LIB_ROCKIT)
    message(FATAL_ERROR
        "Could not find librockit. "
        "Set LUCKFOX_SDK env var or pass -DLIB_ROCKIT=/path/to/librockit.so"
    )
endif()

get_filename_component(SDK_LIB_DIR "${LIB_ROCKIT}" DIRECTORY)

message(STATUS "SDK media include : ${SDK_MEDIA_INCLUDE}")
message(STATUS "SDK rkaiq include : ${SDK_RKAIQ_INCLUDE}")
message(STATUS "SDK lib dir       : ${SDK_LIB_DIR}")

# ---- camera_daemon ----
add_executable(camera_daemon camera_daemon.c)

target_include_directories(camera_daemon PRIVATE
    ${SDK_MEDIA_INCLUDE}
    ${SDK_RKAIQ_INCLUDE}
    ${SDK_RKAIQ_INCLUDE}/uAPI2
    ${SDK_RKAIQ_INCLUDE}/algos
    ${SDK_RKAIQ_INCLUDE}/xcore
    ${SDK_RKAIQ_INCLUDE}/common
)

target_link_libraries(camera_daemon PRIVATE
    ${LIB_ROCKIT}
    ${LIB_RKAIQ}
    pthread
    m
)

target_link_options(camera_daemon PRIVATE
    -L${SDK_LIB_DIR}
    -Wl,-rpath-link,${SDK_LIB_DIR}
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(camera_daemon PRIVATE -s)
endif()

install(TARGETS camera_daemon
    RUNTIME DESTINATION /root/Executables
)
