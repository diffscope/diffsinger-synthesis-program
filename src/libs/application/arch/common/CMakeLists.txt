find_package(jsoncpp CONFIG REQUIRED)

file(GLOB _src CONFIGURE_DEPENDS *.cpp *.h)
add_library(dssp_arch STATIC ${_src})

target_compile_features(dssp_arch PUBLIC cxx_std_23)
target_include_directories(dssp_arch PUBLIC "$<BUILD_INTERFACE:${DSSP_BUILD_INCLUDE_DIR}>")
target_link_libraries(dssp_arch PUBLIC dssp_error dssp_localization dssp_resource JsonCpp::JsonCpp)
target_link_libraries(dssp_arch PRIVATE stdcorelib::stdcorelib)

dssp_sync_include(dssp_arch PREFIX dssparch FORCE)
