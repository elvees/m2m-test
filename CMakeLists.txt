cmake_minimum_required(VERSION 2.6)

project(m2m-test C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavformat libavdevice libavcodec libswscale libavutil)
include_directories(${FFMPEG_INCLUDE_DIRS})
include(CheckIncludeFile)
set(CMAKE_C_FLAGS "-std=gnu99 -Wall")
add_definitions(-DVERSION="v1.4")

add_executable(m2m-test m2m-test.c log.c v4l2-utils.c m420.c)
target_link_libraries(m2m-test ${FFMPEG_LIBRARIES})

add_executable(any2m420 any2m420.c log.c m420.c)
target_link_libraries(any2m420 ${FFMPEG_LIBRARIES})

add_executable(cap-enc cap-enc.c log.c v4l2-utils.c)

check_include_file(linux/hwsgbm.h HAVE_HWSGBM_H)

if(HAVE_HWSGBM_H)
	add_executable(hwsgbm-test hwsgbm-test.c log.c v4l2-utils.c)
	target_link_libraries(hwsgbm-test ${FFMPEG_LIBRARIES})
	install(TARGETS hwsgbm-test RUNTIME DESTINATION bin)
endif()

install(TARGETS m2m-test any2m420 cap-enc RUNTIME DESTINATION bin)
