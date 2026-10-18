
file(GLOB_RECURSE _src CONFIGURE_DEPENDS *.cpp *.h)
add_library(dssp_api_v1 STATIC ${_src})

set(_openapi_data "${CMAKE_CURRENT_BINARY_DIR}/generated/OpenApiSpecData.inc")
add_custom_command(
    OUTPUT "${_openapi_data}"
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT_FILE=${DSSP_SOURCE_DIR}/docs/open-api/1.0.json"
        "-DOUTPUT_FILE=${_openapi_data}"
        -P "${DSSP_SOURCE_DIR}/cmake/EmbedFile.cmake"
    DEPENDS
        "${DSSP_SOURCE_DIR}/docs/open-api/1.0.json"
        "${DSSP_SOURCE_DIR}/cmake/EmbedFile.cmake"
    COMMENT "Embedding the OpenAPI specification"
    VERBATIM
)
target_sources(dssp_api_v1 PRIVATE "${_openapi_data}")
target_include_directories(dssp_api_v1 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_features(dssp_api_v1 PUBLIC cxx_std_23)
target_include_directories(dssp_api_v1 PUBLIC "$<BUILD_INTERFACE:${DSSP_BUILD_INCLUDE_DIR}>")
target_link_libraries(dssp_api_v1 PUBLIC dssp_arch dssp_http dssp_extraction)

# Dispatch templates log through stdcorelib in their synchronized headers.
target_link_libraries(dssp_api_v1 PUBLIC stdcorelib::stdcorelib)

dssp_sync_include(dssp_api_v1 PREFIX dsspapiv1 FORCE)
