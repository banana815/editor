include_guard(GLOBAL)
include(FetchContent)

# Build the same SQLite C source for desktop, mobile and WebAssembly. Opening
# Zotero databases is read-only in the application; the full API is retained
# so isolated test fixtures can be constructed without a separate SQL tool.
# Source and SHA3-256: https://sqlite.org/download.html (3.54.1).
FetchContent_Declare(editor_sqlite_source
  URL https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
  URL_HASH SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR editor-download-only
  TIMEOUT 180
  INACTIVITY_TIMEOUT 30
  TLS_VERIFY ON
)
FetchContent_MakeAvailable(editor_sqlite_source)
add_library(editor_sqlite STATIC "${editor_sqlite_source_SOURCE_DIR}/sqlite3.c")
set_target_properties(editor_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON C_EXTENSIONS ON)
target_include_directories(editor_sqlite PUBLIC "${editor_sqlite_source_SOURCE_DIR}")
target_compile_definitions(editor_sqlite PRIVATE SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_DQS=0)
if(EMSCRIPTEN)
  target_compile_definitions(editor_sqlite PRIVATE SQLITE_THREADSAFE=0)
else()
  find_package(Threads REQUIRED)
  target_compile_definitions(editor_sqlite PRIVATE SQLITE_THREADSAFE=1)
  target_link_libraries(editor_sqlite PUBLIC Threads::Threads)
endif()
if(UNIX)
  target_link_libraries(editor_sqlite PUBLIC m)
endif()
