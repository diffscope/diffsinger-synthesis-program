find_package(absl CONFIG REQUIRED)
find_package(synthrt CONFIG REQUIRED)

file(GLOB _src CONFIGURE_DEPENDS *.cpp *.h)
add_library(dssp_runtime STATIC ${_src})

target_compile_features(dssp_runtime PUBLIC cxx_std_23)
target_include_directories(dssp_runtime PUBLIC "$<BUILD_INTERFACE:${DSSP_BUILD_INCLUDE_DIR}>")
target_compile_definitions(dssp_runtime PUBLIC $<$<PLATFORM_ID:Windows>:NOMINMAX>)

# Build and install trees have the same layout. Keep runtime lookup relative
# to the executable so moving a deployment does not require reconfiguration.
file(RELATIVE_PATH _plugin_relative_dir "${CMAKE_BINARY_DIR}/${DSSP_INSTALL_RUNTIME_DIR}" "${CMAKE_BINARY_DIR}/${DSSP_INSTALL_PLUGINS_DIR}")
file(RELATIVE_PATH _data_relative_dir "${CMAKE_BINARY_DIR}/${DSSP_INSTALL_RUNTIME_DIR}" "${CMAKE_BINARY_DIR}/${DSSP_INSTALL_DATA_DIR}")
target_compile_definitions(dssp_runtime PRIVATE
    DSSP_PLUGIN_RELATIVE_DIR="${_plugin_relative_dir}"
    DSSP_DATA_RELATIVE_DIR="${_data_relative_dir}"
)

target_link_libraries(dssp_runtime PUBLIC dssp_error dssp_resource stdcorelib::stdcorelib synthrt::synthrt)
target_link_libraries(dssp_runtime PRIVATE absl::strings $<$<PLATFORM_ID:Windows>:dxgi.lib>)

if(DSSP_ENABLE_CUDA)
    target_compile_definitions(dssp_runtime PRIVATE DSSP_ENABLE_CUDA)
endif()

dssp_sync_include(dssp_runtime PREFIX dsspruntime FORCE)
