include_guard(GLOBAL)
include(FetchContent)

# The application is C. HarfBuzz is a C API implemented in C++, and SDL uses
# native platform glue internally; these do not introduce another app runtime.
# The JSON dependency is also needed by headless core tests.
set(CJSON_OVERRIDE_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UNINSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_CUSTOM_COMPILER_FLAGS OFF CACHE BOOL "" FORCE)
set(ENABLE_LOCALES OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(editor_cjson
  URL https://codeload.github.com/DaveGamble/cJSON/tar.gz/refs/tags/v1.7.19
  URL_HASH SHA256=7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  TIMEOUT 180
  INACTIVITY_TIMEOUT 30
  TLS_VERIFY ON
)
FetchContent_MakeAvailable(editor_cjson)
set_target_properties(cjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(cjson PUBLIC "$<BUILD_INTERFACE:${editor_cjson_SOURCE_DIR}>")

if(NOT EDITOR_BUILD_APP)
  return()
endif()

option(EDITOR_USE_SYSTEM_SDL "Use a compatible installed desktop SDL3" ON)
# Never accidentally link the host's SDL into a mobile or WebAssembly build.
if(EDITOR_USE_SYSTEM_SDL AND NOT CMAKE_CROSSCOMPILING AND NOT ANDROID AND NOT EMSCRIPTEN)
  find_package(SDL3 3.2.6 QUIET CONFIG)
endif()

if(NOT TARGET SDL3::SDL3)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  # SDLActivity loads SDL3 as a shared library on Android. Static linkage keeps
  # the other distributions independent of preinstalled SDL libraries.
  if(ANDROID)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
  endif()
  FetchContent_Declare(editor_sdl
    URL https://codeload.github.com/libsdl-org/SDL/tar.gz/f00dc919ca35b9756316e86c4777392de442f9b2
    URL_HASH SHA256=e6a9c4e75de8fe719f922dddc208627f77754a502909f457a1355981f20c21e0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TIMEOUT 180
    INACTIVITY_TIMEOUT 30
    TLS_VERIFY ON
  )
  FetchContent_MakeAvailable(editor_sdl)
endif()

# SDL_ttf requires its linkage to match SDL's shared/static interface property.
# For example, Homebrew supplies shared SDL3, while our web build uses static SDL.
get_target_property(_editor_sdl_shared SDL3::SDL3 INTERFACE_SDL3_SHARED)
set(_editor_previous_build_shared_libs "${BUILD_SHARED_LIBS}")
if(_editor_sdl_shared)
  set(BUILD_SHARED_LIBS ON)
else()
  set(BUILD_SHARED_LIBS OFF)
endif()
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ ON CACHE BOOL "" FORCE)
set(SDLTTF_STRICT ON CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
# The exact release-3.2.2 commit and its recorded submodule revisions. Archive
# downloads avoid cloning FreeType/HarfBuzz histories and make hashes auditable.
# SOURCE_SUBDIR deliberately names a nonexistent directory so FetchContent only
# populates these trees; SDL_ttf configures its bundled dependencies below.
FetchContent_Declare(editor_sdl_ttf
  URL https://codeload.github.com/libsdl-org/SDL_ttf/tar.gz/a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b
  URL_HASH SHA256=5024f953a5ce207e87ae73e09644ec3e5357d9907952209035b88bdaeac7edeb
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR editor-download-only
  TIMEOUT 180
  INACTIVITY_TIMEOUT 30
  TLS_VERIFY ON
)
FetchContent_MakeAvailable(editor_sdl_ttf)
FetchContent_Declare(editor_freetype
  URL https://codeload.github.com/libsdl-org/freetype/tar.gz/9973564cfa63763a3e4ac67c09147899539b1e07
  URL_HASH SHA256=026a05a49d114a1235d2926f4c03a9330e4b1a6efe7c217ec9607904c32907d4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_DIR "${editor_sdl_ttf_SOURCE_DIR}/external/freetype"
  SOURCE_SUBDIR editor-download-only
  TIMEOUT 180
  INACTIVITY_TIMEOUT 30
  TLS_VERIFY ON
)
FetchContent_Declare(editor_harfbuzz
  URL https://codeload.github.com/libsdl-org/harfbuzz/tar.gz/564bf9818a18709776856533829c0c04950773d6
  URL_HASH SHA256=a448dd6c22d8e1e1cf39438c662251c1f97f810b8780eed4a6d6ada948c99ddc
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_DIR "${editor_sdl_ttf_SOURCE_DIR}/external/harfbuzz"
  SOURCE_SUBDIR editor-download-only
  TIMEOUT 180
  INACTIVITY_TIMEOUT 30
  TLS_VERIFY ON
)
FetchContent_MakeAvailable(editor_freetype editor_harfbuzz)
add_subdirectory("${editor_sdl_ttf_SOURCE_DIR}" "${editor_sdl_ttf_BINARY_DIR}" EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS "${_editor_previous_build_shared_libs}")

# A bundled CJK font makes Chinese and Latin text work on every target without
# relying on a machine-specific font path. Only this one font is downloaded.
set(EDITOR_ASSETS_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")
file(MAKE_DIRECTORY "${EDITOR_ASSETS_DIR}")
set(_editor_default_font "${EDITOR_ASSETS_DIR}/NotoSansCJKsc-Regular.otf")
set(_editor_font_sha256 "2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b")
set(EDITOR_FONT_PATH "${_editor_default_font}"
  CACHE FILEPATH "Bundled font, or a local font file for offline builds")

if("${EDITOR_FONT_PATH}" STREQUAL "${_editor_default_font}")
  # Existing files may be partial downloads from an interrupted configure.
  # Validate the bundled font on every configure before allowing it into a build.
  set(_editor_font_valid FALSE)
  if(EXISTS "${_editor_default_font}" AND NOT IS_DIRECTORY "${_editor_default_font}")
    file(SHA256 "${_editor_default_font}" _editor_existing_font_sha256)
    if("${_editor_existing_font_sha256}" STREQUAL "${_editor_font_sha256}")
      set(_editor_font_valid TRUE)
    endif()
  endif()

  if(NOT _editor_font_valid)
    set(_editor_font_partial "${_editor_default_font}.part")
    # Both endpoints serve the same immutable commit. The Contents API provides
    # a fallback when a network cannot reach raw.githubusercontent.com reliably.
    set(_editor_font_sources
      https://raw.githubusercontent.com/notofonts/noto-cjk/523d033d6cb47f4a80c58a35753646f5c3608a78/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf
      https://api.github.com/repos/notofonts/noto-cjk/contents/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf?ref=523d033d6cb47f4a80c58a35753646f5c3608a78
    )
    set(_editor_font_errors "")
    message(STATUS "Downloading Noto Sans CJK SC (16 MB, SIL Open Font License)")
    foreach(_editor_font_source IN LISTS _editor_font_sources)
      file(REMOVE "${_editor_font_partial}")
      set(_editor_font_headers)
      if(_editor_font_source MATCHES "^https://api\\.github\\.com/")
        set(_editor_font_headers HTTPHEADER "Accept: application/vnd.github.raw+json")
      endif()
      file(DOWNLOAD "${_editor_font_source}" "${_editor_font_partial}"
        ${_editor_font_headers}
        TLS_VERIFY ON
        TIMEOUT 180
        INACTIVITY_TIMEOUT 30
        STATUS _editor_font_download
      )
      list(GET _editor_font_download 0 _editor_font_status)
      if(NOT _editor_font_status EQUAL 0)
        string(APPEND _editor_font_errors "\n${_editor_font_source}: ${_editor_font_download}")
        file(REMOVE "${_editor_font_partial}")
        continue()
      endif()

      # Check manually so failed verification can clean up its temporary file
      # instead of file(DOWNLOAD EXPECTED_HASH) aborting before cleanup/fallback.
      file(SHA256 "${_editor_font_partial}" _editor_downloaded_font_sha256)
      if("${_editor_downloaded_font_sha256}" STREQUAL "${_editor_font_sha256}")
        set(_editor_font_valid TRUE)
        break()
      endif()
      string(APPEND _editor_font_errors "\n${_editor_font_source}: SHA256 verification failed")
      file(REMOVE "${_editor_font_partial}")
    endforeach()
    if(NOT _editor_font_valid)
      message(FATAL_ERROR "Noto font download failed:${_editor_font_errors}\nRetry configuration or set EDITOR_FONT_PATH to a local font to build offline.")
    endif()
    file(RENAME "${_editor_font_partial}" "${_editor_default_font}" RESULT _editor_font_rename)
    if(NOT "${_editor_font_rename}" STREQUAL "0")
      file(REMOVE "${_editor_font_partial}")
      message(FATAL_ERROR "Cannot install verified Noto font: ${_editor_font_rename}")
    endif()
  endif()
else()
  # A user-supplied font need not be Noto and must never be overwritten.
  if(NOT EXISTS "${EDITOR_FONT_PATH}" OR IS_DIRECTORY "${EDITOR_FONT_PATH}")
    message(FATAL_ERROR "EDITOR_FONT_PATH must name an existing file: ${EDITOR_FONT_PATH}")
  endif()
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/../assets/OFL.txt"
  "${EDITOR_ASSETS_DIR}/OFL.txt" COPYONLY)
