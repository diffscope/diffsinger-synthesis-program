# otter, pinned to the spec2.4-uptake branch tip selected for DSSP.
# A fetch by commit needs no archive hash.
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/diffscope/otter.git
    REF 7d9eaf51be8817de96bfc122f5237ec4355aaf17
    HEAD_REF spec2.4-uptake
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DOTTER_BUILD_TESTS:BOOL=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME otter CONFIG_PATH lib/cmake/otter)

# The plugins install under lib/plugins/otter/<category>/<name>/, which is a search path the host hands to SynthUnit rather than a link target, so vcpkg's usual expectations about lib/ do not apply to them.
set(VCPKG_POLICY_ALLOW_DLLS_IN_LIB enabled)
set(VCPKG_POLICY_SKIP_MISPLACED_CMAKE_FILES_CHECK enabled)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_copy_pdbs()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
