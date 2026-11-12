find_package(otter CONFIG REQUIRED)

file(GLOB _src CONFIGURE_DEPENDS *.cpp *.h)
add_library(dssp_extraction STATIC ${_src})

target_compile_features(dssp_extraction PUBLIC cxx_std_23)
target_include_directories(dssp_extraction PUBLIC "$<BUILD_INTERFACE:${DSSP_BUILD_INCLUDE_DIR}>")
target_link_libraries(dssp_extraction PUBLIC dssp_inference dssp_localization dssp_media dssp_resource otter::otter srtsepinfer::srtsepinfer srtbeatinfer::srtbeatinfer)
target_link_libraries(dssp_extraction PRIVATE stdcorelib::stdcorelib dssp_codecutil)
target_include_directories(dssp_extraction PRIVATE "${DSSP_DRWAV_INCLUDE_DIR}")

dssp_sync_include(dssp_extraction PREFIX dsspextraction FORCE)

dssp_add_translations(dssp_extraction "${CMAKE_CURRENT_SOURCE_DIR}/translations/errors" "${DSSP_RESOURCE_BUILD_DIR}/localization/errors/extraction")
