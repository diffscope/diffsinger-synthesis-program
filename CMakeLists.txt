cmake_minimum_required(VERSION 3.19)

project(dssp
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "DSSP - DiffSinger Synthesis Program: A service that provides DiffSinger singing voice synthesis and audio analysis through the DiffScope Synthesis Platform API"
)

# ----------------------------------
# Build Options
# ----------------------------------
option(DSSP_BUILD_TESTS "Build test cases" OFF)
option(DSSP_INSTALL "Install application" ON)
option(DSSP_ENABLE_CUDA "Enable CUDA" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(DSSP_BUILD_TESTS)
    enable_testing()
endif()

# ----------------------------------
# CMake Settings
# ----------------------------------
if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /manifest:no")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /manifest:no")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /manifest:no")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")

    if(NOT DEFINED CMAKE_DEBUG_POSTFIX)
        set(CMAKE_DEBUG_POSTFIX "d")
    endif()
elseif(MINGW)
    set(CMAKE_STATIC_LIBRARY_PREFIX "")
    set(CMAKE_SHARED_LIBRARY_PREFIX "")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
endif()

if(NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release")
endif()

if(DSSP_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
endif()

# ----------------------------------
# Project Variables
# ----------------------------------
set(DSSP_DESCRIPTION ${PROJECT_DESCRIPTION})
if(NOT DEFINED DSSP_VERSION)
    set(DSSP_VERSION ${PROJECT_VERSION})
endif()
set(DSSP_INSTALL_NAME ${PROJECT_NAME})
set(DSSP_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# ----------------------------------
# Find basic dependencies
# ----------------------------------
# `qmsetup` runs on the build machine, so it is never searched in the target sysroot
find_package(qmsetup REQUIRED)

qm_import(Filesystem)

if(NOT DEFINED QMSETUP_BUILD_DIR)
    set(QMSETUP_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/out")
endif()

qm_init_directories()

# ----------------------------------
# Add source modules
# ----------------------------------
add_subdirectory(src)
