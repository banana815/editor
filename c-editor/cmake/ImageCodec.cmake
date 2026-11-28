include_guard(GLOBAL)
# The decoder is vendored verbatim so headless builds need no network fetch.
# Keep the complete upstream header and its selected MIT notice together.
set(_notelab_stb "${CMAKE_CURRENT_LIST_DIR}/../vendor/stb_image.h")
file(SHA256 "${_notelab_stb}" _notelab_stb_hash)
if(NOT _notelab_stb_hash STREQUAL "dfc2aadda6295f67f677587773f25e5bf8c8ad427ed0db7c518525c91e72e020")
  message(FATAL_ERROR "stb_image must match pinned upstream revision 6b5799bae8a1fe3792691ef4b72ae26434c3e799")
endif()
