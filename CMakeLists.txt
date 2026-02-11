cmake_minimum_required(VERSION 3.16)
project(streamer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
  libavformat
  libavcodec
  libavutil
  libswscale
)

find_library(AVFORMAT_LIB avformat HINTS ${FFMPEG_LIBRARY_DIRS})
find_library(AVCODEC_LIB avcodec HINTS ${FFMPEG_LIBRARY_DIRS})
find_library(AVUTIL_LIB avutil HINTS ${FFMPEG_LIBRARY_DIRS})
find_library(SWSCALE_LIB swscale HINTS ${FFMPEG_LIBRARY_DIRS})

add_executable(streamer
  src/streamer.cpp
)

target_include_directories(streamer PRIVATE
  ${FFMPEG_INCLUDE_DIRS}
)

target_link_directories(streamer PRIVATE
  ${FFMPEG_LIBRARY_DIRS}
)

target_link_libraries(streamer PRIVATE
  ${AVFORMAT_LIB}
  ${AVCODEC_LIB}
  ${AVUTIL_LIB}
  ${SWSCALE_LIB}
)

target_compile_options(streamer PRIVATE
  ${FFMPEG_CFLAGS_OTHER}
)
