find_package(synthrt CONFIG REQUIRED)
find_package(dsinfer CONFIG REQUIRED)
find_package(wolf CONFIG REQUIRED)
find_package(onnxruntime-builds CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

file(GLOB _src CONFIGURE_DEPENDS *.cpp *.h)
add_library(dssp_inference STATIC ${_src})

target_compile_features(dssp_inference PUBLIC cxx_std_23)
target_include_directories(dssp_inference PUBLIC "$<BUILD_INTERFACE:${DSSP_BUILD_INCLUDE_DIR}>")
target_link_libraries(dssp_inference PUBLIC dssp_error dssp_resource dssp_runtime dssp_localization synthrt::synthrt wolf::wolf)
target_link_libraries(dssp_inference PRIVATE dssp_codecutil dsinfer::dsinfer onnxruntime-builds::default OpenSSL::Crypto unofficial::sqlite3::sqlite3)

dssp_sync_include(dssp_inference PREFIX dsspinference FORCE)
