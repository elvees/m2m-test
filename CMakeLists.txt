cmake_minimum_required(VERSION 3.6)

project(M2MTest C)

include(CheckIncludeFile)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFmpeg IMPORTED_TARGET
    libavformat>=57.40 libavdevice libavcodec>=57.48 libswscale libavutil libswresample)
pkg_check_modules(LibDRM IMPORTED_TARGET libdrm libkms)
pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
check_include_file(linux/dmabuf_exporter.h HAVE_DMABUF_EXPORTER_H)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED TRUE)
add_definitions(-DVERSION="v1.6")

if(TARGET PkgConfig::FFmpeg)
    add_executable(m2m-test m2m-test.c log.c v4l2-utils.c m420.c)
    target_compile_definitions(m2m-test PRIVATE -D_FILE_OFFSET_BITS=64)
    target_link_libraries(m2m-test PkgConfig::FFmpeg)

    add_executable(any2m420 any2m420.c log.c m420.c)
    target_compile_definitions(any2m420 PRIVATE -D_FILE_OFFSET_BITS=64)
    target_link_libraries(any2m420 PkgConfig::FFmpeg)

    install(TARGETS m2m-test any2m420 RUNTIME DESTINATION bin)

    if(TARGET PkgConfig::SDL2 AND TARGET PkgConfig::LibDRM)
        add_executable(dec-vpout dec-vpout.c log.c v4l2-utils.c drmdisplay.c)
        target_compile_definitions(dec-vpout PRIVATE -D_FILE_OFFSET_BITS=64)
        target_link_libraries(dec-vpout PkgConfig::FFmpeg PkgConfig::LibDRM PkgConfig::SDL2
                                        pthread)
        install(TARGETS dec-vpout RUNTIME DESTINATION bin)
    endif()
endif()

add_executable(cap-enc cap-enc.c log.c v4l2-utils.c)
target_compile_definitions(cap-enc PRIVATE -D_FILE_OFFSET_BITS=64)

add_executable(devbufbench log.c devbufbench.c v4l2-utils.c)

if(TARGET PkgConfig::LibDRM)
    target_compile_definitions(devbufbench PRIVATE -DLIBDRM)
    target_link_libraries(devbufbench PkgConfig::LibDRM)
endif()

if(HAVE_DMABUF_EXPORTER_H)
    target_compile_definitions(devbufbench PRIVATE -DDMABUFEXP)
endif()

install(TARGETS cap-enc devbufbench RUNTIME DESTINATION bin)
