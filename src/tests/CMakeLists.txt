find_package(GTest CONFIG REQUIRED)
find_package(stdcorelib CONFIG REQUIRED)
find_package(onnxruntime-builds CONFIG REQUIRED)
find_package(ICU 68 REQUIRED COMPONENTS uc i18n data)

# Default time-zone changes are isolated from the application/backend tests.
add_executable(dssp_log_timestamp_tests LogTimestampTest.cpp ../app/LogTimestamp.cpp)
target_compile_features(dssp_log_timestamp_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_log_timestamp_tests PRIVATE GTest::gtest_main ICU::uc ICU::i18n ICU::data)

add_executable(dssp_logger_tests LoggerTest.cpp ../app/Logger.cpp ../app/LogTimestamp.cpp)
target_compile_features(dssp_logger_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_logger_tests PRIVATE GTest::gtest_main stdcorelib::stdcorelib ICU::uc ICU::i18n ICU::data)

add_executable(dssp_tests
    ErrorTest.cpp
    MediaTypeTest.cpp
    RequestCodecTest.cpp
    ResponseTest.cpp
    ResponseNegotiationTest.cpp
    ControllerTest.cpp
    DiscoveryTest.cpp
    ArchitectureDispatchTest.cpp
    ArchitectureMappingTest.cpp
    ArchitectureCatalogTest.cpp
    ServerConfigTest.cpp
    DeviceInfoTest.cpp
    RequestLifetimeTest.cpp
    AccessLogTest.cpp
    LogCapture.h
    TestFiles.h
    TestLanguagePackage.h
    TestAnalysisPackage.h
    TestCoroutine.h
)

target_compile_features(dssp_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_tests PRIVATE dssp_api_v1 dssp_runtime GTest::gtest_main)
target_compile_definitions(dssp_tests PRIVATE DSSP_TEST_OPENAPI_FILE="${DSSP_SOURCE_DIR}/docs/open-api/1.0.json")

dssp_sync_include(dssp_tests PREFIX dssptests FORCE NO_INSTALL)

# Real HTTP lifecycle coverage runs in a separate process: Drogon owns a singleton.
add_executable(dssp_http_access_log_tests HttpAccessLogTest.cpp)
target_compile_features(dssp_http_access_log_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_http_access_log_tests PRIVATE dssp_api_v1 GTest::gtest_main)

# Streaming lifecycle coverage requires an independent Drogon singleton.
add_executable(dssp_http_streaming_tests HttpStreamingTest.cpp)
target_compile_features(dssp_http_streaming_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_http_streaming_tests PRIVATE dssp_api_v1 GTest::gtest_main)

# Backend and composition tests are isolated from the HTTP/API target.
add_executable(dssp_arch_diffsinger_tests DiffSingerTest.cpp SingerDiscoveryTest.cpp LanguagePredictionTest.cpp SynthesisConversionTest.cpp SynthesisTest.cpp SynthesisPipelineTest.cpp ../app/ArchComposition.cpp)
target_compile_features(dssp_arch_diffsinger_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_arch_diffsinger_tests PRIVATE dssp_arch_diffsinger GTest::gtest_main)
target_compile_definitions(dssp_arch_diffsinger_tests PRIVATE DSSP_TEST_RESOURCE_DIR="${DSSP_RESOURCE_BUILD_DIR}")

get_filename_component(_test_vcpkg_prefix "${ONNXRUNTIME_BUILDS_INCLUDE_DIR}" DIRECTORY)
target_compile_definitions(dssp_http_access_log_tests PRIVATE DSSP_TEST_PLUGIN_DIR="${_test_vcpkg_prefix}/$<$<CONFIG:Debug>:debug/>lib/plugins")
target_compile_definitions(dssp_arch_diffsinger_tests PRIVATE DSSP_TEST_PLUGIN_DIR="${_test_vcpkg_prefix}/$<$<CONFIG:Debug>:debug/>lib/plugins")

# This host test deliberately has no architecture target in its link graph.
add_executable(dssp_inference_tests InferenceTest.cpp InferencePoolTest.cpp ParallelismPolicyTest.cpp)
target_compile_features(dssp_inference_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_inference_tests PRIVATE dssp_inference GTest::gtest_main)
target_compile_definitions(dssp_inference_tests PRIVATE DSSP_TEST_PLUGIN_DIR="${_test_vcpkg_prefix}/$<$<CONFIG:Debug>:debug/>lib/plugins")

