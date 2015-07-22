cmake_minimum_required(VERSION 2.6)

project(m2m-test C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavformat libavdevice libavcodec libswscale libavutil)

include_directories(${FFMPEG_INCLUDE_DIRS})
add_executable(m2m-test m2m-test.c)
set_target_properties(m2m-test PROPERTIES COMPILE_FLAGS "-std=gnu99")
target_link_libraries(m2m-test ${FFMPEG_LIBRARIES})

install(TARGETS m2m-test RUNTIME DESTINATION bin)
