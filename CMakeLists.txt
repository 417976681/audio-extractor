cmake_minimum_required(VERSION 3.16)
project(audio_extractor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_PLAYER "Build the web-UI music player (needs the bundled cpp-httplib)" ON)

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# FFmpeg location.
#   Prefer FFMPEG_ROOT (env or -DFFMPEG_ROOT=...). Defaults to the bundled
#   third_party/ffmpeg used for local development / CI.
# ---------------------------------------------------------------------------
if(NOT FFMPEG_ROOT)
  set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
endif()
message(STATUS "FFMPEG_ROOT = ${FFMPEG_ROOT}")

# Helper: locate the MinGW import library (*.dll.a), falling back to others.
macro(find_ff_lib VAR NAME)
  set(${VAR} "${FFMPEG_ROOT}/lib/lib${NAME}.dll.a")
  if(NOT EXISTS "${${VAR}}")
    find_file(${VAR} NAMES "lib${NAME}.dll.a" "lib${NAME}.lib"
              PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
  endif()
  if(NOT EXISTS "${${VAR}}")
    find_library(${VAR} NAMES ${NAME} PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
  endif()
  if(NOT EXISTS "${${VAR}}")
    message(FATAL_ERROR "Could not find FFmpeg library: ${NAME} (looked in ${FFMPEG_ROOT}/lib)")
  endif()
  message(STATUS "  ${NAME} -> ${${VAR}}")
endmacro()

find_ff_lib(FF_AVFORMAT  avformat)
find_ff_lib(FF_AVCODEC   avcodec)
find_ff_lib(FF_AVUTIL    avutil)
find_ff_lib(FF_SWRESAMPLE swresample)
find_ff_lib(FF_AVDEVICE  avdevice)    # sometimes needed by avformat
find_ff_lib(FF_SWSCALE   swscale)     # sometimes needed by avformat

set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
if(NOT EXISTS "${FFMPEG_INCLUDE_DIR}/libavformat/avformat.h")
  message(FATAL_ERROR "FFmpeg headers not found in ${FFMPEG_INCLUDE_DIR}")
endif()

# ---------------------------------------------------------------------------
# libcurl (MinGW).
# ---------------------------------------------------------------------------
if(WIN32)
  # Prefer a bundled curl in third_party/curl (avoids toolchain path issues on
  # native MinGW builds where /mingw64 is not reliably resolved).
  set(CURL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl")
  if(NOT EXISTS "${CURL_ROOT}/include/curl/curl.h")
    # Fall back to the MinGW toolchain location.
    if(NOT MINGW_ROOT)
      set(MINGW_ROOT "C:/msys64/mingw64")
      if(NOT EXISTS "${MINGW_ROOT}/lib")
        set(MINGW_ROOT "/mingw64")
      endif()
    endif()
    set(CURL_ROOT "${MINGW_ROOT}")
  endif()
  message(STATUS "CURL_ROOT = ${CURL_ROOT}")

  set(CURL_LIB "${CURL_ROOT}/lib/libcurl.dll.a")
  if(NOT EXISTS "${CURL_LIB}")
    find_file(CURL_LIB NAMES "libcurl.dll.a" "libcurl.lib"
              PATHS "${CURL_ROOT}/lib" NO_DEFAULT_PATH)
  endif()
  if(NOT EXISTS "${CURL_LIB}")
    find_library(CURL_LIB NAMES curl PATHS "${CURL_ROOT}/lib" NO_DEFAULT_PATH)
  endif()
  set(CURL_INCLUDE_DIR "${CURL_ROOT}/include")
  if(NOT EXISTS "${CURL_LIB}")
    message(FATAL_ERROR "libcurl not found under ${CURL_ROOT}/lib")
  endif()
  message(STATUS "  curl -> ${CURL_LIB}")

  # Runtime DLLs to copy next to the executable.
  file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
  file(GLOB CURL_DLL "${CURL_ROOT}/bin/*.dll")
else()
  # Linux / macOS: prefer pkg-config.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CURL   REQUIRED libcurl)
  pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil libswresample)
  set(CURL_LIB        "${CURL_LIBRARIES}")
  set(CURL_INCLUDE_DIR "${CURL_INCLUDE_DIRS}")
  set(FFMPEG_LIBS     "${FFMPEG_LIBRARIES}")
  set(FFMPEG_INCLUDE_DIR "${FFMPEG_INCLUDE_DIRS}")
endif()

# ===========================================================================
# audiocore — the reusable engine (network + site extractors + transcoding).
# Both the CLI and the music player link against this.
# ===========================================================================
add_library(audiocore STATIC
  src/common/common.h
  src/common/platform.h
  src/common/platform.cpp
  src/common/json.h
  src/common/json.cpp
  src/common/md5.h
  src/common/md5.cpp
  src/net/http_client.h
  src/net/http_client.cpp
  src/extract/extractor.h
  src/extract/direct_extractor.h
  src/extract/direct_extractor.cpp
  src/extract/hls_extractor.h
  src/extract/hls_extractor.cpp
  src/extract/registry.h
  src/extract/bilibili_extractor.h
  src/extract/bilibili_extractor.cpp
  src/extract/youtube_extractor.h
  src/extract/youtube_extractor.cpp
  src/extract/douyin_extractor.h
  src/extract/douyin_extractor.cpp
  src/audio/audio_extractor.h
  src/audio/audio_extractor.cpp
  src/library/music_library.h
  src/library/music_library.cpp
  src/library/lyrics.h
  src/library/lyrics.cpp
)

target_include_directories(audiocore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  "${FFMPEG_INCLUDE_DIR}"
  "${CURL_INCLUDE_DIR}"
)

if(WIN32)
  target_link_libraries(audiocore PUBLIC
    "${FF_AVFORMAT}"
    "${FF_AVCODEC}"
    "${FF_AVUTIL}"
    "${FF_SWRESAMPLE}"
    "${FF_AVDEVICE}"
    "${FF_SWSCALE}"
    "${CURL_LIB}"
    ws2_32
    iphlpapi
  )
else()
  target_link_libraries(audiocore PUBLIC ${FFMPEG_LIBS} ${CURL_LIB} Threads::Threads)
endif()

# Helper: copy FFmpeg/curl runtime DLLs next to a target's executable.
macro(copy_runtime_dlls TGT)
  foreach(dll ${FFMPEG_DLLS} ${CURL_DLL})
    if(dll)
      add_custom_command(TARGET ${TGT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}"
                "$<TARGET_FILE_DIR:${TGT}>"
        VERBATIM)
    endif()
  endforeach()
endmacro()

# ===========================================================================
# audio_extractor — command line front-end
# ===========================================================================
add_executable(audio_extractor src/main.cpp)
target_link_libraries(audio_extractor PRIVATE audiocore)
if(WIN32)
  copy_runtime_dlls(audio_extractor)
endif()

# ===========================================================================
# music_player — local web-UI music player (C++ HTTP server + static assets)
# ===========================================================================
if(BUILD_PLAYER)
  set(HTTPLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib")
  if(NOT EXISTS "${HTTPLIB_DIR}/httplib.h")
    message(FATAL_ERROR
      "third_party/httplib/httplib.h is missing. Run tools/fetch_deps.ps1 "
      "or download cpp-httplib into third_party/httplib to build music_player. "
      "You can also build -DBUILD_PLAYER=OFF for the CLI only.")
  endif()

  add_executable(music_player
    src/player_main.cpp
    src/server/player_server.h
    src/server/player_server.cpp
  )
  target_include_directories(music_player PRIVATE "${HTTPLIB_DIR}")
  target_link_libraries(music_player PRIVATE audiocore Threads::Threads)
  if(WIN32)
    target_link_libraries(music_player PRIVATE ws2_32)
    copy_runtime_dlls(music_player)
  endif()

  # Keep the web assets next to the player binary for easy distribution.
  #
  # Two mechanisms, deliberately:
  #   * POST_BUILD fires whenever music_player is relinked — it resolves the
  #     exe directory correctly even for multi-config generators.
  #   * the ALL target below runs on every build, so editing only HTML/CSS/JS
  #     (no relink) still refreshes what the server serves. It cannot use
  #     $<TARGET_FILE_DIR:music_player> because that would back-edge into
  #     music_player and create a dependency cycle.
  add_custom_command(TARGET music_player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/web" "$<TARGET_FILE_DIR:music_player>/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/extension" "$<TARGET_FILE_DIR:music_player>/extension"
    VERBATIM)

  add_custom_target(music_player_web_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/web" "${CMAKE_CURRENT_BINARY_DIR}/web"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/extension" "${CMAKE_CURRENT_BINARY_DIR}/extension"
    VERBATIM)
endif()
