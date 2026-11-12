find_package(stdcorelib CONFIG REQUIRED)
find_package(ICU 68 REQUIRED COMPONENTS uc i18n data)

file(GLOB _src CONFIGURE_DEPENDS *.cpp *.h cli/*.cpp cli/*.h)
dssp_add_executable(dssp SOURCES ${_src})

set_target_properties(dssp PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    MACOSX_BUNDLE OFF
)

target_compile_definitions(dssp PRIVATE DSSP_DESCRIPTION="${DSSP_DESCRIPTION}")
target_compile_definitions(dssp PRIVATE DSSP_VERSION="${DSSP_VERSION}")

dssp_sync_include(dssp DIRECTORY . PREFIX dsspapp FORCE)
dssp_sync_include(dssp DIRECTORY cli PREFIX dsspcli FORCE)

target_link_libraries(dssp PRIVATE
    dssp_api_v1
    dssp_arch_diffsinger
    dssp_runtime
    dssp_inference
    dssp_localization
    stdcorelib::stdcorelib
    ICU::uc ICU::i18n ICU::data
)

include("${DSSP_SOURCE_DIR}/cmake/DeployInference.cmake")
dssp_deploy_inference_plugins(dssp)

# ICU bundles and the MIME database are explicit application deployment data.
if(DSSP_INSTALL)
    install(DIRECTORY "${DSSP_BUILD_DATA_DIR}/" DESTINATION "${DSSP_INSTALL_DATA_DIR}")
endif()
