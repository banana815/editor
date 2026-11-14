include_guard(GLOBAL)
include(FetchContent)

# The citation processor itself is C. These small C libraries provide XML and
# Unicode primitives; no JavaScript interpreter or C++ collation engine is used.
foreach(option BUILD_TOOLS BUILD_EXAMPLES BUILD_TESTS BUILD_DOCS BUILD_FUZZERS
               BUILD_PKGCONFIG ENABLE_INSTALL SHARED_LIBS DTD)
  set(EXPAT_${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(EXPAT_NS ON CACHE BOOL "" FORCE)
set(EXPAT_GE ON CACHE BOOL "" FORCE)
set(EXPAT_CHAR_TYPE char CACHE STRING "" FORCE)
if(MSVC)
  if(CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
    set(EXPAT_MSVC_STATIC_CRT ON CACHE BOOL "" FORCE)
  else()
    # Reset a previous static configuration when the same build tree switches
    # back to the default/dynamic CRT; do not mix runtimes across libraries.
    set(EXPAT_MSVC_STATIC_CRT OFF CACHE BOOL "" FORCE)
  endif()
endif()
FetchContent_Declare(editor_expat
  URL https://codeload.github.com/libexpat/libexpat/tar.gz/a70841d9ffa64ddde29ab824ed6305035ebe4c47
  URL_HASH SHA256=165b3fe7561caf01adef27b68587d6579166060b04e3e88561ec788f4be0e893
  SOURCE_SUBDIR expat DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  TIMEOUT 180 INACTIVITY_TIMEOUT 30 TLS_VERIFY ON)
FetchContent_MakeAvailable(editor_expat)
set_target_properties(expat PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(_csl_previous_shared "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(editor_utf8proc
  URL https://codeload.github.com/JuliaStrings/utf8proc/tar.gz/79cdd5ab40c79afa559f689ffc13b76812dee1ac
  URL_HASH SHA256=e8fadfcd531d65525cbef157b866c5408e2e3f0d7c0f3aefb0bce01a7a08e35d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  TIMEOUT 180 INACTIVITY_TIMEOUT 30 TLS_VERIFY ON)
FetchContent_MakeAvailable(editor_utf8proc)
set_target_properties(utf8proc PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS "${_csl_previous_shared}")

# Compile the verified C-only collation dependency closure. The upstream demo
# and unused next.c iterator callback route through JavaScript on Emscripten;
# neither is needed by collation. See csl/COLLATION.md and its source manifest.
FetchContent_Declare(editor_mojibake
  URL https://codeload.github.com/zaerl/mojibake/tar.gz/e397201f7f342e20ec0bc7cb45efee0a7cef34ec
  URL_HASH SHA256=7a90d0811c7cec45b514ad0085a6538d0a4526b6753dfd7ecc5cb5992be7ea34
  SOURCE_SUBDIR notelab-download-only DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  TIMEOUT 180 INACTIVITY_TIMEOUT 30 TLS_VERIFY ON)
FetchContent_MakeAvailable(editor_mojibake)
set(_csl_mojibake_sources)
foreach(unit buffer cjk codepoint collation encoding hangul mojibake normalization
             properties quick-check string unicode-tables)
  list(APPEND _csl_mojibake_sources "${editor_mojibake_SOURCE_DIR}/src/${unit}.c")
endforeach()
add_library(notelab_mojibake STATIC ${_csl_mojibake_sources})
set_target_properties(notelab_mojibake PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(notelab_mojibake PUBLIC "${editor_mojibake_SOURCE_DIR}/src")
target_compile_definitions(notelab_mojibake PRIVATE
  MJB_FEATURE_COLLATION=1 MJB_FEATURE_CHARACTER_NAMES=0
  MJB_FEATURE_IDNA=0 MJB_FEATURE_SECURITY=0)
if(MSVC)
  target_compile_options(notelab_mojibake PRIVATE /utf-8)
elseif(CSL_SANITIZERS)
  target_compile_options(notelab_mojibake PRIVATE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(notelab_mojibake PUBLIC -fsanitize=address,undefined)
endif()
