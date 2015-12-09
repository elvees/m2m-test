cmake_minimum_required(VERSION 2.6)

project(m2m-test C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavformat libavdevice libavcodec libswscale libavutil)
include_directories(${FFMPEG_INCLUDE_DIRS})

set(CMAKE_C_FLAGS "-std=gnu99 -Wall")
add_definitions(-DVERSION="v1.1")

add_executable(m2m-test m2m-test.c m420.c)
target_link_libraries(m2m-test ${FFMPEG_LIBRARIES})

add_executable(any2m420 any2m420.c m420.c)
target_link_libraries(any2m420 ${FFMPEG_LIBRARIES})

install(TARGETS m2m-test any2m420 RUNTIME DESTINATION bin)
