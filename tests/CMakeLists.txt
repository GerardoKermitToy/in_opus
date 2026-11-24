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

add_executable(playback_notification_tests playback_notification_tests.cpp ../src/output_settings.cpp)
target_link_libraries(playback_notification_tests PRIVATE test_fixtures user32 advapi32)
target_compile_options(playback_notification_tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(playback_notification_tests in_opus)
add_test(NAME playback_notifications COMMAND playback_notification_tests "$<TARGET_FILE:in_opus>" "${CMAKE_CURRENT_BINARY_DIR}/notification-data")
set_tests_properties(playback_notifications PROPERTIES TIMEOUT 90)

add_executable(dialog_tests dialog_tests.cpp)
target_link_libraries(dialog_tests PRIVATE test_fixtures user32 gdi32 advapi32)
target_compile_options(dialog_tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(dialog_tests in_opus)
add_test(NAME file_info_dialog COMMAND dialog_tests "$<TARGET_FILE:in_opus>" "${CMAKE_CURRENT_BINARY_DIR}/dialog-data")
set_tests_properties(file_info_dialog PROPERTIES TIMEOUT 30)

add_executable(tag_editor_tests tag_editor_tests.cpp ../src/metadata.cpp)
target_link_libraries(tag_editor_tests PRIVATE test_fixtures user32 gdi32 advapi32 comctl32)
target_compile_options(tag_editor_tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(tag_editor_tests in_opus)
add_test(NAME unified_tag_editor COMMAND tag_editor_tests "$<TARGET_FILE:in_opus>" "${CMAKE_CURRENT_BINARY_DIR}/unified-editor-data")
set_tests_properties(unified_tag_editor PROPERTIES TIMEOUT 90)

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
    "-DARCHIVES=$<TARGET_FILE:ogg>;$<TARGET_FILE:opus>;$<TARGET_FILE:opusfile>;$<TARGET_FILE:opus_decoder>;$<TARGET_FILE:speex_resampler>;$<TARGET_FILE:test_fixtures>"
    "-DCONFIG=$<CONFIG>"
    "-DLTO=$<AND:$<CONFIG:Release>,$<BOOL:${winamp_opus_ipo_supported}>>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_dynamic_crt.cmake")
set_tests_properties(dynamic_crt PROPERTIES TIMEOUT 30)

add_executable(advanced_playback_tests advanced_playback_tests.cpp ../src/metadata.cpp)
target_link_libraries(advanced_playback_tests PRIVATE test_fixtures ws2_32)
target_compile_options(advanced_playback_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME advanced_playback COMMAND advanced_playback_tests "${CMAKE_CURRENT_BINARY_DIR}/advanced-data")
set_tests_properties(advanced_playback PROPERTIES TIMEOUT 45)

add_executable(advanced_settings_tests advanced_settings_tests.cpp ../src/output_settings.cpp)
target_link_libraries(advanced_settings_tests PRIVATE test_fixtures user32 gdi32 advapi32)
target_compile_options(advanced_settings_tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(advanced_settings_tests in_opus)
add_test(NAME advanced_settings COMMAND advanced_settings_tests "$<TARGET_FILE:in_opus>" "${CMAKE_CURRENT_BINARY_DIR}/advanced-settings-data")
set_tests_properties(advanced_settings PROPERTIES TIMEOUT 30)

add_executable(native_codec_tests native_codec_tests.cpp)
target_link_libraries(native_codec_tests PRIVATE test_fixtures ws2_32)
target_compile_options(native_codec_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME native_codecs COMMAND native_codec_tests "${CMAKE_CURRENT_BINARY_DIR}/native-codec-data")
set_tests_properties(native_codecs PROPERTIES TIMEOUT 90 FIXTURES_SETUP native_codec_data)
add_executable(advanced_plugin_tests advanced_plugin_tests.cpp ../src/output_settings.cpp)
target_link_libraries(advanced_plugin_tests PRIVATE test_fixtures user32 gdi32 advapi32)
target_compile_options(advanced_plugin_tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(advanced_plugin_tests in_opus)
add_test(NAME advanced_plugin COMMAND advanced_plugin_tests "$<TARGET_FILE:in_opus>" "${CMAKE_CURRENT_BINARY_DIR}/native-codec-data")
set_tests_properties(advanced_plugin PROPERTIES TIMEOUT 60 DEPENDS native_codecs FIXTURES_REQUIRED native_codec_data)

# Features shared from the independent Ogg plugin, restricted to Ogg Opus.
add_executable(stream_playback_tests stream_playback_tests.cpp ../src/output_settings.cpp)
target_link_libraries(stream_playback_tests PRIVATE test_fixtures ws2_32 user32 advapi32)
target_compile_options(stream_playback_tests PRIVATE /W4 /permissive- /utf-8)
add_dependencies(stream_playback_tests in_opus)
add_test(NAME stream_playback_features COMMAND stream_playback_tests "$<TARGET_FILE:in_opus>" "${CMAKE_CURRENT_BINARY_DIR}/stream-feature-data")
set_tests_properties(stream_playback_features PROPERTIES TIMEOUT 60)

add_executable(processing_feature_tests processing_feature_tests.cpp)
target_link_libraries(processing_feature_tests PRIVATE test_fixtures)
target_compile_options(processing_feature_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME processing_features COMMAND processing_feature_tests "${CMAKE_CURRENT_BINARY_DIR}/processing-feature-data")
set_tests_properties(processing_features PROPERTIES TIMEOUT 90)
add_executable(gain_policy_tests gain_policy_tests.cpp)
target_link_libraries(gain_policy_tests PRIVATE opus_decoder)
target_compile_options(gain_policy_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME gain_policy COMMAND gain_policy_tests)
set_tests_properties(gain_policy PROPERTIES TIMEOUT 30)

add_executable(feature_settings_tests feature_settings_tests.cpp ../src/output_settings.cpp ../src/file_info_dialog.rc)
add_executable(host_feature_tests host_feature_tests.cpp ../src/output_settings.cpp)
add_executable(language_feature_tests language_feature_tests.cpp)
foreach(feature_target feature_settings_tests host_feature_tests language_feature_tests)
    target_link_libraries(${feature_target} PRIVATE opus_decoder)
    target_compile_options(${feature_target} PRIVATE /W4 /permissive- /utf-8)
    add_test(NAME ${feature_target} COMMAND ${feature_target})
    set_tests_properties(${feature_target} PROPERTIES TIMEOUT 30)
endforeach()

add_executable(tag_edit_tests tag_edit_tests.cpp)
add_executable(extended_read_tests extended_read_tests.cpp)
foreach(api_target tag_edit_tests extended_read_tests)
    target_link_libraries(${api_target} PRIVATE test_fixtures)
    target_compile_options(${api_target} PRIVATE /W4 /permissive- /utf-8)
    add_dependencies(${api_target} in_opus)
    add_test(NAME ${api_target} COMMAND ${api_target} "$<TARGET_FILE:in_opus>" "${CMAKE_CURRENT_BINARY_DIR}/${api_target}-data")
    set_tests_properties(${api_target} PROPERTIES TIMEOUT 90)
endforeach()

add_executable(transport_feature_tests transport_feature_tests.cpp)
target_link_libraries(transport_feature_tests PRIVATE test_fixtures ws2_32)
target_compile_options(transport_feature_tests PRIVATE /W4 /permissive- /utf-8)
add_test(NAME transport_features COMMAND transport_feature_tests "${CMAKE_CURRENT_BINARY_DIR}/transport-feature-data")
set_tests_properties(transport_features PROPERTIES TIMEOUT 90)

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
