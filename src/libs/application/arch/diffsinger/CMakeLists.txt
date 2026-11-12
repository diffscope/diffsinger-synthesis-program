find_package(dsinfer CONFIG REQUIRED)

file(GLOB _src CONFIGURE_DEPENDS *.cpp *.h)
add_library(dssp_arch_diffsinger STATIC ${_src})

target_compile_features(dssp_arch_diffsinger PUBLIC cxx_std_23)
target_include_directories(dssp_arch_diffsinger PUBLIC "$<BUILD_INTERFACE:${DSSP_BUILD_INCLUDE_DIR}>")
target_link_libraries(dssp_arch_diffsinger PUBLIC dssp_arch dssp_inference dssp_media dssp_localization dsinfer::dsinfer)
target_link_libraries(dssp_arch_diffsinger PRIVATE stdcorelib::stdcorelib)
target_compile_definitions(dssp_arch_diffsinger PRIVATE DSSP_PROGRAM_VERSION="${DSSP_VERSION}")

# Keep environment tags tied to the actual dependency pins, not duplicated literals.
foreach(_dependency IN ITEMS synthrt wolf)
    file(STRINGS "${DSSP_SOURCE_DIR}/scripts/vcpkg-overrides/${_dependency}/portfile.cmake" _ref_line REGEX "^[ \t]*REF [0-9a-f]+$")
    string(REGEX REPLACE "^[ \t]*REF ([0-9a-f]+)$" "\\1" _ref "${_ref_line}")
    string(TOUPPER "${_dependency}" _dependency_upper)
    target_compile_definitions(dssp_arch_diffsinger PRIVATE "DSSP_${_dependency_upper}_REVISION=\"${_ref}\"")
endforeach()

dssp_sync_include(dssp_arch_diffsinger PREFIX dssparchdiffsinger FORCE)

dssp_add_translations(dssp_arch_diffsinger "${CMAKE_CURRENT_SOURCE_DIR}/translations" "${DSSP_RESOURCE_BUILD_DIR}/localization/diffsinger")

dssp_add_translations(dssp_arch_diffsinger "${CMAKE_CURRENT_SOURCE_DIR}/translations/errors" "${DSSP_RESOURCE_BUILD_DIR}/localization/errors/diffsinger" errors)
