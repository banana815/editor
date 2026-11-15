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
file(GLOB _ndk_notices LIST_DIRECTORIES FALSE "${CMAKE_ANDROID_NDK}/NOTICE*")
if(NOT _ndk_notices)
  message(FATAL_ERROR "The Android NDK's runtime license notices are missing")
endif()
add_custom_target(notelab_android_assets
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_assets}/licenses/android-ndk"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/editor/assets/font.otf" "${_assets}/font.otf"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/editor/assets/OFL.txt" "${_assets}/OFL.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/editor/assets/emoji.ttf" "${_assets}/emoji.ttf"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/editor/assets/OFL-Emoji.txt" "${_assets}/OFL-Emoji.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/editor/assets/THIRD_PARTY.md" "${_assets}/THIRD_PARTY.md"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/editor/assets/licenses" "${_assets}/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ndk_notices} "${_assets}/licenses/android-ndk"
  VERBATIM
)
add_dependencies(notelab notelab_android_assets)
