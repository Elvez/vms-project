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

add_executable(streamer
  src/streamer.cpp
)

target_include_directories(streamer PRIVATE
  ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(streamer PRIVATE
  ${FFMPEG_LIBRARIES}
)

target_compile_options(streamer PRIVATE
  ${FFMPEG_CFLAGS_OTHER}
)
