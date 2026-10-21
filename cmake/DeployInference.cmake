include_guard(GLOBAL)

function(dssp_deploy_inference_plugins target)
    find_package(onnxruntime-builds CONFIG REQUIRED)

    # The upstream packages install plugins separately from their link targets.
    get_filename_component(_vcpkg_prefix "${ONNXRUNTIME_BUILDS_INCLUDE_DIR}" DIRECTORY)
    set(_library_source "${_vcpkg_prefix}/$<$<CONFIG:Debug>:debug/>lib")
    set(_plugin_root "${DSSP_BUILD_PLUGINS_DIR}")

    if(WIN32)
        set(_runtime_source "${_vcpkg_prefix}/$<$<CONFIG:Debug>:debug/>bin")
        set(_dependency_dir "${DSSP_BUILD_RUNTIME_DIR}")
        set(_install_dependency_dir "${DSSP_INSTALL_RUNTIME_DIR}")
    else()
        set(_runtime_source "${_library_source}")
        set(_dependency_dir "${DSSP_BUILD_LIBRARY_DIR}")
        set(_install_dependency_dir "${DSSP_INSTALL_LIBRARY_DIR}")
    endif()

    # Categories and driver bundles are loaded at runtime, so linking the four
    # libraries alone cannot deploy them. Preserve their upstream directory shape.
    foreach(_library IN ITEMS dsinfer wolf otter)
        qm_add_copy_command(${target}
            SOURCES "${_library_source}/plugins/${_library}/"
            DESTINATION "${_plugin_root}/${_library}"
            SKIP_INSTALL
        )

        if(DSSP_INSTALL)
            install(DIRECTORY "${_library_source}/plugins/${_library}/"
                DESTINATION "${DSSP_INSTALL_PLUGINS_DIR}/${_library}")
        endif()
    endforeach()

    # Stage copies of project libraries and interpreters because deployment rewrites
    # runtime paths. CMake installation requires the unmodified target binaries.
    set(_separation_destination "${_plugin_root}/srtsepinfer/inferenceinterpreters/demucs")
    add_custom_target(srtsepinfer_deploy_plugins
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dependency_dir}" "${_separation_destination}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:srtsepinfer>" "${_dependency_dir}/$<TARGET_FILE_NAME:srtsepinfer>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:srtsepinferdemucs>" "${_separation_destination}/$<TARGET_FILE_NAME:srtsepinferdemucs>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SRTSEPINFER_PLUGIN_MANIFEST}" "${_separation_destination}/plugin.json"
        DEPENDS srtsepinfer srtsepinferdemucs "${SRTSEPINFER_PLUGIN_MANIFEST}"
        COMMENT "Stage project-built srtsepinfer library and interpreter"
        VERBATIM
    )

    add_dependencies(${target} srtsepinfer_deploy_plugins)

    set(_beat_destination "${_plugin_root}/srtbeatinfer/inferenceinterpreters/beat-this")
    add_custom_target(srtbeatinfer_deploy_plugins
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dependency_dir}" "${_beat_destination}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:srtbeatinfer>" "${_dependency_dir}/$<TARGET_FILE_NAME:srtbeatinfer>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:srtbeatinferbeatthis>" "${_beat_destination}/$<TARGET_FILE_NAME:srtbeatinferbeatthis>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRTBEATINFER_PLUGIN_MANIFEST}" "${_beat_destination}/plugin.json"
        DEPENDS srtbeatinfer srtbeatinferbeatthis "${SRTBEATINFER_PLUGIN_MANIFEST}"
        COMMENT "Stage project-built srtbeatinfer library and interpreter"
        VERBATIM)

    add_dependencies(${target} srtbeatinfer_deploy_plugins)

    set(_ort_destination "${_plugin_root}/dsinfer/inferencedrivers/onnx/runtime")
    set(_ort_install_destination "${DSSP_INSTALL_PLUGINS_DIR}/dsinfer/inferencedrivers/onnx/runtime")

    if(NOT IS_DIRECTORY "${ONNXRUNTIME_BUILDS_RUNTIME_DIR}")
        message(FATAL_ERROR "The onnxruntime-builds default runtime payload is missing.")
    endif()

    qm_add_copy_command(${target}
        SOURCES "${ONNXRUNTIME_BUILDS_RUNTIME_DIR}/"
        DESTINATION "${_ort_destination}"
        SKIP_INSTALL
    )

    if(DSSP_INSTALL)
        install(DIRECTORY "${ONNXRUNTIME_BUILDS_RUNTIME_DIR}/" DESTINATION "${_ort_install_destination}")
    endif()

    if(DSSP_ENABLE_CUDA)
        if(NOT IS_DIRECTORY "${ONNXRUNTIME_BUILDS_CUDA_RUNTIME_DIR}")
            message(FATAL_ERROR
                "DSSP_ENABLE_CUDA requires vcpkg install --x-feature=cuda12. "
                "Reinstall synthrt with this feature, or set DSSP_ENABLE_CUDA=OFF.")
        endif()

        qm_add_copy_command(${target}
            SOURCES "${ONNXRUNTIME_BUILDS_CUDA_RUNTIME_DIR}/"
            DESTINATION "${_ort_destination}/cuda"
            SKIP_INSTALL
        )

        if(DSSP_INSTALL)
            install(DIRECTORY "${ONNXRUNTIME_BUILDS_CUDA_RUNTIME_DIR}/" DESTINATION "${_ort_install_destination}/cuda")
        endif()
    endif()

    # Dynamically loaded plugins have dependencies that the executable's
    # deployment cannot discover. Let qmsetup resolve those plugin binaries;
    # do not maintain a separate list for cpp-pinyin, LuaJIT, or future plugins.
    if(NOT QMSETUP_CORECMD_EXECUTABLE)
        message(FATAL_ERROR "Inference plugin deployment requires qmsetup's qmcorecmd tool.")
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-Dqmcorecmd=${QMSETUP_CORECMD_EXECUTABLE}"
            "-Dplugin_root=${_plugin_root}"
            "-Dlibrary_dir=${_runtime_source}"
            "-Ddeploy_dir=${_dependency_dir}"
            -P "${DSSP_SOURCE_DIR}/cmake/DeployPluginDependencies.cmake"
        COMMENT "Deploy inference plugin dependencies with qmsetup"
        VERBATIM
    )

    # Changing the CUDA option must also remove a payload staged by an earlier build.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-Dexpect_cuda=$<BOOL:${DSSP_ENABLE_CUDA}>"
            "-Druntime_dir=${_ort_destination}"
            -P "${DSSP_SOURCE_DIR}/cmake/OrtRuntimeGate.cmake"
        COMMENT "Gate ONNX Runtime CUDA payload (DSSP_ENABLE_CUDA=${DSSP_ENABLE_CUDA})"
        VERBATIM
    )

    if(DSSP_INSTALL)
        install(CODE "
            execute_process(
                COMMAND \"${CMAKE_COMMAND}\"
                    \"-Dqmcorecmd=${QMSETUP_CORECMD_EXECUTABLE}\"
                    \"-Dplugin_root=\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${DSSP_INSTALL_PLUGINS_DIR}\"
                    \"-Dlibrary_dir=${_runtime_source}\"
                    \"-Ddeploy_dir=\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_install_dependency_dir}\"
                    -P \"${DSSP_SOURCE_DIR}/cmake/DeployPluginDependencies.cmake\"
                COMMAND_ERROR_IS_FATAL ANY
            )
            execute_process(
                COMMAND \"${CMAKE_COMMAND}\"
                    -Dexpect_cuda=${DSSP_ENABLE_CUDA}
                    \"-Druntime_dir=\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${_ort_install_destination}\"
                    -P \"${DSSP_SOURCE_DIR}/cmake/OrtRuntimeGate.cmake\"
                COMMAND_ERROR_IS_FATAL ANY
            )
        ")
    endif()
endfunction()
