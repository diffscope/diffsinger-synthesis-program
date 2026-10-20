# This library and its interpreter are project targets, not vcpkg payloads.
find_package(synthrt CONFIG REQUIRED)
find_package(dsinfer CONFIG REQUIRED)
find_package(stdcorelib CONFIG REQUIRED)
find_package(pffft CONFIG REQUIRED)

include(GenerateExportHeader)

add_library(srtsepinfer SHARED src/SeparationApiL1.cpp src/SeparationError.cpp src/SeparationTask.cpp)
add_library(srtsepinfer::srtsepinfer ALIAS srtsepinfer)

target_compile_features(srtsepinfer PUBLIC cxx_std_23)
target_link_libraries(srtsepinfer PUBLIC synthrt::synthrt)
target_include_directories(srtsepinfer PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>")

generate_export_header(srtsepinfer EXPORT_FILE_NAME "include/srtsepinfer/export.h")
# Deployment rewrites runtime paths. Retain the original binary for CMake installation.
set_target_properties(srtsepinfer PROPERTIES
    OUTPUT_NAME synthrt-srtsepinfer
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime/$<CONFIG>")

add_library(srtsepinfer_dsp STATIC src/SpectralTransform.cpp)
target_compile_features(srtsepinfer_dsp PUBLIC cxx_std_23)
target_link_libraries(srtsepinfer_dsp PUBLIC srtsepinfer)
target_link_libraries(srtsepinfer_dsp PRIVATE pffft::pffft)
target_include_directories(srtsepinfer_dsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(srtsepinfer_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)

add_library(srtsepinferdemucs MODULE plugins/demucs/main.cpp)
target_compile_features(srtsepinferdemucs PRIVATE cxx_std_23)
target_link_libraries(srtsepinferdemucs PRIVATE srtsepinfer_dsp dsinfer::dsinfer stdcorelib::stdcorelib)

# Build outside the deployed tree so staging has a genuine target dependency and
# can restore deleted metadata without relying on a vcpkg directory glob.
set(SRTSEPINFER_PLUGIN_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/plugins/$<CONFIG>/demucs")
set_target_properties(srtsepinferdemucs PROPERTIES PREFIX "" DEBUG_POSTFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${SRTSEPINFER_PLUGIN_BUILD_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SRTSEPINFER_PLUGIN_BUILD_DIR}")
stdc_add_plugin_metadata(TARGET srtsepinferdemucs IID org.openvpi.synthrt.plugin.InferenceInterpreter)

set(SRTSEPINFER_PLUGIN_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugins/demucs/plugin.json" CACHE INTERNAL "srtsepinfer plugin manifest")

if(DSSP_INSTALL)
    install(TARGETS srtsepinfer RUNTIME DESTINATION "${DSSP_INSTALL_RUNTIME_DIR}" LIBRARY DESTINATION "${DSSP_INSTALL_LIBRARY_DIR}")
    install(TARGETS srtsepinferdemucs RUNTIME DESTINATION "${DSSP_INSTALL_PLUGINS_DIR}/srtsepinfer/inferenceinterpreters/demucs" LIBRARY DESTINATION "${DSSP_INSTALL_PLUGINS_DIR}/srtsepinfer/inferenceinterpreters/demucs")
    install(FILES "${SRTSEPINFER_PLUGIN_MANIFEST}" DESTINATION "${DSSP_INSTALL_PLUGINS_DIR}/srtsepinfer/inferenceinterpreters/demucs")
endif()
