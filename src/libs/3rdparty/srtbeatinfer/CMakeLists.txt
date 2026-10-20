find_package(synthrt CONFIG REQUIRED)
find_package(dsinfer CONFIG REQUIRED)
find_package(stdcorelib CONFIG REQUIRED)

include(GenerateExportHeader)

add_library(srtbeatinfer SHARED src/BeatApiL1.cpp src/BeatError.cpp src/BeatTask.cpp)
add_library(srtbeatinfer::srtbeatinfer ALIAS srtbeatinfer)

target_compile_features(srtbeatinfer PUBLIC cxx_std_23)
target_link_libraries(srtbeatinfer PUBLIC synthrt::synthrt)
target_include_directories(srtbeatinfer PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>")

generate_export_header(srtbeatinfer EXPORT_FILE_NAME "include/srtbeatinfer/export.h")
# Deployment rewrites runtime paths. Retain the original binary for CMake installation.
set_target_properties(srtbeatinfer PROPERTIES
    OUTPUT_NAME synthrt-srtbeatinfer
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime/$<CONFIG>"
)

add_library(srtbeatinfer_dsp STATIC src/BeatFeatures.cpp)
target_compile_features(srtbeatinfer_dsp PUBLIC cxx_std_23)
target_link_libraries(srtbeatinfer_dsp PUBLIC srtbeatinfer)
target_include_directories(srtbeatinfer_dsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(srtbeatinfer_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)

add_library(srtbeatinferbeatthis MODULE plugins/beat-this/main.cpp)
target_compile_features(srtbeatinferbeatthis PRIVATE cxx_std_23)
target_link_libraries(srtbeatinferbeatthis PRIVATE srtbeatinfer_dsp dsinfer::dsinfer stdcorelib::stdcorelib)
set_target_properties(srtbeatinferbeatthis PROPERTIES
    PREFIX "" DEBUG_POSTFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/$<CONFIG>/beat-this"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/$<CONFIG>/beat-this"
)

stdc_add_plugin_metadata(TARGET srtbeatinferbeatthis IID org.openvpi.synthrt.plugin.InferenceInterpreter)
set(SRTBEATINFER_PLUGIN_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugins/beat-this/plugin.json" CACHE INTERNAL "srtbeatinfer plugin manifest")

if(DSSP_INSTALL)
    install(TARGETS srtbeatinfer RUNTIME DESTINATION "${DSSP_INSTALL_RUNTIME_DIR}" LIBRARY DESTINATION "${DSSP_INSTALL_LIBRARY_DIR}")
    install(TARGETS srtbeatinferbeatthis RUNTIME DESTINATION "${DSSP_INSTALL_PLUGINS_DIR}/srtbeatinfer/inferenceinterpreters/beat-this" LIBRARY DESTINATION "${DSSP_INSTALL_PLUGINS_DIR}/srtbeatinfer/inferenceinterpreters/beat-this")
    install(FILES "${SRTBEATINFER_PLUGIN_MANIFEST}" DESTINATION "${DSSP_INSTALL_PLUGINS_DIR}/srtbeatinfer/inferenceinterpreters/beat-this")
endif()
