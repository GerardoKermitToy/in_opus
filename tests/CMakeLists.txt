if(EXISTS "${PROJECT_SOURCE_DIR}/third_party/reference/input-sdk/IN2.H"
   AND EXISTS "${PROJECT_SOURCE_DIR}/third_party/reference/output-sdk/OUT.H")
    add_library(abi_reference_check OBJECT abi_reference_check.cpp)
    target_include_directories(abi_reference_check PRIVATE "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/third_party/reference/input-sdk"
        "${PROJECT_SOURCE_DIR}/third_party/reference/output-sdk")
endif()

add_library(test_fixtures STATIC fixtures.cpp)
target_link_libraries(test_fixtures PUBLIC opus_decoder)
target_compile_options(test_fixtures PRIVATE /W4 /permissive- /utf-8)

add_executable(decoder_tests decoder_tests.cpp)
target_link_libraries(decoder_tests PRIVATE test_fixtures)
target_compile_options(decoder_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME decoder_self_test COMMAND decoder_tests --self-test "${CMAKE_CURRENT_BINARY_DIR}/decoder-data")

add_executable(plugin_tests plugin_tests.cpp)
target_link_libraries(plugin_tests PRIVATE test_fixtures user32 advapi32)
target_compile_options(plugin_tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(plugin_tests in_opus)
add_test(NAME plugin_integration COMMAND plugin_tests "$<TARGET_FILE:in_opus>" "${CMAKE_CURRENT_BINARY_DIR}/plugin-data")
set_tests_properties(decoder_self_test plugin_integration PROPERTIES TIMEOUT 30)

add_executable(dialog_tests dialog_tests.cpp)
target_link_libraries(dialog_tests PRIVATE test_fixtures user32 gdi32 advapi32)
target_compile_options(dialog_tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(dialog_tests in_opus)
add_test(NAME file_info_dialog COMMAND dialog_tests "$<TARGET_FILE:in_opus>" "${CMAKE_CURRENT_BINARY_DIR}/dialog-data")
set_tests_properties(file_info_dialog PROPERTIES TIMEOUT 30)

add_executable(multichannel_tests multichannel_tests.cpp)
target_link_libraries(multichannel_tests PRIVATE test_fixtures)
target_compile_options(multichannel_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME multichannel_downmix COMMAND multichannel_tests --self-test "${CMAKE_CURRENT_BINARY_DIR}/surround-data")
set_tests_properties(multichannel_downmix PROPERTIES TIMEOUT 30)

add_executable(native_output_tests native_output_tests.cpp ../src/output_settings.cpp)
target_link_libraries(native_output_tests PRIVATE test_fixtures user32 gdi32 advapi32)
target_compile_options(native_output_tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(native_output_tests in_opus)
add_test(NAME native_output COMMAND native_output_tests "$<TARGET_FILE:in_opus>" "${CMAKE_CURRENT_BINARY_DIR}/native-data")
set_tests_properties(native_output PROPERTIES TIMEOUT 60)

# Check PE imports and readable archive CRT directives. The project's forced
# include also validates the compiler's CRT macros for opaque /GL objects.
add_test(NAME dynamic_crt COMMAND "${CMAKE_COMMAND}"
    "-DLINKER=${CMAKE_LINKER}"
    "-DPLUGIN=$<TARGET_FILE:in_opus>"
    "-DARCHIVES=$<TARGET_FILE:ogg>;$<TARGET_FILE:opus>;$<TARGET_FILE:opusfile>;$<TARGET_FILE:opus_decoder>;$<TARGET_FILE:test_fixtures>"
    "-DCONFIG=$<CONFIG>"
    "-DLTO=$<AND:$<CONFIG:Release>,$<BOOL:${winamp_opus_ipo_supported}>>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_dynamic_crt.cmake")
set_tests_properties(dynamic_crt PROPERTIES TIMEOUT 30)

if(WINAMP_OPUS_MULTICHANNEL_TEST_FILE)
    add_test(NAME surround_decode COMMAND decoder_tests --file "${WINAMP_OPUS_MULTICHANNEL_TEST_FILE}")
    add_test(NAME surround_plugin COMMAND plugin_tests "$<TARGET_FILE:in_opus>"
        "${CMAKE_CURRENT_BINARY_DIR}/surround-plugin-data" "${WINAMP_OPUS_MULTICHANNEL_TEST_FILE}")
    add_test(NAME surround_matrix COMMAND multichannel_tests --file "${WINAMP_OPUS_MULTICHANNEL_TEST_FILE}")
    add_test(NAME surround_native COMMAND native_output_tests "$<TARGET_FILE:in_opus>"
        "${CMAKE_CURRENT_BINARY_DIR}/surround-native-data" "${WINAMP_OPUS_MULTICHANNEL_TEST_FILE}")
    set_tests_properties(surround_decode surround_plugin surround_matrix surround_native PROPERTIES TIMEOUT 60)
endif()

if(WINAMP_OPUS_TEST_FILE)
    add_test(NAME sample_decode COMMAND decoder_tests --file "${WINAMP_OPUS_TEST_FILE}")
    add_test(NAME sample_plugin COMMAND plugin_tests "$<TARGET_FILE:in_opus>"
        "${CMAKE_CURRENT_BINARY_DIR}/sample-data" "${WINAMP_OPUS_TEST_FILE}")
    add_test(NAME sample_dialog COMMAND dialog_tests "$<TARGET_FILE:in_opus>"
        "${CMAKE_CURRENT_BINARY_DIR}/sample-dialog-data" "${WINAMP_OPUS_TEST_FILE}")
    set_tests_properties(sample_decode sample_plugin sample_dialog PROPERTIES TIMEOUT 60)
endif()
