include_guard(GLOBAL)
if(NOT EDITOR_BUILD_APP OR ANDROID OR EMSCRIPTEN OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(FATAL_ERROR "EDITOR_DISTRIBUTION is for desktop application builds only")
endif()
foreach(dependency SDL3::SDL3 SDL3_ttf::SDL3_ttf)
  get_target_property(dependency_type ${dependency} TYPE)
  if(NOT dependency_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "Desktop archives require a static ${dependency}; configure a fresh build directory")
  endif()
endforeach()

install(TARGETS notelab RUNTIME DESTINATION . COMPONENT NoteLab
  BUNDLE DESTINATION . COMPONENT NoteLab)
if(NOT APPLE)
  install(FILES "${EDITOR_ASSETS_DIR}/font.otf" "${EDITOR_ASSETS_DIR}/OFL.txt"
    "${EDITOR_EMOJI_BUNDLED_PATH}" "${EDITOR_EMOJI_LICENSE_PATH}"
    DESTINATION assets COMPONENT NoteLab)
endif()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
  "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md" DESTINATION . COMPONENT NoteLab)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/licenses/"
  DESTINATION licenses COMPONENT NoteLab)

set(CPACK_PACKAGE_NAME "NoteLab")
set(CPACK_PACKAGE_VENDOR "NoteLab contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "C note editor — development preview")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/packages")
set(_editor_package_arch "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
  string(REPLACE ";" "-" _editor_package_arch "${CMAKE_OSX_ARCHITECTURES}")
endif()
set(CPACK_PACKAGE_FILE_NAME "NoteLab-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${_editor_package_arch}")
set(CPACK_GENERATOR "ZIP")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(CPACK_GENERATOR "TGZ")
endif()
# Package only the app's install component, never dependency development files.
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};${PROJECT_NAME};NoteLab;/")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_VERBATIM_VARIABLES ON)
include(CPack)