add_executable(dssp_resource_tests ResourceTest.cpp TaskExecutorTest.cpp WorkerPoolTest.cpp ExecutionGateTest.cpp CoroutineTest.cpp)
target_compile_features(dssp_resource_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_resource_tests PRIVATE dssp_resource GTest::gtest_main)

add_executable(dssp_extraction_tests ExtractionConversionTest.cpp ExtractionServiceTest.cpp)
target_compile_features(dssp_extraction_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_extraction_tests PRIVATE dssp_extraction dssp_codecutil GTest::gtest_main)
target_include_directories(dssp_extraction_tests PRIVATE "${DSSP_DRWAV_INCLUDE_DIR}")
target_compile_definitions(dssp_extraction_tests PRIVATE DSSP_TEST_PLUGIN_DIR="${_test_vcpkg_prefix}/$<$<CONFIG:Debug>:debug/>lib/plugins")

add_executable(dssp_metadata_resources_tests CodecutilTest.cpp LocalizationTest.cpp MediaPathTest.cpp MediaResourceTest.cpp WaveTest.cpp)
target_compile_features(dssp_metadata_resources_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_metadata_resources_tests PRIVATE dssp_inference dssp_media dssp_codecutil GTest::gtest_main)
target_include_directories(dssp_metadata_resources_tests PRIVATE "${DSSP_DRWAV_INCLUDE_DIR}")
target_compile_definitions(dssp_metadata_resources_tests PRIVATE DSSP_TEST_RESOURCE_DIR="${DSSP_RESOURCE_BUILD_DIR}")
target_compile_definitions(dssp_metadata_resources_tests PRIVATE DSSP_TEST_TRANSLATION_DIR="${CMAKE_CURRENT_BINARY_DIR}/translations")

dssp_add_translations(dssp_metadata_resources_tests "${CMAKE_CURRENT_SOURCE_DIR}/translations" "${CMAKE_CURRENT_BINARY_DIR}/translations")

# Discovery happens only when tests are run, not during cross-compilation/build.
include(GoogleTest)

gtest_discover_tests(dssp_log_timestamp_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(dssp_logger_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(dssp_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(dssp_http_access_log_tests DISCOVERY_MODE PRE_TEST)

gtest_discover_tests(dssp_http_streaming_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
gtest_discover_tests(dssp_arch_diffsinger_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(dssp_inference_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(dssp_resource_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 10)
gtest_discover_tests(dssp_extraction_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 10)
gtest_discover_tests(dssp_metadata_resources_tests DISCOVERY_MODE PRE_TEST)

add_executable(dssp_separation_tests SeparationTest.cpp DemucsSpectralReference.h)
target_compile_features(dssp_separation_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_separation_tests PRIVATE dssp_api_v1 srtsepinfer_dsp GTest::gtest_main)
target_include_directories(dssp_separation_tests PRIVATE "${DSSP_DRWAV_INCLUDE_DIR}")
target_compile_definitions(dssp_separation_tests PRIVATE DSSP_TEST_SEPARATION_PLUGIN_DIR="${DSSP_BUILD_PLUGINS_DIR}/srtsepinfer/inferenceinterpreters")

add_dependencies(dssp_separation_tests srtsepinfer_deploy_plugins)

gtest_discover_tests(dssp_separation_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 15)

add_executable(dssp_beat_tests BeatTest.cpp BeatFeatureReference.h)
target_compile_features(dssp_beat_tests PRIVATE cxx_std_23)
target_link_libraries(dssp_beat_tests PRIVATE dssp_api_v1 srtbeatinfer_dsp GTest::gtest_main)
target_include_directories(dssp_beat_tests PRIVATE "${DSSP_DRWAV_INCLUDE_DIR}")
target_compile_definitions(dssp_beat_tests PRIVATE DSSP_TEST_BEAT_PLUGIN_DIR="${DSSP_BUILD_PLUGINS_DIR}/srtbeatinfer/inferenceinterpreters")

add_dependencies(dssp_beat_tests srtbeatinfer_deploy_plugins)

gtest_discover_tests(dssp_beat_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 15)
