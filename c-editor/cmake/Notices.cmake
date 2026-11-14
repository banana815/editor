include_guard(GLOBAL)
if(NOT EDITOR_BUILD_APP)
  return()
endif()

set(EDITOR_NOTICES_DIR "${EDITOR_ASSETS_DIR}/licenses")
file(GLOB_RECURSE _editor_notice_sources CONFIGURE_DEPENDS
  LIST_DIRECTORIES FALSE "${CMAKE_CURRENT_LIST_DIR}/../licenses/*")
set(EDITOR_NOTICE_FILES)
foreach(source IN LISTS _editor_notice_sources)
  file(RELATIVE_PATH relative "${CMAKE_CURRENT_LIST_DIR}/../licenses" "${source}")
  set(destination "${EDITOR_NOTICES_DIR}/${relative}")
  get_filename_component(directory "${destination}" DIRECTORY)
  file(MAKE_DIRECTORY "${directory}")
  configure_file("${source}" "${destination}" COPYONLY)
  list(APPEND EDITOR_NOTICE_FILES "${destination}")
endforeach()
# Keep original contributor metadata with the compiled CSL resources, alongside
# the C dependencies' licenses and Unicode/schema provenance. All platforms use
# this same asset tree, including ordinary downloadable browser notices.
file(GLOB_RECURSE _editor_csl_notice_sources CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
  "${CMAKE_CURRENT_LIST_DIR}/../csl/licenses/*"
  "${CMAKE_CURRENT_LIST_DIR}/../csl/resources/*")
file(GLOB _editor_csl_data_notices CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
  "${CMAKE_CURRENT_LIST_DIR}/../csl/data/*-sources.json"
  "${CMAKE_CURRENT_LIST_DIR}/../csl/data/*-source.json"
  "${CMAKE_CURRENT_LIST_DIR}/../csl/data/UNICODE-LICENSE.txt")
list(APPEND _editor_csl_notice_sources ${_editor_csl_data_notices})
foreach(source IN LISTS _editor_csl_notice_sources)
  file(RELATIVE_PATH relative "${CMAKE_CURRENT_LIST_DIR}/../csl" "${source}")
  set(destination "${EDITOR_NOTICES_DIR}/csl/${relative}")
  get_filename_component(directory "${destination}" DIRECTORY)
  file(MAKE_DIRECTORY "${directory}")
  configure_file("${source}" "${destination}" COPYONLY)
  list(APPEND EDITOR_NOTICE_FILES "${destination}")
endforeach()
set(EDITOR_THIRD_PARTY_PATH "${EDITOR_ASSETS_DIR}/THIRD_PARTY.md")
configure_file("${CMAKE_CURRENT_LIST_DIR}/../THIRD_PARTY.md" "${EDITOR_THIRD_PARTY_PATH}" COPYONLY)
