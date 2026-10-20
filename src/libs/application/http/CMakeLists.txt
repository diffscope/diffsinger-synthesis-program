find_package(Drogon CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(re2 CONFIG REQUIRED)
find_package(uriparser CONFIG REQUIRED)

file(GLOB _src CONFIGURE_DEPENDS *.cpp *.h)
add_library(dssp_http STATIC ${_src})

target_compile_features(dssp_http PUBLIC cxx_std_23)
target_include_directories(dssp_http PUBLIC "$<BUILD_INTERFACE:${DSSP_BUILD_INCLUDE_DIR}>")
target_compile_definitions(dssp_http PUBLIC $<$<PLATFORM_ID:Windows>:NOMINMAX>)
target_link_libraries(dssp_http PUBLIC dssp_error dssp_resource Drogon::Drogon)
target_link_libraries(dssp_http PRIVATE absl::strings stdcorelib::stdcorelib re2::re2 uriparser::uriparser)

dssp_sync_include(dssp_http PREFIX dssphttp FORCE)
