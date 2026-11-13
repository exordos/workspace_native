cmake_minimum_required(VERSION 3.24)
project(WorkspaceNative VERSION 0.20.0 LANGUAGES CXX)

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
find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets Network Qml Quick QuickControls2 QuickDialogs2 Svg WebSockets Sql DBus Multimedia)
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
    src/native-deeplink.cpp src/native-deeplink.h
    src/native-chat-crypto.cpp src/native-chat-crypto.h
    src/native-chat-key-vault.cpp src/native-chat-key-vault.h
    src/native-preferences.cpp src/native-preferences.h
    src/native-auth-guard.cpp src/native-auth-guard.h
    src/native-instance.cpp src/native-instance.h
    src/native-updater.cpp src/native-updater.h
    src/native-cache.cpp src/native-cache.h
    src/native-cache-service.cpp src/native-cache-service.h
    src/native-draft-store.cpp src/native-draft-store.h
    src/native-draft-service.cpp src/native-draft-service.h
    src/native-drafts.cpp src/native-drafts.h
    src/native-notifications.cpp src/native-notifications.h
    src/call/jitsi-call.cpp src/call/jitsi-call.h
    src/call/native-call-store.cpp src/call/native-call-store.h
    src/call/native-call-window.cpp src/call/native-call-window.h
    src/call/incoming-call.cpp src/call/incoming-call.h
    src/call/incoming-call-lifecycle.cpp src/call/incoming-call-lifecycle.h
    src/media-viewer.cpp src/media-viewer.h
    src/session.cpp src/session.h
    src/session-vault.cpp src/session-vault.h
    src/realtime-client.cpp src/realtime-client.h
    src/native-controller.cpp src/native-controller.h)
target_include_directories(workspace-native-core PUBLIC src)
# Incoming invite UI variant, the native counterpart of the reference
# VITE_CALL_INCOMING_MODAL_VARIANT build flag (shared/lib/env.ts:132-135):
# "large" (default) or "compact"; invalid values fall back to "large" at
# runtime through resolveIncomingCallVariant.
set(WORKSPACE_CALL_INCOMING_MODAL_VARIANT "large" CACHE STRING
    "Incoming call invite UI variant: large | compact (invalid falls back to large)")
target_compile_definitions(workspace-native-core PRIVATE
    WORKSPACE_NATIVE_CALL_INCOMING_VARIANT="${WORKSPACE_CALL_INCOMING_MODAL_VARIANT}")
target_link_libraries(workspace-native-core PUBLIC Qt6::Core Qt6::Gui Qt6::Network Qt6::WebSockets Qt6::Sql)
target_link_libraries(workspace-native-core PRIVATE Qt6Keychain::Qt6Keychain md4c::md4c-html OpenSSL::Crypto Qt6::DBus)
# The notification attention adapter asks the window manager for attention
# over raw xcb on Linux; without the headers it degrades to a silent no-op.
include(CheckIncludeFileCXX)
check_include_file_cxx(xcb/xcb.h WORKSPACE_NATIVE_HAVE_XCB)
if(WORKSPACE_NATIVE_HAVE_XCB AND UNIX AND NOT APPLE)
    target_compile_definitions(workspace-native-core PRIVATE WORKSPACE_NATIVE_HAVE_XCB=1)
    target_link_libraries(workspace-native-core PRIVATE xcb)
endif()
qt_add_resources(workspace-native-core native_emoji PREFIX /workspace/data BASE resources
    FILES resources/unicode_emoji.json resources/emoji_picker.json
        resources/emoji_picker_navigation.svg resources/emoji_picker_search.svg
        resources/emoji_picker_clear.svg resources/emoji_picker_triangle.svg)

add_library(workspace-native-ui STATIC src/native-app.cpp src/native-app.h
    src/avatar-images.cpp src/avatar-images.h src/native-image-input.h
    src/message-images.cpp src/message-images.h)
target_link_libraries(workspace-native-ui PUBLIC workspace-native-core Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2 Qt6::Multimedia)
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
# Live external-API handshake scripts the embed view injects (issue #162
# round 4): the Qt WebChannel JS API (vendored qwebchannel.js) plus the
# in-page glue mapping meeting-frame events onto the embed bridge.
qt_add_resources(workspace-native-ui native_call_bridge_js PREFIX /workspace/qml
    BASE qml/web FILES qml/web/qwebchannel.js qml/web/workspace-call-events.js)
