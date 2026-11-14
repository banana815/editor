cmake_minimum_required(VERSION 3.24)
project(NoteLabAndroid LANGUAGES C CXX)
if(NOT ANDROID)
  message(FATAL_ERROR "Build this project through Android Gradle and the NDK")
endif()
# editor_core and all its static dependencies are linked into libmain.so.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(EDITOR_USE_SYSTEM_SDL OFF CACHE BOOL "" FORCE)
add_subdirectory(../../.. editor)
if(NOT NOTELAB_ANDROID_ASSETS_DIR)
  message(FATAL_ERROR "Gradle must supply NOTELAB_ANDROID_ASSETS_DIR")
endif()
set(_assets "${NOTELAB_ANDROID_ASSETS_DIR}/${ANDROID_ABI}")
add_custom_target(notelab_android_assets
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_assets}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/editor/assets/font.otf" "${_assets}/font.otf"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/editor/assets/OFL.txt" "${_assets}/OFL.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/editor/assets/emoji.ttf" "${_assets}/emoji.ttf"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/editor/assets/OFL-Emoji.txt" "${_assets}/OFL-Emoji.txt"
  VERBATIM
)
add_dependencies(notelab notelab_android_assets)
