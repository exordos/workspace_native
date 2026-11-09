cmake_minimum_required(VERSION 3.24)
project(WorkspaceNative VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
include(CTest)
include(GNUInstallDirs)
if(UNIX AND NOT APPLE)
    # Distribution Qt libraries may have no RPATH. Bundle deployment must be able
    # to add one, including to the copied libraries' transitive dependencies.
    find_program(WORKSPACE_NATIVE_PATCHELF_EXECUTABLE patchelf REQUIRED)
    set(QT_DEPLOY_USE_PATCHELF ON)
endif()
find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets Network Qml Quick QuickControls2 QuickDialogs2 Svg WebSockets Sql)
find_package(Qt6Keychain REQUIRED)
find_package(md4c CONFIG REQUIRED)
find_package(OpenSSL 3.2 REQUIRED COMPONENTS Crypto SSL)
qt_standard_project_setup(REQUIRES 6.8)

add_library(workspace-native-core STATIC
    src/api-client.cpp src/api-client.h
    src/domain.cpp src/domain.h
    src/workspace-reference.cpp src/workspace-reference.h
    src/file-client.cpp src/file-client.h
    src/native-markdown.cpp src/native-markdown.h
    src/native-chat-crypto.cpp src/native-chat-crypto.h
    src/native-chat-key-vault.cpp src/native-chat-key-vault.h
    src/native-preferences.cpp src/native-preferences.h
    src/native-instance.cpp src/native-instance.h
    src/native-updater.cpp src/native-updater.h
    src/native-cache.cpp src/native-cache.h
    src/native-cache-service.cpp src/native-cache-service.h
    src/native-draft-store.cpp src/native-draft-store.h
    src/native-draft-service.cpp src/native-draft-service.h
    src/session.cpp src/session.h
    src/session-vault.cpp src/session-vault.h
    src/realtime-client.cpp src/realtime-client.h
    src/native-controller.cpp src/native-controller.h)
target_include_directories(workspace-native-core PUBLIC src)
target_link_libraries(workspace-native-core PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::WebSockets Qt6::Sql)
target_link_libraries(workspace-native-core PRIVATE Qt6Keychain::Qt6Keychain md4c::md4c-html OpenSSL::Crypto)
qt_add_resources(workspace-native-core native_emoji PREFIX /workspace/data BASE resources
    FILES resources/unicode_emoji.json resources/emoji_picker.json
        resources/emoji_picker_navigation.svg resources/emoji_picker_search.svg
        resources/emoji_picker_clear.svg resources/emoji_picker_triangle.svg)

add_library(workspace-native-ui STATIC src/native-app.cpp src/native-app.h
    src/avatar-images.cpp src/avatar-images.h src/native-image-input.h
    src/message-images.cpp src/message-images.h)
target_link_libraries(workspace-native-ui PUBLIC workspace-native-core Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2)
add_library(workspace-native-desktop STATIC src/native-desktop.cpp src/native-desktop.h)
target_include_directories(workspace-native-desktop PUBLIC src)
target_link_libraries(workspace-native-desktop PUBLIC Qt6::Widgets)
qt_add_resources(workspace-native-desktop native_tray PREFIX /workspace/tray
    BASE ../electron/resources FILES
        ../electron/resources/tray-icon-linux.png ../electron/resources/tray-icon-linux-unread.png
        ../electron/resources/tray-icon-mac.png ../electron/resources/tray-icon-mac-unread.png
        ../electron/resources/tray-icon.png ../electron/resources/tray-icon-unread.png)