set(native_qml_files
qml/Main.qml qml/NativeTheme.qml qml/NativeIconButton.qml qml/NativeButton.qml qml/NativeField.qml
qml/NativeJitsiCard.qml
qml/NativeCallWindow.qml qml/NativeIncomingCallWindow.qml qml/ShellIconButton.qml qml/NativeCallWebEngineView.qml
qml/SessionSwitcher.qml
    qml/NativeReactionPicker.qml qml/NativeQuickReactions.qml qml/NativeAvatarImage.qml qml/NativeAboutDrawer.qml qml/NativeRightDrawer.qml qml/NativeMessageImage.qml qml/NativeMediaViewer.qml qml/NativeDraftsPage.qml qml/NativeMentionsPage.qml qml/NativeInboxPage.qml qml/NativeChannelInfo.qml qml/NativeUserProfile.qml)
foreach(qml_file IN LISTS native_qml_files)
    get_filename_component(qml_name "${qml_file}" NAME)
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
endforeach()
set(native_icons chat_bubble calendar mail folders profile channels add attach smile
    composer-send composer-bottom-panel-open composer-bottom-panel-close
    composer-add-link composer-format-bold composer-format-italic composer-strikethrough
    composer-list-numbered composer-list-bulleted composer-chat-dashed composer-frame-source
    composer-inline-code composer-visibility
    composer-emoji composer-attach-compact side_panel phone chevron-down close check
    call_incoming_video_toggle
    reply copy pen delete plus star logout search new_window mail_activity_compact home marker_outline alternate_email drafts_compact chat_lock info warning download chevron-right
    at bell bell_off person_add
    stylus images border_color links business_center handshake account_circle group
    globe_location_pin schedule calendar_month celebration videos)
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
# Optional compiled-in default-organization shortcut for the login wizard
# (docs/NATIVE_DESKTOP_LOGIN.md). Empty keeps the block hidden, matching the
# reference gate on DEFAULT_LOGIN_ORGANIZATION_URL.
set(WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_URL "" CACHE STRING
    "Default login organization origin compiled into the binary")
set(WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_NAME "" CACHE STRING
    "Display name for the default login organization shortcut")
foreach(default_login_org IN ITEMS URL NAME)
    if(NOT WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_${default_login_org} STREQUAL "")
        target_compile_definitions(workspace-native-ui PRIVATE
            WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_${default_login_org}="${WORKSPACE_NATIVE_DEFAULT_LOGIN_ORGANIZATION_${default_login_org}}")
    endif()
