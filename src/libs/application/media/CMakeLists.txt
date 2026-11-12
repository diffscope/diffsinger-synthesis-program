find_package(unofficial-libmagic CONFIG REQUIRED)
find_package(uriparser CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_path(DSSP_DRWAV_INCLUDE_DIR NAMES dr_wav.h REQUIRED)

file(GLOB _src CONFIGURE_DEPENDS *.cpp *.h)
add_library(dssp_media STATIC ${_src})

target_compile_features(dssp_media PUBLIC cxx_std_23)
target_include_directories(dssp_media PUBLIC "$<BUILD_INTERFACE:${DSSP_BUILD_INCLUDE_DIR}>")
target_include_directories(dssp_media PRIVATE "${DSSP_DRWAV_INCLUDE_DIR}")
target_link_libraries(dssp_media PUBLIC dssp_error dssp_localization)
target_link_libraries(dssp_media PRIVATE dssp_codecutil unofficial::libmagic::libmagic stdcorelib::stdcorelib uriparser::uriparser OpenSSL::Crypto)

dssp_sync_include(dssp_media PREFIX dsspmedia FORCE)

dssp_add_translations(dssp_media "${CMAKE_CURRENT_SOURCE_DIR}/translations/errors" "${DSSP_RESOURCE_BUILD_DIR}/localization/errors/media" errors)

set(_dictionary "${DSSP_RESOURCE_BUILD_DIR}/media/magic.mgc")
add_custom_command(OUTPUT "${_dictionary}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DSSP_RESOURCE_BUILD_DIR}/media"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${unofficial-libmagic_DICTIONARY}" "${_dictionary}"
    DEPENDS "${unofficial-libmagic_DICTIONARY}"
    VERBATIM
)

add_custom_target(dssp_media_resources DEPENDS "${_dictionary}")
add_dependencies(dssp_media dssp_media_resources)
