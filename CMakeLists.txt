cmake_minimum_required(VERSION 2.6)

project(m2m-test C)

include(CheckIncludeFile)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG libavformat>=57.40 libavdevice libavcodec>=57.48 libswscale libavutil)
set(CMAKE_C_FLAGS "-std=gnu99 -Wall")
add_definitions(-DVERSION="v1.5")

if(FFMPEG_FOUND)
	include_directories(${FFMPEG_INCLUDE_DIRS})

	add_executable(m2m-test m2m-test.c log.c v4l2-utils.c m420.c)
	target_compile_definitions(m2m-test PRIVATE -D_FILE_OFFSET_BITS=64)
	target_link_libraries(m2m-test ${FFMPEG_LIBRARIES})

	add_executable(any2m420 any2m420.c log.c m420.c)
	target_link_libraries(any2m420 ${FFMPEG_LIBRARIES})

	install(TARGETS m2m-test any2m420 RUNTIME DESTINATION bin)
endif()

check_include_file(linux/dmabuf_exporter.h HAVE_DMABUF_EXPORTER_H)
if(HAVE_DMABUF_EXPORTER_H)
	add_definitions(-DDMABUFEXP)
endif()

pkg_check_modules(LIBDRM libdrm libkms)
if(LIBDRM_FOUND)
	include_directories(${LIBDRM_INCLUDE_DIRS})
	add_definitions(-DLIBDRM)
endif()

add_executable(cap-enc cap-enc.c log.c v4l2-utils.c)
target_compile_definitions(cap-enc PRIVATE -D_FILE_OFFSET_BITS=64)
add_executable(devbufbench log.c devbufbench.c v4l2-utils.c)
target_link_libraries(devbufbench ${LIBDRM_LIBRARIES})

install(TARGETS cap-enc devbufbench RUNTIME DESTINATION bin)
