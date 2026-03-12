cmake_minimum_required(VERSION 3.16)
project(luckfox_gui C)

set(CMAKE_C_STANDARD 99)

# LVGL configuration
set(LV_CONF_PATH "${CMAKE_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)

# Add LVGL library (must exist as git submodule at lib/lvgl)
add_subdirectory(lib/lvgl)

# Gather all application sources
file(GLOB_RECURSE APP_SOURCES
    "src/*.c"
)

add_executable(luckfox_gui ${APP_SOURCES})

target_include_directories(luckfox_gui PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lib/lvgl
    ${CMAKE_SOURCE_DIR}/lib/lvgl/src
)

target_link_libraries(luckfox_gui PRIVATE
    lvgl
    pthread
    m
)

# Strip symbols for minimal binary size on target
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_link_options(luckfox_gui PRIVATE -s)
endif()

# Host simulator build: add SDL2 backend when not cross-compiling
if(SW_SIMULATOR)
    find_package(SDL2 REQUIRED)
    target_include_directories(luckfox_gui PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(luckfox_gui PRIVATE ${SDL2_LIBRARIES})
    target_compile_definitions(luckfox_gui PRIVATE SW_SIMULATOR=1)
endif()

install(TARGETS luckfox_gui
    RUNTIME DESTINATION /root/Executables
)