endforeach()
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
    find_package(Qt6 REQUIRED COMPONENTS Test DBus)
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
    foreach(test api-client domain workspace-reference file-client avatar-images message-images native-design native-markdown native-chat-crypto native-chat-key-vault native-chat-runtime native-preferences native-auth-guard native-locale native-instance native-cache native-cache-service native-cache-preferences native-draft-store native-draft-service native-drafts native-draft-sync session session-vault realtime-client native-controller native-window native-desktop native-call)
        add_executable(${test}-test tests/${test}-test.cpp)
        target_link_libraries(${test}-test PRIVATE workspace-native-ui Qt6::Test Qt6::DBus)
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
        if(test STREQUAL "native-call")
            # #250/#251/#252 source-contract guards read the shipped embed
            # sources from the source tree (the same files qrc embeds).
            target_compile_definitions(${test}-test PRIVATE
                WORKSPACE_NATIVE_CALL_QML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
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
            # Whole-group runtime on the GPU-less reference stand measures 82.9-94.2s
            # across solo runs, and the suite-context run was killed at the old 90s
            # ceiling after printing PASS for the whole list. 150s = worst measured
            # runtime plus suite-load headroom (issues #16/#17/#18/#22: never tune a
            # ceiling to single-digit margins of one host's runtime).
            set_tests_properties(native-${test} PROPERTIES TIMEOUT 150)
        elseif(test STREQUAL "native-window")
            # Include the direct-chat keyboard, recovery and rendering cases.
            # Whole-group runtime measures 365.8s solo on the GPU-less host
            # (373 cases, 0 failed, issue #197 gate), already above the old
            # 360s ceiling that made the group a deterministic timeout. 480s =
            # worst measured runtime plus suite-load headroom (#16/#103: never
            # tune a ceiling to single-digit margins of one host's runtime).
            set_tests_properties(native-${test} PROPERTIES TIMEOUT 480)
        elseif(test STREQUAL "native-drafts")
            # Full record/byte/metadata pressure plus retained-operation cases.
            set_tests_properties(native-${test} PROPERTIES TIMEOUT 90)
        elseif(test STREQUAL "native-draft-sync")
            set_tests_properties(native-${test} PROPERTIES TIMEOUT 120)
        endif()
    endforeach()
    add_test(NAME native-native-draft-workload COMMAND native-window-test)
    set_tests_properties(native-native-draft-workload PROPERTIES TIMEOUT 180
        ENVIRONMENT "CASSI_NATIVE_DRAFT_WORKLOAD=1;CASSI_NATIVE_DRAFT_WORKLOAD_REPORT=${CMAKE_CURRENT_BINARY_DIR}/Testing/native-draft-workload.json")
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
        uiEncryptedMessageDecryptRetryRowCyclesAndRepairs
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
    # The explicit UI list measured 310s green and 548s when failing QTRY waits
    # added time on the GPU-less reference stand (llvmpipe, bare Xvnc 1440x900
    # plus openbox), so the old 360s ceiling timeout-killed healthy runs there.
    # A timeout kill prints no further result and was misread as an
    # order-dependent stall (issues #17 and #18). Worst observed 548s plus 25%
    # headroom rounded up to 30s: 720s. Running this binary unfiltered also
    # executes the 274 core rows in the same process (measured 893s total), so
    # keep the explicit function list here and budget about 900s for direct
    # full-binary runs. Individual assertion waits stay unchanged.
    set_tests_properties(native-native-e2ee-ui PROPERTIES TIMEOUT 720)
    if(UNIX AND NOT APPLE)
        # Run only against staging through verify-installed-runtime.py --probe.
        # This helper is not installed into the delivered application.
        add_executable(native-runtime-probe tests/native-runtime-probe.cpp)
        target_link_libraries(native-runtime-probe PRIVATE workspace-native-core OpenSSL::SSL)
    endif()
    option(WORKSPACE_NATIVE_PLATFORM_TESTS "Enable checks that access the real OS secure store" OFF)
    if(WORKSPACE_NATIVE_PLATFORM_TESTS)
        add_executable(secure-store-platform-test tests/secure-store-platform-test.cpp)
        target_link_libraries(secure-store-platform-test PRIVATE workspace-native-core Qt6::Test Qt6::DBus)
        add_test(NAME native-secure-store-platform COMMAND secure-store-platform-test)
        # The issue #28 scenarios add three more keyring daemon start/stop cycles
        # plus one deliberate 15 s interactive-wait sample on top of the original
        # flows, so the old 150 s ceiling no longer leaves headroom on slow hosts.
        set_tests_properties(native-secure-store-platform PROPERTIES TIMEOUT 300 LABELS "platform;secure-store")
        add_executable(native-chat-key-vault-platform-test tests/native-chat-key-vault-platform-test.cpp)
        target_link_libraries(native-chat-key-vault-platform-test PRIVATE workspace-native-core Qt6::Test)
        add_test(NAME native-chat-key-vault-platform COMMAND native-chat-key-vault-platform-test)
        set_tests_properties(native-chat-key-vault-platform PROPERTIES TIMEOUT 150 LABELS "platform;secure-store")
    endif()
    # Two-tier test gate (issue #150).
    # gui label: suites that need a real window manager (Xvnc + openbox) because
    # they measure window geometry, palette resolution or drive real input.
    # The headless quick gate runs everything else in parallel offscreen:
    #   QT_QPA_PLATFORM=offscreen ctest -j 8 -LE gui
    # RESOURCE_LOCK "xdisplay": window-driving suites must never overlap on a
    # shared display (focus and keyboard-grab races), so a parallel pre-release
    # run (DISPLAY=:30 ctest -j 6) serializes only these tests and runs the
    # remaining suites concurrently.
    set_tests_properties(native-native-window native-native-draft-workload native-native-e2ee-ui native-native-preferences
        PROPERTIES LABELS "gui" RESOURCE_LOCK "xdisplay")
    set_tests_properties(native-native-desktop native-native-e2ee-read
        PROPERTIES RESOURCE_LOCK "xdisplay")
endif()

# Throwaway Jitsi embed spike for docs/NATIVE_DESKTOP_CALL_STACK.md (#160).
# Default OFF: the default build tree never sees Qt6::WebEngineQuick, and the
# spike registers nothing in CTest. Enabled only with
# -DWORKSPACE_NATIVE_CALL_SPIKE=ON in a separate build directory.
option(WORKSPACE_NATIVE_CALL_SPIKE
    "Build the throwaway Jitsi embed spike (no tests, no default-build impact)" OFF)
if(WORKSPACE_NATIVE_CALL_SPIKE)
    find_package(Qt6 REQUIRED COMPONENTS WebEngineQuick WebEngineCore)
    qt_add_executable(native-call-spike MANUAL_FINALIZATION spike/call/main.cpp)
    target_link_libraries(native-call-spike PRIVATE Qt6::Quick Qt6::WebEngineQuick Qt6::WebEngineCore)
    qt_add_resources(native-call-spike spike_call_files PREFIX /spike/call BASE spike/call FILES
        spike/call/CallSpike.qml spike/call/probe.html spike/call/jitsi.html
        spike/call/jitsi-loader-attempt.html spike/call/vendor/jitsi-external_api.js)
    qt_finalize_executable(native-call-spike)
endif()