get_target_property(native_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(native_qt_tools "${native_qmake}" DIRECTORY)
find_program(WORKSPACE_NATIVE_LRELEASE NAMES lrelease-qt6 lrelease6 lrelease
    HINTS "${native_qt_tools}" /usr/lib/qt6/bin REQUIRED)
set(native_ru_qm "${CMAKE_CURRENT_BINARY_DIR}/translations/workspace_ru.qm")
add_custom_command(OUTPUT "${native_ru_qm}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/translations"
    COMMAND "${WORKSPACE_NATIVE_LRELEASE}" "${CMAKE_CURRENT_SOURCE_DIR}/translations/workspace_ru.ts"
        -qm "${native_ru_qm}"
    DEPENDS translations/workspace_ru.ts VERBATIM)
set_source_files_properties("${native_ru_qm}" PROPERTIES GENERATED TRUE)
qt_add_resources(workspace-native-ui native_translations PREFIX /workspace/i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}/translations" FILES "${native_ru_qm}")
set(native_qml_files
    qml/Main.qml qml/NativeTheme.qml qml/NativeIconButton.qml qml/NativeButton.qml qml/NativeField.qml
    qml/NativeReactionPicker.qml qml/NativeQuickReactions.qml qml/NativeAvatarImage.qml qml/NativeAboutDrawer.qml qml/NativeMessageImage.qml)
foreach(qml_file IN LISTS native_qml_files)
    get_filename_component(qml_name "${qml_file}" NAME)
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
endforeach()
set(native_icons chat_bubble calendar mail folders profile channels add attach smile
    composer-send composer-bottom-panel-open composer-bottom-panel-close
    composer-emoji composer-attach-compact side_panel phone chevron-down close check
    reply copy pen delete plus star logout search new_window mail_activity_compact home marker_outline alternate_email drafts_compact chat_lock info warning download chevron-right)
set(native_icon_files)
foreach(icon IN LISTS native_icons)
    list(APPEND native_icon_files "../web/src/shared/assets/icons/${icon}.svg")
endforeach()
# Reuse the Source service icons without duplicating its asset files.
foreach(icon key-granted key-rejected)
    set(service_icon "../web/src/features/encrypted-chat-message/assets/${icon}.svg")
    set_source_files_properties("${service_icon}" PROPERTIES QT_RESOURCE_ALIAS "${icon}.svg")
    list(APPEND native_icon_files "${service_icon}")
endforeach()
qt_add_resources(workspace-native-ui native_icons PREFIX /workspace/icons
    BASE ../web/src/shared/assets/icons FILES ${native_icon_files})
qt_add_resources(workspace-native-ui native_brand PREFIX /workspace/brand
    BASE ../web/public FILES ../web/public/organization-fallback.svg ../web/public/favicon.svg)
qt_add_resources(workspace-native-ui native_app_icon PREFIX /workspace/brand
    BASE ../electron/resources FILES ../electron/resources/icon.png)

qt_add_executable(workspace-native WIN32 MACOSX_BUNDLE src/main.cpp)
target_compile_definitions(workspace-native PRIVATE WORKSPACE_NATIVE_VERSION="${PROJECT_VERSION}")
target_link_libraries(workspace-native PRIVATE workspace-native-ui workspace-native-desktop OpenSSL::SSL)
qt_add_qml_module(workspace-native
    URI Exordos.Workspace.Native
    RESOURCE_PREFIX /workspace/qml
    NO_RESOURCE_TARGET_PATH
    NO_GENERATE_QMLTYPES
    QML_FILES ${native_qml_files})
set_target_properties(workspace-native PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Workspace Native"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.exordos.workspace.native.preview"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")

if(WIN32)
    enable_language(RC)
    set(WORKSPACE_NATIVE_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../electron/resources/icon.ico")
    configure_file(resources/workspace-native.rc.in workspace-native.rc @ONLY)
    target_sources(workspace-native PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/workspace-native.rc")
elseif(APPLE)
    set(native_macos_icon "${CMAKE_CURRENT_SOURCE_DIR}/../electron/resources/icon.icns")
    target_sources(workspace-native PRIVATE "${native_macos_icon}")
    set_source_files_properties("${native_macos_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(workspace-native PROPERTIES MACOSX_BUNDLE_ICON_FILE icon.icns)
endif()

foreach(target workspace-native-core workspace-native-ui workspace-native-desktop workspace-native)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

install(TARGETS workspace-native BUNDLE DESTINATION . RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ../../LICENSE resources/unicode_emoji.LICENSE resources/emoji_picker.LICENSE resources/md4c.LICENSE resources/openssl.LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/workspace-native-preview)
if(UNIX AND NOT APPLE)
    install(IMPORTED_RUNTIME_ARTIFACTS md4c::md4c md4c::md4c-html
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    # Keep the HPKE provider and Qt's TLS backend on the same OpenSSL runtime.
    # FindOpenSSL exposes UNKNOWN imported targets, so install the actual files
    # rather than a developer .so symlink or IMPORTED_RUNTIME_ARTIFACTS.
    foreach(crypto_target OpenSSL::Crypto OpenSSL::SSL)
        get_target_property(crypto_library ${crypto_target} IMPORTED_LOCATION)
        file(REAL_PATH "${crypto_library}" crypto_runtime_library)
        install(FILES "${crypto_runtime_library}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endforeach()
    install(FILES resources/workspace-native.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
        RENAME com.exordos.workspace.native.preview.desktop)
    install(FILES ../electron/resources/icon.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
        RENAME com.exordos.workspace.native.preview.png)
endif()
set(native_deploy_plugin_args)
if(UNIX AND NOT APPLE)
    # Qt's default Linux deployment can select only XCB, even on a Wayland
    # desktop. Include the Wayland platform and its dynamically loaded helpers.
    list(APPEND native_deploy_plugin_args
        NO_TRANSLATIONS
        INCLUDE_PLUGINS qwayland qsvg qsvgicon
        INCLUDE_PLUGIN_TYPES wayland-shell-integration
            wayland-graphics-integration-client wayland-decoration-client)
endif()
qt_generate_deploy_qml_app_script(TARGET workspace-native
    OUTPUT_SCRIPT deploy_script MACOS_BUNDLE_POST_BUILD ${native_deploy_plugin_args})
install(SCRIPT ${deploy_script})
if(UNIX AND NOT APPLE)
    # The app currently supports English/source strings and Russian. Deploy
    # only the reviewed Qt locale instead of every unrelated SDK catalog.
    qt_generate_deploy_script(TARGET workspace-native OUTPUT_SCRIPT native_translations_script
        CONTENT "qt_deploy_translations(LOCALES ru)")
    install(SCRIPT ${native_translations_script})
    configure_file(cmake/FinalizeLinuxRuntime.cmake.in FinalizeLinuxRuntime.cmake @ONLY)
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/FinalizeLinuxRuntime.cmake")
endif()

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(native-updater-test tests/native-updater-test.cpp src/native-updater.cpp src/native-updater.h)
    target_include_directories(native-updater-test PRIVATE src)
    target_link_libraries(native-updater-test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
    add_test(NAME native-updater COMMAND native-updater-test)
    set_tests_properties(native-updater PROPERTIES TIMEOUT 120)

    # Real About leaf policy with public QObject adapters; no network/window.
    add_executable(native-about-policy-test tests/native-about-policy-test.cpp)
    target_link_libraries(native-about-policy-test PRIVATE workspace-native-ui Qt6::Test)
    qt_add_resources(native-about-policy-test native_about_policy_qml
        PREFIX /workspace/qml BASE qml FILES
            qml/NativeAboutDrawer.qml qml/NativeIconButton.qml qml/NativeTheme.qml)
    add_test(NAME native-about-policy COMMAND native-about-policy-test)
    set_tests_properties(native-about-policy PROPERTIES TIMEOUT 30)

    find_program(WORKSPACE_NATIVE_LUPDATE NAMES lupdate-qt6 lupdate6 lupdate
        HINTS "${native_qt_tools}" /usr/lib/qt6/bin REQUIRED)
    foreach(test api-client domain workspace-reference file-client avatar-images message-images native-design native-markdown native-chat-crypto native-chat-key-vault native-chat-runtime native-preferences native-locale native-instance native-cache native-cache-service native-cache-preferences native-draft-store native-draft-service session session-vault realtime-client native-controller native-window native-desktop)
        add_executable(${test}-test tests/${test}-test.cpp)
        target_link_libraries(${test}-test PRIVATE workspace-native-ui Qt6::Test)
        if(test STREQUAL "native-desktop")
            target_link_libraries(${test}-test PRIVATE workspace-native-desktop)
        endif()
        if(test STREQUAL "native-locale")
            target_compile_definitions(${test}-test PRIVATE
                WORKSPACE_NATIVE_LUPDATE_EXECUTABLE="${WORKSPACE_NATIVE_LUPDATE}")
        endif()
        if(test STREQUAL "native-chat-crypto")
            target_compile_definitions(${test}-test PRIVATE
                WORKSPACE_NATIVE_CRYPTO_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/tests/workspace-chat-crypto-v1-vectors.json")
        endif()
        # Only the window harness loads the scene; the production executable embeds
        # the same sources as a QML module so deployment scans its actual imports.
        if(test STREQUAL "native-window")
            target_link_libraries(${test}-test PRIVATE Qt6::Widgets)
            qt_add_resources(${test}-test native_qml
                PREFIX /workspace/qml BASE qml FILES ${native_qml_files})
        endif()
        set_target_properties(${test}-test PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
        add_test(NAME native-${test} COMMAND ${test}-test)
        set_tests_properties(native-${test} PROPERTIES TIMEOUT 45)
        if(test STREQUAL "native-controller")
            # Include Archive/placement and creation HTTP/lifecycle cases in the complete suite.
            # This is the whole-suite budget, not a request timeout.
            set_tests_properties(native-${test} PROPERTIES TIMEOUT 700)
        elseif(test STREQUAL "native-cache")
            # The complete 69-case SAN run takes 49.4s; individual waits are unchanged.
            set_tests_properties(native-${test} PROPERTIES TIMEOUT 90)
        elseif(test STREQUAL "native-window")
            # Include the direct-chat keyboard, recovery and rendering cases.
            set_tests_properties(native-${test} PROPERTIES TIMEOUT 360)
        endif()
    endforeach()
    # Test-only real TLS/source-JS grant workflow; no runtime/download dependency.
    find_program(WORKSPACE_NATIVE_E2EE_NODE_EXECUTABLE node REQUIRED)
    find_program(WORKSPACE_NATIVE_E2EE_OPENSSL_EXECUTABLE openssl REQUIRED)
    # Actual local TLS/h2 transport regressions. Existing QtTest failOnWarning policy is untouched.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_executable(native-updater-http2-recovery-driver tests/updater-http2/recovery.cpp src/native-updater.cpp src/native-updater.h)
        target_include_directories(native-updater-http2-recovery-driver PRIVATE src)
        target_link_libraries(native-updater-http2-recovery-driver PRIVATE Qt6::Core Qt6::Network OpenSSL::SSL)
        add_executable(native-updater-http2-controls-driver tests/updater-http2/controls.cpp src/native-updater.cpp src/native-updater.h)
        target_include_directories(native-updater-http2-controls-driver PRIVATE src)
        target_link_libraries(native-updater-http2-controls-driver PRIVATE Qt6::Core Qt6::Network OpenSSL::SSL)
        foreach(case recovery two-fail reset-cap http-403 http-404 http-429 http-503 schema postheaders tls http1 cancel destroy replacement deadline ipv6 archive-policy endpoint-admission)
            add_test(NAME "native-updater-http2-${case}" COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/updater-http2/run.py"
                --case "${case}" --driver "$<TARGET_FILE:native-updater-http2-controls-driver>"
                --recovery-driver "$<TARGET_FILE:native-updater-http2-recovery-driver>"
                --updater-source-dir "${CMAKE_CURRENT_SOURCE_DIR}/src"
                --node "${WORKSPACE_NATIVE_E2EE_NODE_EXECUTABLE}" --openssl "${WORKSPACE_NATIVE_E2EE_OPENSSL_EXECUTABLE}"
                --output-root "${CMAKE_CURRENT_BINARY_DIR}/Testing/updater-h2")
            set_tests_properties("native-updater-http2-${case}" PROPERTIES TIMEOUT 180 LABELS "updater;transport;tls;http2")
        endforeach()
    endif()
    add_executable(native-e2ee-read-test tests/native-e2ee-read-test.cpp)
    target_link_libraries(native-e2ee-read-test PRIVATE workspace-native-core Qt6::Test)
    target_compile_definitions(native-e2ee-read-test PRIVATE
        WORKSPACE_NATIVE_E2EE_NODE_EXECUTABLE="${WORKSPACE_NATIVE_E2EE_NODE_EXECUTABLE}"
        WORKSPACE_NATIVE_E2EE_OPENSSL_EXECUTABLE="${WORKSPACE_NATIVE_E2EE_OPENSSL_EXECUTABLE}"
        WORKSPACE_NATIVE_E2EE_PEER_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tests/e2ee-peer/peer.mjs")
    set_target_properties(native-e2ee-read-test PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    add_test(NAME native-native-e2ee-read COMMAND native-e2ee-read-test)
    # Original189 cases plus42 additive observer/resolver/lifecycle/disclosure data.
    # Measured old suite359.164s + new42 focus130.768s + RT control17.018s;
    # 25% headroom rounded up to30s:660s. Individual waits stay unchanged.
    set_tests_properties(native-native-e2ee-read PROPERTIES TIMEOUT 660)
    # Reuse the real TLS/source-JS fixture for the visible encrypted UI slice.
    # The explicit functions keep the existing core and pulse suite separate.
    add_executable(native-e2ee-ui-test tests/native-e2ee-read-test.cpp)
    target_link_libraries(native-e2ee-ui-test PRIVATE workspace-native-ui Qt6::Widgets Qt6::Test)
    target_compile_definitions(native-e2ee-ui-test PRIVATE
        WORKSPACE_NATIVE_E2EE_UI=1
        WORKSPACE_NATIVE_E2EE_NODE_EXECUTABLE="${WORKSPACE_NATIVE_E2EE_NODE_EXECUTABLE}"
        WORKSPACE_NATIVE_E2EE_OPENSSL_EXECUTABLE="${WORKSPACE_NATIVE_E2EE_OPENSSL_EXECUTABLE}"
        WORKSPACE_NATIVE_E2EE_PEER_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tests/e2ee-peer/peer.mjs")
    qt_add_resources(native-e2ee-ui-test native_qml
        PREFIX /workspace/qml BASE qml FILES ${native_qml_files})
    set_target_properties(native-e2ee-ui-test PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    add_test(NAME native-native-e2ee-ui COMMAND native-e2ee-ui-test
        uiEncryptedReadUsesActualSourceGrantAndSafeCopy
        uiEncryptedInlineImageRequiresReadableBodyAndResetsItsDemand
        uiEncryptedMissingErrorArchiveAndNullCurrentRemainDistinct
        uiEncryptedOsRetryDoesNotSendAKeyRequest
        uiEncryptedCheckIsGetOnlyAndRetryKeepsTheSameIntent
        uiEncryptedSourceHeaderGeometry
        uiEncryptedSourceLockedGeometry
        uiEncryptedSourceLockedPaint
        uiEncryptedReadableFooterHasOneAvailabilityNotice
        uiEncryptedSourceHeaderAvatarUsesDefaultThemeCircle
        uiEncryptedSourceSidebarAvatarUsesLockFallback
        uiEncryptedComposerSendsActualSourceCipher
        uiNewChatEncryptedCanonicalDirectUsesActualSourceGrantReadAndSend
        uiEncryptedComposerEmojiUsesActualSourcePeer
        uiEncryptedComposerFormattingUsesActualSourcePeer
        uiEncryptedComposerLockedKeyNeverFallsBackToPlaintext
        uiOrdinaryComposerPreservesMarkdownAndAggregatePrompt
        uiEncryptedComposerCheckAndRetryKeepFrozenCipher
        uiEncryptedComposerCurrentKeyWithoutHistoryHasNoOutdatedNotice
        uiEncryptedComposerClosedTopicKeepsDisabledSourceComposer
        uiEncryptedClosedHistoricalComposerKeepsKeyUpdateRequest
        uiEncryptedSourceKeyNoticePaint
        uiEncryptedSourceKeyNoticeGeometry
        uiIncomingKeyRequestCentersWrappedLinesAtMinimumWidth
        uiIncomingKeyRequestUsesActualNativeResponseButtons
        uiIncomingKeyRequestKeepsDisabledActionLabelsWhilePending
        uiOwnCurrentDeviceRequestHasNoInlineIssuerActions
        uiReceivedKeyServiceResponseUsesSourceTextAndRequest
        uiKeyServiceResponseHasNoActionWithoutCurrentRecipient
        uiKeyServiceResponseUsesCanonicalSelfAndOutgoingGrammar
        uiRejectedKeyResponseRefreshesAfterFreshIssuerAuthority
        uiServiceInitialHistoryDisclosureUsesRealKeyboardAndUnmountsBodies
        uiServiceHistoryCommitAndRealtimeHaveDifferentExpansion
        uiServiceAcceptedHistoryIsSemanticallyReadyBeforeQueuedViewport
        uiServiceExpansionPredicateRejectsAnActuallyAbsentDelegate
        uiServiceLargeCollapsedHistoryHasOneDelegateAndNoMissingRFetch
        uiServiceProductionReducerPreservesManualMergeFocusAndContext
        uiServiceManualChoiceFollowsActualSplitMergeAndHeadDeletion
        uiEncryptedWindowTeardownWaitsForVisibleIncubationBeforeHide
        uiServiceAutomaticOffscreenCollapsePreservesAnchorAndManualChoice
        uiCollapsedServiceReadBoundaryUsesLatestActualSameTopicUuid
        uiServiceAccountFilterPrecedesAuthorAndTopicGrouping
        uiServiceRunBoundariesUseActualAuthorTopicAndLocalDay
        uiServiceAnnouncementRequestsOnlyItsExactMissingGeneration
        uiCollapsedServiceRunPreservesSourceOuterGroupGap
        uiMissingRequestKeyboardDisclosureHydratesTheActualSourceResponse
        uiMissingRequestSharedRSurvivesFirstConsumerDeletionAndCollapse
        uiMissingRequestWorkerFallbackRemainsVisibleAfterFailedRefresh
        uiMissingRequestGenuineRuntimeResetReassertsAnUnchangedMountedBody
        uiMissingRequestOwnerReplacementCannotPopulateTheNewOwnersBody
        uiMissingRequestOnlyANewSemanticDisclosureCanRetryFailure
        uiMissingRequestSelectionReducerRemountGetsANewDisclosure
        uiOrdinaryAccountRecipientIsPublishedBeforeAnyServiceBodyMount
        uiOrdinaryManagerGrantsFromTheActualMainAction
        uiOrdinaryManagerRejectsFromTheActualMainAction
        uiOrdinaryKeyResponseRepeatsTheSameMainActionWithoutReplacingItsFrozenWire
        uiOrdinaryRetainedResponseCannotEnableAnotherRequestOrItsOppositeAction)
    # Original108 cases plus13 mounted resolver/disclosure data. Measured old108
    # 216.090s +new15 focused65.412s, with25% headroom rounded up to30s:360s.
    # Individual assertion waits stay unchanged.
    set_tests_properties(native-native-e2ee-ui PROPERTIES TIMEOUT 360)
    if(UNIX AND NOT APPLE)
        # Run only against staging through verify-installed-runtime.py --probe.
        # This helper is not installed into the delivered application.
        add_executable(native-runtime-probe tests/native-runtime-probe.cpp)
        target_link_libraries(native-runtime-probe PRIVATE workspace-native-core OpenSSL::SSL)
    endif()
    option(WORKSPACE_NATIVE_PLATFORM_TESTS "Enable checks that access the real OS secure store" OFF)
    if(WORKSPACE_NATIVE_PLATFORM_TESTS)
        add_executable(secure-store-platform-test tests/secure-store-platform-test.cpp)
        target_link_libraries(secure-store-platform-test PRIVATE workspace-native-core Qt6::Test)
        add_test(NAME native-secure-store-platform COMMAND secure-store-platform-test)
        set_tests_properties(native-secure-store-platform PROPERTIES TIMEOUT 150 LABELS "platform;secure-store")
        add_executable(native-chat-key-vault-platform-test tests/native-chat-key-vault-platform-test.cpp)
        target_link_libraries(native-chat-key-vault-platform-test PRIVATE workspace-native-core Qt6::Test)
        add_test(NAME native-chat-key-vault-platform COMMAND native-chat-key-vault-platform-test)
        set_tests_properties(native-chat-key-vault-platform PROPERTIES TIMEOUT 150 LABELS "platform;secure-store")
    endif()
endif()
