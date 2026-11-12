find_package(ICU 68 REQUIRED COMPONENTS uc i18n data)
find_package(re2 CONFIG REQUIRED)

file(GLOB _src CONFIGURE_DEPENDS *.cpp *.h)
add_library(dssp_localization STATIC ${_src})

target_compile_features(dssp_localization PUBLIC cxx_std_23)
target_include_directories(dssp_localization PUBLIC "$<BUILD_INTERFACE:${DSSP_BUILD_INCLUDE_DIR}>")
target_link_libraries(dssp_localization PUBLIC dssp_error)
target_link_libraries(dssp_localization PRIVATE ICU::uc ICU::i18n ICU::data re2::re2 stdcorelib::stdcorelib)

dssp_sync_include(dssp_localization PREFIX dssplocalization FORCE)

# genrb runs on the host, including when the application is cross compiled.
find_program(DSSP_GENRB NAMES genrb
    HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/icu/bin" "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/icu/bin"
    NO_CMAKE_FIND_ROOT_PATH REQUIRED
)

function(dssp_add_translations target source_dir output_dir)
    file(GLOB _sources CONFIGURE_DEPENDS "${source_dir}/*.txt")
    set(_outputs)

    foreach(_source IN LISTS _sources)
        get_filename_component(_locale "${_source}" NAME_WE)
        set(_output "${output_dir}/${_locale}.res")

        add_custom_command(OUTPUT "${_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${output_dir}"
            COMMAND "${DSSP_GENRB}" -q -e UTF-8 -d "${output_dir}" "${_source}"
            DEPENDS "${_source}"
            VERBATIM
        )

        list(APPEND _outputs "${_output}")
    endforeach()

    set(_resource_target "${target}_resources")
    if(ARGC GREATER 3)
        set(_resource_target "${target}_${ARGV3}_resources")
    endif()
    add_custom_target(${_resource_target} DEPENDS ${_outputs})
    add_dependencies(${target} ${_resource_target})
endfunction()
