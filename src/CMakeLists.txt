cmake_minimum_required(VERSION 3.21)

project(Pilon VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
include(cmake/SourceLayout.cmake)

FetchContent_Declare(libvterm
    URL https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz
    URL_HASH SHA256=09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(libvterm)

add_library(pilon_vterm STATIC
    ${libvterm_SOURCE_DIR}/src/encoding.c
    ${libvterm_SOURCE_DIR}/src/keyboard.c
    ${libvterm_SOURCE_DIR}/src/mouse.c
    ${libvterm_SOURCE_DIR}/src/parser.c
    ${libvterm_SOURCE_DIR}/src/pen.c
    ${libvterm_SOURCE_DIR}/src/screen.c
    ${libvterm_SOURCE_DIR}/src/state.c
    ${libvterm_SOURCE_DIR}/src/unicode.c
    ${libvterm_SOURCE_DIR}/src/vterm.c
)
target_include_directories(pilon_vterm PUBLIC ${libvterm_SOURCE_DIR}/include)
set_target_properties(pilon_vterm PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

set(PILON_QT_COMPONENTS Concurrent Gui GuiPrivate Multimedia Network Quick QuickControls2 QuickDialogs2 Sql Widgets)
if(ANDROID)
    list(APPEND PILON_QT_COMPONENTS CorePrivate)
else()
    list(APPEND PILON_QT_COMPONENTS DBus)
endif()
find_package(Qt6 6.10 REQUIRED COMPONENTS ${PILON_QT_COMPONENTS})
if(ANDROID)
    # androiddeployqt scans source directories recursively, including unrelated QML tests.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_target_property(PILON_QML_SCANNER_CONFIGS Qt6::qmlimportscanner IMPORTED_CONFIGURATIONS)
    foreach(config IN LISTS PILON_QML_SCANNER_CONFIGS)
        get_target_property(scanner Qt6::qmlimportscanner "IMPORTED_LOCATION_${config}")
        set(filtered_scanner "${CMAKE_CURRENT_BINARY_DIR}/qmlimportscanner-${config}")
        file(CONFIGURE OUTPUT "${filtered_scanner}" CONTENT
            "#!/bin/sh\nexec \"${Python3_EXECUTABLE}\" \"${CMAKE_CURRENT_SOURCE_DIR}/../src/scripts/android-qml-importscanner.py\" \"${scanner}\" \"$@\"\n")
        file(CHMOD "${filtered_scanner}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
        set_property(TARGET Qt6::qmlimportscanner PROPERTY "IMPORTED_LOCATION_${config}" "${filtered_scanner}")
    endforeach()
    include(cmake/AndroidDependencies.cmake)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBSSH2 REQUIRED IMPORTED_TARGET libssh2)
    set(PILON_SSH2_TARGET PkgConfig::LIBSSH2)
    set(PILON_PLATFORM_LIBRARIES util)
    set(PILON_CALENDAR_PLATFORM_LIBRARIES Qt6::DBus)
endif()

# iCalendar recurrence and CalDAV payloads use a standards implementation.
if(NOT ANDROID)
    pkg_check_modules(LIBICAL QUIET IMPORTED_TARGET libical)
endif()
if(LIBICAL_FOUND)
    set(PILON_ICAL_TARGET PkgConfig::LIBICAL)
else()
    set(LIBICAL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ICAL_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(ICAL_GLIB OFF CACHE BOOL "" FORCE)
    set(ICAL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ICAL_ALLOW_EMPTY_VCALENDAR ON CACHE BOOL "" FORCE)
    set(SHARED_ONLY OFF CACHE BOOL "" FORCE)
    set(STATIC_ONLY ON CACHE BOOL "" FORCE)
    FetchContent_Declare(libical
        URL https://github.com/libical/libical/releases/download/v3.0.20/libical-3.0.20.tar.gz
        URL_HASH SHA256=e73de92f5a6ce84c1b00306446b290a2b08cdf0a80988eca0a2c9d5c3510b4c2
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(libical)
    set(PILON_ICAL_TARGET ical-static)
endif()
set(PILON_GOOGLE_CLIENT_ID "$ENV{PILON_GOOGLE_CLIENT_ID}" CACHE STRING "Google desktop OAuth client ID")
set(PILON_MICROSOFT_CLIENT_ID "$ENV{PILON_MICROSOFT_CLIENT_ID}" CACHE STRING "Microsoft public desktop application ID")
set(PILON_MICROSOFT_ANDROID_CLIENT_ID "$ENV{PILON_MICROSOFT_ANDROID_CLIENT_ID}" CACHE STRING "Registered Microsoft Android application ID")
set(PILON_CALENDAR_SOURCES ../addons/calendar/src/calendarmanager.cpp ../addons/calendar/src/calendarmanager.h ../addons/calendar/src/calendarstorage.cpp ../addons/calendar/src/calendarnetwork.cpp ../addons/calendar/src/calendarsharing.cpp ../addons/calendar/src/calendarformat.cpp ../addons/calendar/src/calendarformat.h ../addons/calendar/src/calendarfiles.h ../addons/calendar/src/calendarwidget.cpp)
add_library(pilon_workspace STATIC core/workspace/pilonworkspace.cpp core/workspace/pilonworkspace.h)
set_target_properties(pilon_workspace PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC ON)
target_include_directories(pilon_workspace PUBLIC ${PILON_INCLUDE_DIRECTORIES})
target_link_libraries(pilon_workspace PUBLIC Qt6::Core)
add_library(pilon_calendar STATIC ${PILON_CALENDAR_SOURCES})
set_target_properties(pilon_calendar PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC ON)
target_link_libraries(pilon_calendar PUBLIC Qt6::Gui Qt6::Qml Qt6::Network Qt6::Sql Qt6::Concurrent
    ${PILON_CALENDAR_PLATFORM_LIBRARIES} ${PILON_ICAL_TARGET})
if(NOT LIBICAL_FOUND)
    target_include_directories(pilon_calendar PUBLIC ${libical_BINARY_DIR}/src ${libical_SOURCE_DIR}/src ${libical_SOURCE_DIR}/src/libical ${libical_BINARY_DIR}/src/libical)
endif()
if(ANDROID)
    target_compile_definitions(pilon_calendar PRIVATE PILON_GOOGLE_CLIENT_ID=""
        PILON_MICROSOFT_CLIENT_ID="${PILON_MICROSOFT_ANDROID_CLIENT_ID}")
else()
    target_compile_definitions(pilon_calendar PRIVATE
        PILON_GOOGLE_CLIENT_ID="${PILON_GOOGLE_CLIENT_ID}"
        PILON_MICROSOFT_CLIENT_ID="${PILON_MICROSOFT_CLIENT_ID}")
endif()


qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)
if(ANDROID)
    qt_policy(SET QTP0002 NEW)
endif()

# Each feature owns its implementation once; the app and tests link the same code.
add_library(pilon_documents STATIC core/documents/noteimport.cpp core/documents/noteimport.h core/documents/documentpreview.cpp core/documents/documentpreview.h)
target_include_directories(pilon_documents PUBLIC ${PILON_INCLUDE_DIRECTORIES})
target_link_libraries(pilon_documents PUBLIC Qt6::Gui Qt6::Qml Qt6::Concurrent)
if(ANDROID)
    set(PDFium_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/android/pdfium/sdk/include" CACHE PATH "" FORCE)
    set(PDFium_LIBRARY "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/runtime/arm64-v8a/libpdfium.so" CACHE FILEPATH "" FORCE)
    find_package(PDFium CONFIG REQUIRED
        PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../build/android/pdfium/sdk" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    target_link_libraries(pilon_documents PRIVATE pdfium)
endif()

add_library(pilon_devices STATIC ../addons/ssh/src/sshmanager.cpp ../addons/ssh/src/sshmanager.h ../addons/devices/src/tailscalemanager.cpp ../addons/devices/src/tailscalemanager.h ../addons/devices/src/tailscalediscovery.cpp)
target_include_directories(pilon_devices PUBLIC ${PILON_INCLUDE_DIRECTORIES})
target_link_libraries(pilon_devices PUBLIC Qt6::Gui Qt6::Qml Qt6::Network Qt6::Concurrent ${PILON_PLATFORM_LIBRARIES})

add_library(pilon_files STATIC ../addons/files/src/remotefilemanager.cpp ../addons/files/src/remotefilemanager.h)
target_include_directories(pilon_files PUBLIC ${PILON_INCLUDE_DIRECTORIES})
target_link_libraries(pilon_files PUBLIC pilon_documents Qt6::Network ${PILON_SSH2_TARGET} PRIVATE Qt6::Quick)
if(NOT ANDROID)
    target_link_libraries(pilon_files PRIVATE Qt6::DBus)
endif()

add_library(pilon_notes STATIC
    ../addons/notes/src/noteslibrary.cpp ../addons/notes/src/notesmanager.cpp ../addons/notes/src/notesmanager.h
    ../addons/notes/src/notedocument.cpp ../addons/notes/src/notedocument.h ../addons/notes/src/notecanvas.cpp ../addons/notes/src/notecanvas.h
    ../addons/notes/src/notehandwriting.cpp ../addons/notes/src/notehandwriting.h)
target_include_directories(pilon_notes PUBLIC ${PILON_INCLUDE_DIRECTORIES})
target_link_libraries(pilon_notes PUBLIC pilon_documents Qt6::Quick)

add_library(pilon_assistant_models STATIC ../addons/assistant/src/assistantmodels.cpp ../addons/assistant/src/assistantmodels.h)
target_include_directories(pilon_assistant_models PUBLIC ${PILON_INCLUDE_DIRECTORIES})
target_link_libraries(pilon_assistant_models PUBLIC Qt6::Gui Qt6::Qml Qt6::Network)

add_library(pilon_assistant STATIC
    ../addons/assistant/src/assistantmanager.cpp ../addons/assistant/src/assistantmanager.h ../addons/assistant/src/assistantcodex.cpp ../addons/assistant/src/assistantcache.cpp
    ../addons/assistant/src/assistantconnections.cpp ../addons/assistant/src/assistantconnections.h ../addons/assistant/src/assistantstorage.cpp ../addons/assistant/src/assistantspeech.cpp
    ../addons/assistant/src/assistantresearch.cpp ../addons/assistant/src/assistantfiles.cpp ../addons/assistant/src/assistantworkspace.cpp ../addons/assistant/src/assistantmedia.cpp ../addons/assistant/src/assistantmedia.h
    ../addons/assistant/src/codexruntime.cpp ../addons/assistant/src/codexruntime.h ../addons/assistant/src/codexclient.cpp ../addons/assistant/src/codexclient.h ../addons/assistant/src/codexprovider.cpp ../addons/assistant/src/codexprovider.h)
target_include_directories(pilon_assistant PUBLIC ${PILON_INCLUDE_DIRECTORIES})
target_link_libraries(pilon_assistant PUBLIC
    pilon_calendar pilon_devices pilon_assistant_models Qt6::Concurrent Qt6::Multimedia)
if(ANDROID)
    target_link_libraries(pilon_assistant PRIVATE pilon_documents)
endif()

add_library(pilon_git STATIC ../addons/git/src/githubmanager.cpp ../addons/git/src/githubmanager.h ../addons/git/src/gitcredentials.cpp ../addons/git/src/gitcredentials.h ../addons/git/src/gitmanagedfiles.cpp ../addons/git/src/gitmanagedfiles.h)
target_include_directories(pilon_git PUBLIC ${PILON_INCLUDE_DIRECTORIES})
target_link_libraries(pilon_git PUBLIC Qt6::Gui Qt6::Qml Qt6::Concurrent)
if(ANDROID)
    target_link_libraries(pilon_git PUBLIC Qt6::Network)
endif()

add_library(pilon_addons STATIC core/addons/addonregistry.cpp core/addons/addonregistry.h)
target_include_directories(pilon_addons PUBLIC ${PILON_INCLUDE_DIRECTORIES})
target_link_libraries(pilon_addons PUBLIC Qt6::Qml)

add_library(pilon_widgets STATIC core/widgets/widgetstore.cpp core/widgets/widgetstore.h core/widgets/widgetplatform.cpp core/widgets/widgetplatform.h)
target_include_directories(pilon_widgets PUBLIC ${PILON_INCLUDE_DIRECTORIES})
target_link_libraries(pilon_widgets PUBLIC Qt6::Qml)
if(ANDROID)
    target_link_libraries(pilon_widgets PRIVATE Qt6::CorePrivate)
endif()

set(PILON_FEATURE_TARGETS pilon_addons pilon_widgets pilon_calendar pilon_documents pilon_devices pilon_files
    pilon_notes pilon_assistant_models pilon_assistant pilon_git)
set_target_properties(${PILON_FEATURE_TARGETS} PROPERTIES POSITION_INDEPENDENT_CODE ON)
foreach(feature IN LISTS PILON_FEATURE_TARGETS)
    target_link_libraries(${feature} PUBLIC pilon_workspace)
endforeach()

qt_add_executable(pilon
    app/main.cpp
    app/applicationservice.cpp
    app/applicationservice.h
    app/applicationreset.cpp
    app/applicationreset.h
    app/tabletinputguard.cpp
    app/tabletinputguard.h
)

file(GLOB_RECURSE PILON_SKILL_FILES CONFIGURE_DEPENDS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/../addons/assistant/skills/*")

set(PILON_AGENT_FILES ../addons/assistant/tools/pyproject.toml ../addons/assistant/tools/uv.lock
    ../addons/assistant/tools/browser_worker.py ../addons/assistant/tools/create_artifact.py ../addons/assistant/tools/AGENTS.md)

set(PILON_UI_FILES
    ui/controls/AppShortcut.qml
    ../addons/assistant/ui/AssistantActionButton.qml
    ui/controls/RoundedCheckBox.qml
    ui/controls/PilonButton.qml
    ui/controls/PilonWrapButton.qml
    ui/controls/PilonCloseButton.qml
    ui/controls/PilonColourDialog.qml
    ui/controls/PilonComboBox.qml
    ui/controls/PilonDialog.qml
    ui/controls/PilonMenu.qml
    ui/controls/PilonMenuItem.qml
    ui/controls/PilonPopup.qml
    ui/controls/PilonScrollView.qml
    ui/controls/PilonSlider.qml
    ui/controls/PilonSpinBox.qml
    ui/controls/PilonTextArea.qml
    ui/controls/PilonTextField.qml
    ui/controls/PilonHoldButton.qml
    ui/controls/PilonToolButton.qml
    ui/controls/PilonWindowControls.qml
    ui/controls/PilonWindowChrome.qml
    ui/controls/PilonWindowFrame.qml
    ui/controls/PilonNavigationButton.qml
    ui/controls/PilonPanelButton.qml
    ui/controls/PilonSectionHeader.qml
    ui/controls/PilonFormRow.qml
)

qt_add_qml_module(pilon
    URI Pilon
    VERSION 1.0
    SOURCES
        app/keybindings.cpp
        app/keybindings.h
        ../addons/ssh/src/terminalitem.cpp
        ../addons/ssh/src/terminalitem.h
    QML_FILES
        core/addons/ui/AddonWidgets.qml
        ${PILON_UI_FILES}
        app/ui/KeybindingSettings.qml
        ../addons/assistant/ui/AssistantPage.qml
        ../addons/assistant/ui/AssistantAttachmentCard.qml
        ../addons/assistant/ui/AssistantModelButton.qml
        ../addons/files/ui/FilesPage.qml
        core/documents/ui/DocumentView.qml
        core/documents/ui/ViewerSurface.qml
        ../addons/notes/ui/NotePaperDialog.qml
        ../addons/notes/ui/NotesPage.qml
        ../addons/files/ui/FileTile.qml
        ../addons/files/ui/FileTransferPopup.qml
        ../addons/files/ui/FileAccessPopup.qml
        ../addons/files/ui/FilePickerWindow.qml
        ../addons/files/ui/FileSelector.qml
        app/ui/Main.qml
        core/addons/ui/AddonHost.qml
        core/addons/ui/AddonIntegrationHost.qml
        core/addons/ui/AddonActionMenu.qml
        core/addons/ui/AddonIntegrationsSettings.qml
        core/addons/ui/AddonCleanup.qml
        core/addons/ui/AddonLifecycle.qml
        core/addons/ui/AddonSettingsContent.qml
        core/addons/ui/AddonsSettings.qml
        core/addons/ui/SidebarTabOrderDialog.qml
        app/ui/SettingsWindow.qml
        app/ui/AccountResetDialog.qml
        app/ui/AppNavigation.qml
        ../addons/files/ui/FileWorkspace.qml
        ../addons/files/ui/FileAppearance.qml
        app/ui/FirstRunSetup.qml
        app/ui/SetupOverview.qml
        app/ui/SharingConfiguration.qml
        ../addons/calendar/ui/CalendarButton.qml
        ../addons/calendar/ui/CalendarAccountGroup.qml
        ../addons/calendar/ui/CalendarDatePicker.qml
        ../addons/calendar/ui/CalendarTimePicker.qml
        ../addons/calendar/ui/CalendarTimeWheel.qml
        ../addons/calendar/ui/CalendarPalette.qml
        ../addons/calendar/ui/CalendarRecurrenceEditor.qml
        ../addons/calendar/ui/CalendarPage.qml
        ../addons/calendar/ui/CalendarShareDialog.qml
        ../addons/calendar/ui/CalendarSharesDialog.qml
        ../addons/calendar/ui/CalendarProposalPrompt.qml
        ../addons/calendar/ui/CalendarEventEditor.qml
        ../addons/calendar/ui/CalendarSettings.qml
        ../addons/assistant/ui/ModelSettings.qml
    RESOURCES
        ../addons/assistant/config/assistant-models.json
        ../addons/assistant/ui/ChatText.js
        ui/controls/PilonStyle.js
        ${PILON_SKILL_FILES}
        ${PILON_AGENT_FILES}
        ../addons/notes/assets/icons/application-x-pilon-note.svg
        ../addons/calendar/assets/icons/application-x-pilon-calendar.svg
        assets/icons/application-x-pilon-extension.svg
        assets/icons/pilon.svg
        ../addons/calendar/assets/icons/calendar.svg
        assets/icons/chat-bubble-left-right.svg
        assets/icons/microphone.svg
        assets/icons/paper-clip.svg
        assets/icons/plus.svg
        assets/icons/minus.svg
        assets/icons/list-bullet.svg
        assets/icons/undo.svg
        assets/icons/redo.svg
        assets/icons/cursor-arrow.svg
        assets/icons/arrows-move.svg
        assets/icons/lock-closed.svg
        assets/icons/lock-open.svg
        assets/icons/pencil.svg
        assets/icons/eraser.svg
        assets/icons/play.svg
        assets/icons/pause.svg
        assets/icons/trash.svg
        assets/icons/speaker-wave.svg
        assets/icons/stop.svg
        assets/icons/archive-box.svg
        assets/icons/arrow-right-end-on-rectangle.svg
        assets/icons/arrow-right-start-on-rectangle.svg
        assets/icons/arrow-up.svg
        assets/icons/bookmark.svg
        assets/icons/link.svg
        assets/icons/share.svg
        assets/icons/refresh.svg
        assets/icons/ellipsis-horizontal.svg
        assets/icons/check.svg
        assets/icons/chevron-left.svg
        assets/icons/chevron-right.svg
        assets/icons/cog-6-tooth.svg
        assets/icons/command-line.svg
        assets/icons/computer-desktop.svg
        assets/icons/document.svg
        assets/icons/note.svg
        assets/icons/document-text.svg
        assets/icons/film.svg
        assets/icons/folder.svg
        assets/icons/HEROICONS_LICENSE.txt
        assets/icons/magnifying-glass.svg
        assets/icons/musical-note.svg
        assets/icons/photo.svg
        assets/icons/squares-2x2.svg
        assets/icons/swatch.svg
        assets/icons/x-mark.svg
        assets/icons/wifi-off.svg
        ../addons/notes/workers/handwriting_worker.py
        core/documents/workers/document_worker.py
        ../addons/ssh/assets/licenses/LIBVTERM_LICENSE.txt
        ../addons/calendar/assets/licenses/LIBICAL_LICENSE.txt
)

target_link_libraries(pilon PRIVATE
    ${PILON_FEATURE_TARGETS}
    Qt6::Concurrent
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Multimedia
    Qt6::Network
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    Qt6::Widgets
    pilon_vterm
    ${PILON_SSH2_TARGET}
    ${PILON_PLATFORM_LIBRARIES}
)
if(ANDROID)
    target_link_libraries(pilon PRIVATE Qt6::CorePrivate)
    set(PILON_ANDROID_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/android-package")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/" DESTINATION "${PILON_ANDROID_PACKAGE_DIR}")
    # Platform implementations stay with their add-on; assemble Qt's package without changing Java/JNI paths.
    set(PILON_ANDROID_ADDON_COPY_COMMANDS)
    foreach(addon IN ITEMS assistant calendar devices git notes ssh)
        set(android_sources "${CMAKE_CURRENT_SOURCE_DIR}/../addons/${addon}/platform/android")
        if(IS_DIRECTORY "${android_sources}")
            file(COPY "${android_sources}/" DESTINATION "${PILON_ANDROID_PACKAGE_DIR}")
            list(APPEND PILON_ANDROID_ADDON_COPY_COMMANDS
                COMMAND "${CMAKE_COMMAND}" -E copy_directory "${android_sources}" "${PILON_ANDROID_PACKAGE_DIR}")
        endif()
    endforeach()
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/office/assets/"
        DESTINATION "${PILON_ANDROID_PACKAGE_DIR}/assets")
    set(PILON_ANDROID_AGENT_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/agent-runtime")
    file(COPY "${PILON_ANDROID_AGENT_RUNTIME_DIR}/assets/"
        DESTINATION "${PILON_ANDROID_PACKAGE_DIR}/assets")
    file(MAKE_DIRECTORY "${PILON_ANDROID_PACKAGE_DIR}/tests/assets/agent-runtime")
    file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/../addons/assistant/tests/android/android_agent_runtime_smoke.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../addons/assistant/tools/create_artifact.py"
        DESTINATION "${PILON_ANDROID_PACKAGE_DIR}/tests/assets/agent-runtime")
    set(PILON_ANDROID_TAILSCALE_AAR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/java/libtailscale.aar")
    if(NOT EXISTS "${PILON_ANDROID_TAILSCALE_AAR}")
        message(FATAL_ERROR "The Android Tailscale SDK is missing. Run src/scripts/android/build.sh first.")
    endif()
    file(MAKE_DIRECTORY "${PILON_ANDROID_PACKAGE_DIR}/libs")
    file(COPY "${PILON_ANDROID_TAILSCALE_AAR}" DESTINATION "${PILON_ANDROID_PACKAGE_DIR}/libs")
    set(PILON_ANDROID_QT_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/qt-platform")
    set(PILON_ANDROID_QT_PLATFORM_PLUGIN "${PILON_ANDROID_QT_PLATFORM_DIR}/plugins/libplugins_platforms_qtforandroid_arm64-v8a.so")
    set(PILON_ANDROID_QT_PLATFORM_JAR "${PILON_ANDROID_QT_PLATFORM_DIR}/pilon-Qt6Android.jar")
    foreach(artifact IN ITEMS "${PILON_ANDROID_QT_PLATFORM_PLUGIN}" "${PILON_ANDROID_QT_PLATFORM_JAR}")
        if(NOT EXISTS "${artifact}")
            message(FATAL_ERROR "The Android Qt surface backport is missing. Run src/scripts/android/build.sh first.")
        endif()
    endforeach()
    add_custom_command(OUTPUT "${PILON_ANDROID_QT_PLATFORM_PLUGIN}" "${PILON_ANDROID_QT_PLATFORM_JAR}"
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../src/scripts/android/build-qt-platform.sh"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../src/scripts/android/build-qt-platform.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/qt-platform/CMakeLists.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/qt-platform/surface-lifecycle.patch"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/qt-platform/accessibility-activation.patch"
            "${CMAKE_CURRENT_SOURCE_DIR}/platform/android/qt-platform/ime-options.patch"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/platform/android/android_qt_ime_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/platform/android/android_qt_texture_resize_tests.py" VERBATIM)
    add_custom_target(pilon_android_qt_platform
        DEPENDS "${PILON_ANDROID_QT_PLATFORM_PLUGIN}" "${PILON_ANDROID_QT_PLATFORM_JAR}")
    file(COPY "${PILON_ANDROID_QT_PLATFORM_JAR}" DESTINATION "${PILON_ANDROID_PACKAGE_DIR}/libs")
    file(MAKE_DIRECTORY "${PILON_ANDROID_PACKAGE_DIR}/qt-platform-native")
    file(COPY "${PILON_ANDROID_QT_PLATFORM_PLUGIN}" DESTINATION "${PILON_ANDROID_PACKAGE_DIR}/qt-platform-native")
    add_executable(pilon_android_git_credential ../addons/git/platform/android/gitcredential.c)
    set_target_properties(pilon_android_git_credential PROPERTIES
        OUTPUT_NAME "libpilon_git_credential" SUFFIX ".so" AUTOMOC OFF AUTOUIC OFF)
    target_link_options(pilon_android_git_credential PRIVATE
        -Wl,-z,max-page-size=16384,-z,common-page-size=16384)
    set(PILON_ANDROID_NETWORK_HELPERS
        "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/runtime/arm64-v8a/libpilon_ssh.so"
        "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/runtime/arm64-v8a/libpilon_scp.so")
    set(PILON_ANDROID_SSH_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/ssh")
    set(PILON_ANDROID_SSH_LICENSES "${PILON_ANDROID_SSH_LICENSE_DIR}/OpenSSH-LICENCE.txt"
        "${PILON_ANDROID_SSH_LICENSE_DIR}/ldns-LICENSE.txt")
    add_custom_command(OUTPUT ${PILON_ANDROID_NETWORK_HELPERS} ${PILON_ANDROID_SSH_LICENSES}
        COMMAND "${CMAKE_COMMAND}" -E env "ANDROID_NDK_ROOT=${ANDROID_NDK_ROOT}"
            "PILON_ANDROID_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            bash "${CMAKE_CURRENT_SOURCE_DIR}/../addons/ssh/scripts/android/build-network-runtime.sh"
        DEPENDS pilon_android_openssl ${PILON_ANDROID_OPENSSL_LIBRARIES}
            "${CMAKE_CURRENT_SOURCE_DIR}/../addons/ssh/scripts/android/build-network-runtime.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/../addons/ssh/platform/android/openssh.patch" VERBATIM)
    add_custom_target(pilon_android_network_runtime
        DEPENDS ${PILON_ANDROID_NETWORK_HELPERS} ${PILON_ANDROID_SSH_LICENSES})
    set(PILON_ANDROID_GIT_HELPERS
        "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/runtime/arm64-v8a/libpilon_git.so"
        "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/runtime/arm64-v8a/libpilon_git_remote_http.so")
    set(PILON_ANDROID_GIT_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/git")
    set(PILON_ANDROID_GIT_LICENSES "${PILON_ANDROID_GIT_LICENSE_DIR}/Git-COPYING.txt"
        "${PILON_ANDROID_GIT_LICENSE_DIR}/SHA1DC-LICENSE.txt" "${PILON_ANDROID_GIT_LICENSE_DIR}/curl-COPYING.txt")
    add_custom_command(OUTPUT ${PILON_ANDROID_GIT_HELPERS} ${PILON_ANDROID_GIT_LICENSES}
        COMMAND "${CMAKE_COMMAND}" -E env "ANDROID_NDK_ROOT=${ANDROID_NDK_ROOT}"
            "PILON_ANDROID_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
            bash "${CMAKE_CURRENT_SOURCE_DIR}/../addons/git/scripts/android/build-git-runtime.sh"
        DEPENDS pilon_android_openssl ${PILON_ANDROID_OPENSSL_LIBRARIES}
            "${CMAKE_CURRENT_SOURCE_DIR}/../addons/git/scripts/android/build-git-runtime.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/../addons/git/platform/android/git.patch" VERBATIM)
    add_custom_target(pilon_android_git_runtime
        DEPENDS ${PILON_ANDROID_GIT_HELPERS} ${PILON_ANDROID_GIT_LICENSES})
    set(PILON_ANDROID_PROVIDER_HELPERS
        "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/runtime/arm64-v8a/libpilon_gh.so"
        "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/runtime/arm64-v8a/libpilon_glab.so")
    add_custom_command(OUTPUT ${PILON_ANDROID_PROVIDER_HELPERS}
        COMMAND "${CMAKE_COMMAND}" -E env "ANDROID_NDK_ROOT=${ANDROID_NDK_ROOT}"
            bash "${CMAKE_CURRENT_SOURCE_DIR}/../addons/git/scripts/android/build-git-providers.sh"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../addons/git/scripts/android/build-git-providers.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/../addons/git/platform/android/provider-gh.patch"
            "${CMAKE_CURRENT_SOURCE_DIR}/../addons/git/platform/android/provider-glab.patch"
            "${CMAKE_CURRENT_SOURCE_DIR}/../addons/git/platform/android/provider_keyring.go" VERBATIM)
    add_custom_target(pilon_android_git_providers DEPENDS ${PILON_ANDROID_PROVIDER_HELPERS})
    set_target_properties(pilon PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${PILON_ANDROID_PACKAGE_DIR}"
        QT_ANDROID_PACKAGE_NAME "org.pilon.Pilon"
        QT_ANDROID_APP_NAME "Pilon"
        QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
        QT_ANDROID_VERSION_CODE 3
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 37
        QT_ANDROID_COMPILE_SDK_VERSION "37.0"
        QT_ANDROID_SDK_BUILD_TOOLS_REVISION "37.0.0"
        QT_ANDROID_LEGACY_PACKAGING TRUE
        QT_ANDROID_EXTRA_LIBS "${PILON_ANDROID_OPENSSL_LIBRARIES};${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/runtime/arm64-v8a/libonnxruntime.so;${PDFium_LIBRARY};${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/runtime/arm64-v8a/libaudiocpp.so")
    set(PILON_ANDROID_RUNTIME_HELPERS ${PILON_ANDROID_NETWORK_HELPERS} ${PILON_ANDROID_GIT_HELPERS} ${PILON_ANDROID_PROVIDER_HELPERS}
        $<TARGET_FILE:pilon_android_git_credential>)
    file(GLOB PILON_ANDROID_AGENT_RUNTIME_LIBRARIES CONFIGURE_DEPENDS
        "${PILON_ANDROID_AGENT_RUNTIME_DIR}/runtime/arm64-v8a/*.so")
    if(NOT PILON_ANDROID_AGENT_RUNTIME_LIBRARIES)
        message(FATAL_ERROR "The Android project runtimes are missing. Run src/scripts/android/build.sh first.")
    endif()
    list(APPEND PILON_ANDROID_RUNTIME_HELPERS ${PILON_ANDROID_AGENT_RUNTIME_LIBRARIES})
    set(PILON_ANDROID_OFFICE_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/office/assets")
    foreach(library IN ITEMS lo-native-code freebl3 nspr4 nss3 nssckbi nssdbm3 nssutil3
            plc4 plds4 smime3 softokn3 sqlite3 ssl3 pilon_office ggml-base ggml whisper
            ggml-cpu-android_armv8.0_1 ggml-cpu-android_armv8.2_1 ggml-cpu-android_armv8.2_2
            ggml-cpu-android_armv8.6_1)
        set(library_path "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/runtime/arm64-v8a/lib${library}.so")
        if(NOT EXISTS "${library_path}")
            message(FATAL_ERROR "Android runtime library is missing: ${library_path}. Run src/scripts/android/build.sh first.")
        endif()
        list(APPEND PILON_ANDROID_RUNTIME_HELPERS "${library_path}")
    endforeach()
    foreach(runtime IN ITEMS codex dns llama ocr ffmpeg ffprobe whisper-cli breeze)
        set(helper "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/runtime/arm64-v8a/libpilon_${runtime}.so")
        if(NOT EXISTS "${helper}")
            message(FATAL_ERROR "Android runtime helper is missing: ${helper}. Run src/scripts/android/build.sh first.")
        endif()
        list(APPEND PILON_ANDROID_RUNTIME_HELPERS "${helper}")
    endforeach()
    # Qt retains this directory between deployments; remove stale imports before copying dependencies.
    file(REMOVE_RECURSE "${CMAKE_CURRENT_BINARY_DIR}/android-build/libs/arm64-v8a")
    # Workers and their private libraries load in child processes, outside Qt's eager preload list.
    add_custom_target(pilon_android_runtimes
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/android-build/libs/arm64-v8a"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${PILON_ANDROID_RUNTIME_HELPERS}
            "${CMAKE_CURRENT_BINARY_DIR}/android-build/libs/arm64-v8a/"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "${PILON_ANDROID_PACKAGE_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/platform/android"
            "${PILON_ANDROID_PACKAGE_DIR}"
        ${PILON_ANDROID_ADDON_COPY_COMMANDS}
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PILON_ANDROID_OFFICE_ASSETS}"
            "${PILON_ANDROID_PACKAGE_DIR}/assets"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PILON_ANDROID_AGENT_RUNTIME_DIR}/assets"
            "${PILON_ANDROID_PACKAGE_DIR}/assets"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PILON_ANDROID_PACKAGE_DIR}/tests/assets/agent-runtime"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/../addons/assistant/tests/android/android_agent_runtime_smoke.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../addons/assistant/tools/create_artifact.py" "${PILON_ANDROID_PACKAGE_DIR}/tests/assets/agent-runtime"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PILON_ANDROID_PACKAGE_DIR}/libs"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PILON_ANDROID_TAILSCALE_AAR}"
            "${PILON_ANDROID_PACKAGE_DIR}/libs/libtailscale.aar"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PILON_ANDROID_QT_PLATFORM_JAR}"
            "${PILON_ANDROID_PACKAGE_DIR}/libs/pilon-Qt6Android.jar"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${PILON_ANDROID_PACKAGE_DIR}/qt-platform-native"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PILON_ANDROID_QT_PLATFORM_PLUGIN}"
            "${PILON_ANDROID_PACKAGE_DIR}/qt-platform-native/libplugins_platforms_qtforandroid_arm64-v8a.so"
        DEPENDS ${PILON_ANDROID_RUNTIME_HELPERS} "${PILON_ANDROID_TAILSCALE_AAR}"
            "${PILON_ANDROID_QT_PLATFORM_JAR}" "${PILON_ANDROID_QT_PLATFORM_PLUGIN}" VERBATIM)
    add_dependencies(pilon_android_runtimes pilon_android_network_runtime pilon_android_git_runtime pilon_android_git_providers
        pilon_android_git_credential pilon_android_qt_platform)
    add_dependencies(pilon pilon_android_runtimes)
endif()
# Foreign-type wrappers are generated here and included by qmltyperegistrar.
target_include_directories(pilon PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
foreach(feature IN LISTS PILON_FEATURE_TARGETS)
    qt_generate_foreign_qml_types(${feature} pilon)
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(PILON_PACKAGES)
foreach(addon IN ITEMS files ssh assistant notes calendar git devices)
    file(GLOB_RECURSE package_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../addons/${addon}/package/*")
    set(package "${CMAKE_CURRENT_BINARY_DIR}/addons/${addon}.piext")
    add_custom_command(OUTPUT "${package}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../src/scripts/compile-addon.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../addons/${addon}/package" --output "${package}" --builtin --rcc $<TARGET_FILE:Qt6::rcc>
        DEPENDS ${package_sources} "${CMAKE_CURRENT_SOURCE_DIR}/../src/scripts/compile-addon.py"
        VERBATIM)
    list(APPEND PILON_PACKAGES "${package}")
endforeach()
add_custom_target(pilon_addon_packages ALL DEPENDS ${PILON_PACKAGES})
add_dependencies(pilon pilon_addon_packages)
if(ANDROID)
    qt_add_resources(pilon android_builtin_addons PREFIX "/pilon-addons"
        BASE "${CMAKE_CURRENT_BINARY_DIR}/addons" FILES ${PILON_PACKAGES})
endif()
if(ANDROID)
    file(GLOB_RECURSE PILON_CALENDAR_ZONES CONFIGURE_DEPENDS "${libical_SOURCE_DIR}/zoneinfo/*.ics")
    list(APPEND PILON_CALENDAR_ZONES "${libical_SOURCE_DIR}/zoneinfo/zones.tab")
    qt_add_resources(pilon android_calendar_zones PREFIX "/pilon-zoneinfo"
        BASE "${libical_SOURCE_DIR}/zoneinfo" FILES ${PILON_CALENDAR_ZONES})
    set_source_files_properties("${pilon_openssl_SOURCE_DIR}/LICENSE.txt" PROPERTIES QT_RESOURCE_ALIAS "OpenSSL.txt")
    set_source_files_properties("${libssh2_SOURCE_DIR}/COPYING" PROPERTIES QT_RESOURCE_ALIAS "libssh2.txt")
    qt_add_resources(pilon android_dependency_licenses PREFIX "/licenses"
        FILES "${pilon_openssl_SOURCE_DIR}/LICENSE.txt" "${libssh2_SOURCE_DIR}/COPYING")
    set(PILON_ANDROID_OCR_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/ocr")
    file(GLOB_RECURSE PILON_ANDROID_OCR_LICENSES CONFIGURE_DEPENDS "${PILON_ANDROID_OCR_LICENSE_DIR}/*")
    qt_add_resources(pilon android_ocr_licenses PREFIX "/licenses/ocr"
        BASE "${PILON_ANDROID_OCR_LICENSE_DIR}" FILES ${PILON_ANDROID_OCR_LICENSES})
    set(PILON_ANDROID_PDF_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/pdfium")
    file(GLOB_RECURSE PILON_ANDROID_PDF_LICENSES CONFIGURE_DEPENDS "${PILON_ANDROID_PDF_LICENSE_DIR}/*")
    qt_add_resources(pilon android_pdf_licenses PREFIX "/licenses/pdfium"
        BASE "${PILON_ANDROID_PDF_LICENSE_DIR}" FILES ${PILON_ANDROID_PDF_LICENSES})
    set_source_files_properties(${PILON_ANDROID_SSH_LICENSES} PROPERTIES GENERATED TRUE)
    qt_add_resources(pilon android_ssh_licenses PREFIX "/licenses/ssh"
        BASE "${PILON_ANDROID_SSH_LICENSE_DIR}" FILES ${PILON_ANDROID_SSH_LICENSES})
    set_source_files_properties(${PILON_ANDROID_GIT_LICENSES} PROPERTIES GENERATED TRUE)
    qt_add_resources(pilon android_git_licenses PREFIX "/licenses/git"
        BASE "${PILON_ANDROID_GIT_LICENSE_DIR}" FILES ${PILON_ANDROID_GIT_LICENSES})
    set(PILON_ANDROID_OFFICE_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/office")
    file(GLOB_RECURSE PILON_ANDROID_OFFICE_LICENSES CONFIGURE_DEPENDS "${PILON_ANDROID_OFFICE_LICENSE_DIR}/*")
    qt_add_resources(pilon android_office_licenses PREFIX "/licenses/office"
        BASE "${PILON_ANDROID_OFFICE_LICENSE_DIR}" FILES ${PILON_ANDROID_OFFICE_LICENSES})
    set(PILON_ANDROID_AI_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/runtime")
    file(GLOB_RECURSE PILON_ANDROID_AI_LICENSES CONFIGURE_DEPENDS "${PILON_ANDROID_AI_LICENSE_DIR}/*")
    qt_add_resources(pilon android_ai_licenses PREFIX "/licenses/runtime"
        BASE "${PILON_ANDROID_AI_LICENSE_DIR}" FILES ${PILON_ANDROID_AI_LICENSES})
    set(PILON_ANDROID_MEDIA_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/media")
    file(GLOB_RECURSE PILON_ANDROID_MEDIA_LICENSES CONFIGURE_DEPENDS "${PILON_ANDROID_MEDIA_LICENSE_DIR}/*")
    qt_add_resources(pilon android_media_licenses PREFIX "/licenses/media"
        BASE "${PILON_ANDROID_MEDIA_LICENSE_DIR}" FILES ${PILON_ANDROID_MEDIA_LICENSES})
    set(PILON_ANDROID_BREEZE_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/breeze")
    file(GLOB_RECURSE PILON_ANDROID_BREEZE_LICENSES CONFIGURE_DEPENDS "${PILON_ANDROID_BREEZE_LICENSE_DIR}/*")
    qt_add_resources(pilon android_breeze_licenses PREFIX "/licenses/breeze"
        BASE "${PILON_ANDROID_BREEZE_LICENSE_DIR}" FILES ${PILON_ANDROID_BREEZE_LICENSES})
    set(PILON_ANDROID_PROVIDER_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/git-providers")
    file(GLOB_RECURSE PILON_ANDROID_PROVIDER_LICENSES CONFIGURE_DEPENDS "${PILON_ANDROID_PROVIDER_LICENSE_DIR}/*")
    qt_add_resources(pilon android_provider_licenses PREFIX "/licenses/git-providers"
        BASE "${PILON_ANDROID_PROVIDER_LICENSE_DIR}" FILES ${PILON_ANDROID_PROVIDER_LICENSES})
    set(PILON_ANDROID_QT_PLATFORM_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/qt-platform")
    file(GLOB_RECURSE PILON_ANDROID_QT_PLATFORM_LICENSES CONFIGURE_DEPENDS "${PILON_ANDROID_QT_PLATFORM_LICENSE_DIR}/*")
    qt_add_resources(pilon android_qt_platform_licenses PREFIX "/licenses/qt-platform"
        BASE "${PILON_ANDROID_QT_PLATFORM_LICENSE_DIR}" FILES ${PILON_ANDROID_QT_PLATFORM_LICENSES})
    set(PILON_ANDROID_AGENT_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/agent-runtime")
    file(GLOB_RECURSE PILON_ANDROID_AGENT_LICENSES CONFIGURE_DEPENDS "${PILON_ANDROID_AGENT_LICENSE_DIR}/*")
    qt_add_resources(pilon android_agent_runtime_licenses PREFIX "/licenses/agent-runtime"
        BASE "${PILON_ANDROID_AGENT_LICENSE_DIR}" FILES ${PILON_ANDROID_AGENT_LICENSES})
    set(PILON_ANDROID_TAILSCALE_LICENSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../builds/android/licenses/tailscale")
    file(GLOB_RECURSE PILON_ANDROID_TAILSCALE_LICENSES CONFIGURE_DEPENDS "${PILON_ANDROID_TAILSCALE_LICENSE_DIR}/*")
    qt_add_resources(pilon android_tailscale_licenses PREFIX "/licenses/tailscale"
        BASE "${PILON_ANDROID_TAILSCALE_LICENSE_DIR}" FILES ${PILON_ANDROID_TAILSCALE_LICENSES})
endif()
install(FILES ${PILON_PACKAGES} DESTINATION addons)
install(FILES ../src/platform/linux/desktop/pilon-mime.xml DESTINATION share/mime/packages)
install(FILES ../addons/notes/assets/icons/application-x-pilon-note.svg ../addons/calendar/assets/icons/application-x-pilon-calendar.svg
    assets/icons/application-x-pilon-extension.svg DESTINATION share/icons/hicolor/scalable/mimetypes)

install(TARGETS pilon
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
)

option(PILON_BUILD_TESTS "Build Pilon's regression tests" OFF)
if(PILON_BUILD_TESTS)
    include(cmake/StageTestSources.cmake)
    add_compile_definitions(PILON_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test QuickTest GuiPrivate)
    qt_add_executable(pilon_workspace_tests tests/core/workspace_tests.cpp)
    target_link_libraries(pilon_workspace_tests PRIVATE pilon_workspace Qt6::Test)
    add_test(NAME workspace COMMAND pilon_workspace_tests)
    qt_add_executable(pilon_addon_tests tests/core/addon_tests.cpp)
    target_link_libraries(pilon_addon_tests PRIVATE pilon_addons Qt6::Test)
    add_test(NAME addons COMMAND pilon_addon_tests)
    qt_add_executable(pilon_widget_tests tests/core/widget_tests.cpp)
    target_link_libraries(pilon_widget_tests PRIVATE pilon_widgets Qt6::Test)
    add_test(NAME widgets COMMAND pilon_widget_tests)
    qt_add_executable(pilon_calendar_widget_tests ../addons/calendar/tests/calendar_widget_tests.cpp)
    target_link_libraries(pilon_calendar_widget_tests PRIVATE pilon_calendar pilon_widgets Qt6::Test)
    add_test(NAME calendar_widgets COMMAND pilon_calendar_widget_tests)
    qt_add_executable(pilon_ssh_tests ../addons/ssh/tests/ssh_tests.cpp)
    target_link_libraries(pilon_ssh_tests PRIVATE pilon_devices Qt6::Test)
    add_test(NAME ssh COMMAND pilon_ssh_tests)
    qt_add_executable(pilon_terminal_tests ../addons/ssh/tests/terminal_tests.cpp
        ../addons/ssh/src/terminalitem.cpp ../addons/ssh/src/terminalitem.h app/keybindings.cpp app/keybindings.h)
    target_include_directories(pilon_terminal_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(pilon_terminal_tests PRIVATE pilon_workspace pilon_vterm Qt6::Quick Qt6::Test)
    add_test(NAME terminal_resize COMMAND pilon_terminal_tests -platform offscreen)
    set_tests_properties(terminal_resize PROPERTIES TIMEOUT 20)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ssh-test-bin")
    configure_file(../addons/ssh/tests/fixtures/ssh.py "${CMAKE_CURRENT_BINARY_DIR}/ssh-test-bin/ssh" COPYONLY)
    file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/ssh-test-bin/ssh" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    qt_add_executable(pilon_github_tests ../addons/git/tests/github_tests.cpp)
    target_link_libraries(pilon_github_tests PRIVATE pilon_git Qt6::Test)
    add_test(NAME github COMMAND pilon_github_tests -platform offscreen)
    qt_add_executable(pilon_gitcredentials_tests ../addons/git/tests/gitcredentials_tests.cpp)
    add_executable(pilon_gitcredential_test_helper ../addons/git/platform/android/gitcredential.c)
    set_target_properties(pilon_gitcredential_test_helper PROPERTIES AUTOMOC OFF AUTOUIC OFF)
    target_compile_definitions(pilon_gitcredentials_tests PRIVATE
        PILON_GIT_CREDENTIAL_HELPER="$<TARGET_FILE:pilon_gitcredential_test_helper>")
    add_dependencies(pilon_gitcredentials_tests pilon_gitcredential_test_helper)
    target_link_libraries(pilon_gitcredentials_tests PRIVATE pilon_git Qt6::Test Qt6::Network)
    add_test(NAME git_credentials COMMAND pilon_gitcredentials_tests)
    if(NOT ANDROID)
        add_test(NAME git_credential_transport COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/../addons/git/tests/gitcredential_transport_tests.py)
    endif()
    qt_add_executable(pilon_gitmanagedfiles_tests ../addons/git/tests/gitmanagedfiles_tests.cpp)
    target_link_libraries(pilon_gitmanagedfiles_tests PRIVATE pilon_git Qt6::Test)
    add_test(NAME git_managed_files COMMAND pilon_gitmanagedfiles_tests)
    qt_add_executable(pilon_gitsync_tests ../addons/git/tests/gitsync_tests.cpp)
    target_link_libraries(pilon_gitsync_tests PRIVATE pilon_git pilon_notes pilon_calendar Qt6::Test)
    add_test(NAME git_sync COMMAND pilon_gitsync_tests -platform offscreen)
    set_tests_properties(git_sync PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software;PILON_NOTES_HANDWRITING=0")
    qt_add_executable(pilon_device_tests ../addons/devices/tests/tailscale_tests.cpp)
    target_link_libraries(pilon_device_tests PRIVATE pilon_devices Qt6::Test)
    add_test(NAME devices COMMAND pilon_device_tests)

    qt_add_executable(pilon_service_tests tests/core/applicationservice_tests.cpp
        app/applicationservice.cpp app/applicationservice.h app/applicationreset.cpp app/applicationreset.h)
    target_include_directories(pilon_service_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    qt_add_resources(pilon_service_tests app_icon PREFIX "/qt/qml/Pilon" FILES assets/icons/pilon.svg)
    target_link_libraries(pilon_service_tests PRIVATE pilon_workspace Qt6::Test Qt6::Widgets Qt6::Network Qt6::Sql)
    add_test(NAME application_service COMMAND pilon_service_tests -platform offscreen)
    qt_add_executable(pilon_calendar_tests ../addons/calendar/tests/calendar_tests.cpp)
    target_include_directories(pilon_calendar_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(pilon_calendar_tests PRIVATE pilon_calendar Qt6::Test)
    add_test(NAME calendar COMMAND pilon_calendar_tests)
    find_program(PILON_DBUS_RUN_SESSION dbus-run-session)
    if(PILON_DBUS_RUN_SESSION)
        add_test(NAME calendar_background_notifications COMMAND ${PILON_DBUS_RUN_SESSION} -- $<TARGET_FILE:pilon_calendar_tests> remindersWithoutWindow)
        set_tests_properties(calendar_background_notifications PROPERTIES ENVIRONMENT "PILON_TEST_PRIVATE_BUS=1")
    endif()
    qt_add_executable(pilon_calendar_ui_tests ../addons/calendar/tests/harness/calendar_ui_main.cpp app/keybindings.cpp app/keybindings.h)
    target_link_libraries(pilon_calendar_ui_tests PRIVATE pilon_calendar Qt6::QuickTest Qt6::Quick Qt6::Widgets)
    # Exercise native dialog sizing with the same QML compilation as the app.
    qt_add_qml_module(pilon_calendar_ui_tests
        URI PilonCalendarUiTests
        VERSION 1.0
        QML_FILES
            ../addons/calendar/ui/CalendarSharesDialog.qml ../addons/calendar/ui/CalendarShareDialog.qml ../addons/calendar/ui/CalendarEventEditor.qml
            ../addons/calendar/ui/CalendarPalette.qml ../addons/calendar/ui/CalendarRecurrenceEditor.qml
            ../addons/calendar/ui/CalendarAccountGroup.qml ../addons/calendar/ui/CalendarDatePicker.qml ../addons/calendar/ui/CalendarTimePicker.qml ../addons/calendar/ui/CalendarTimeWheel.qml ../addons/calendar/ui/CalendarButton.qml
            ${PILON_UI_FILES}
        RESOURCES ui/controls/PilonStyle.js assets/icons/chevron-left.svg assets/icons/chevron-right.svg assets/icons/x-mark.svg)
    add_test(NAME calendar_ui COMMAND pilon_calendar_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_calendar.qml)
    add_test(NAME calendar_input COMMAND pilon_calendar_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_calendar_input.qml)
    add_test(NAME calendar_geometry COMMAND pilon_calendar_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_calendar_geometry.qml)
    add_test(NAME calendar_datetime COMMAND pilon_calendar_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_calendar_datetime.qml)
    add_test(NAME calendar_responsive_ui COMMAND pilon_calendar_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_calendar_responsive.qml)
    add_test(NAME calendar_widget_runtime COMMAND pilon_calendar_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_calendar_widgets.qml)
    set_tests_properties(calendar_ui calendar_input calendar_geometry calendar_datetime calendar_responsive_ui PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software")


    qt_add_executable(pilon_document_tests tests/core/documentpreview_tests.cpp)
    target_link_libraries(pilon_document_tests PRIVATE pilon_documents Qt6::Test)
    qt_add_resources(pilon_document_tests document_test_worker PREFIX "/qt/qml/Pilon" FILES core/documents/workers/document_worker.py)
    add_test(NAME documents COMMAND pilon_document_tests -platform offscreen)
    qt_add_executable(pilon_model_tests ../addons/assistant/tests/assistantmodel_tests.cpp)
    target_link_libraries(pilon_model_tests PRIVATE pilon_assistant_models Qt6::Test)
    qt_add_resources(pilon_model_tests model_catalog PREFIX "/qt/qml/Pilon" FILES ../addons/assistant/config/assistant-models.json)
    add_test(NAME models COMMAND pilon_model_tests)
    qt_add_executable(pilon_assistant_tests ../addons/assistant/tests/assistant_tests.cpp)
    target_link_libraries(pilon_assistant_tests PRIVATE pilon_assistant pilon_files Qt6::Test Qt6::Quick)
    qt_add_resources(pilon_assistant_tests assistant_test_skills
        PREFIX "/qt/qml/Pilon" FILES ${PILON_SKILL_FILES} ${PILON_AGENT_FILES})
    add_test(NAME assistant COMMAND pilon_assistant_tests -platform offscreen)
    # This suite includes the implementation to exercise its private traversal helpers.
    qt_add_executable(pilon_file_tests ../addons/files/tests/remotefile_tests.cpp ../addons/files/src/remotefilemanager.h)
    target_link_libraries(pilon_file_tests PRIVATE pilon_documents Qt6::Test
        Qt6::Network Qt6::Concurrent Qt6::Quick ${PILON_SSH2_TARGET})
    if(NOT ANDROID)
        target_link_libraries(pilon_file_tests PRIVATE Qt6::DBus)
    endif()
    add_test(NAME files COMMAND pilon_file_tests -platform offscreen)
    qt_add_executable(pilon_notes_tests ../addons/notes/tests/notes_tests.cpp)
    target_link_libraries(pilon_notes_tests PRIVATE pilon_notes Qt6::Test)
    qt_add_resources(pilon_notes_tests notes_ocr_test PREFIX "/qt/qml/Pilon" FILES ../addons/notes/workers/handwriting_worker.py)
    add_test(NAME notes COMMAND pilon_notes_tests -platform offscreen)
    qt_add_executable(pilon_notes_ui_tests ../addons/notes/tests/harness/notes_ui_main.cpp
        app/tabletinputguard.cpp app/tabletinputguard.h app/keybindings.cpp app/keybindings.h ../addons/ssh/src/terminalitem.cpp ../addons/ssh/src/terminalitem.h)
    target_link_libraries(pilon_notes_ui_tests PRIVATE ${PILON_FEATURE_TARGETS} pilon_vterm
        Qt6::QuickTest Qt6::GuiPrivate Qt6::Quick Qt6::Widgets)
    add_dependencies(pilon_notes_ui_tests pilon_addon_packages)
    set(PILON_EXAMPLE_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/test-addons/counter.piext")
    add_custom_command(OUTPUT "${PILON_EXAMPLE_PACKAGE}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../src/scripts/compile-addon.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../examples/counter" --output "${PILON_EXAMPLE_PACKAGE}" --rcc $<TARGET_FILE:Qt6::rcc>
        DEPENDS ../examples/counter/manifest.json ../examples/counter/Page.qml ../examples/counter/Settings.qml ../examples/counter/Lifecycle.qml ../src/scripts/compile-addon.py
        VERBATIM)
    add_custom_target(pilon_example_package DEPENDS "${PILON_EXAMPLE_PACKAGE}")
    add_dependencies(pilon_notes_ui_tests pilon_example_package)
    set(PILON_CHECKLIST_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/test-addons/checklist.piext")
    add_custom_command(OUTPUT "${PILON_CHECKLIST_PACKAGE}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../src/scripts/compile-addon.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../examples/checklist" --output "${PILON_CHECKLIST_PACKAGE}" --rcc $<TARGET_FILE:Qt6::rcc>
        DEPENDS ../examples/checklist/manifest.json ../examples/checklist/Page.qml ../examples/checklist/Settings.qml ../src/scripts/compile-addon.py
        VERBATIM)
    add_custom_target(pilon_checklist_package DEPENDS "${PILON_CHECKLIST_PACKAGE}")
    add_dependencies(pilon_notes_ui_tests pilon_checklist_package)
    foreach(fixture IN ITEMS provider consumer)
        set(package "${CMAKE_CURRENT_BINARY_DIR}/test-addons/dependency-${fixture}.piext")
        add_custom_command(OUTPUT "${package}"
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../src/scripts/compile-addon.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dependency-${fixture}" --output "${package}" --rcc $<TARGET_FILE:Qt6::rcc>
            DEPENDS tests/fixtures/dependency-${fixture}/manifest.json tests/fixtures/dependency-${fixture}/Runtime.qml ../src/scripts/compile-addon.py
            VERBATIM)
        add_custom_target(pilon_dependency_${fixture}_package DEPENDS "${package}")
        add_dependencies(pilon_notes_ui_tests pilon_dependency_${fixture}_package)
        string(TOUPPER "${fixture}" fixture_define)
        target_compile_definitions(pilon_notes_ui_tests PRIVATE PILON_DEPENDENCY_${fixture_define}_PACKAGE="${package}")
    endforeach()
    set(PILON_WIDGET_TEST_PACKAGE "${CMAKE_CURRENT_BINARY_DIR}/test-addons/widgets-test-provider.piext")
    add_custom_command(OUTPUT "${PILON_WIDGET_TEST_PACKAGE}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/../src/scripts/compile-addon.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/widget-provider" --output "${PILON_WIDGET_TEST_PACKAGE}" --rcc $<TARGET_FILE:Qt6::rcc>
        DEPENDS tests/fixtures/widget-provider/manifest.json tests/fixtures/widget-provider/Runtime.qml ../src/scripts/compile-addon.py
        VERBATIM)
    add_custom_target(pilon_widget_test_package DEPENDS "${PILON_WIDGET_TEST_PACKAGE}")
    add_dependencies(pilon_notes_ui_tests pilon_widget_test_package)
    target_compile_definitions(pilon_notes_ui_tests PRIVATE PILON_WIDGET_TEST_PACKAGE="${PILON_WIDGET_TEST_PACKAGE}"
        PILON_QML_SOURCE_DIR="${PILON_TEST_SOURCE_DIR}" PILON_EXAMPLE_PACKAGE="${PILON_EXAMPLE_PACKAGE}" PILON_CHECKLIST_PACKAGE="${PILON_CHECKLIST_PACKAGE}")
    file(GLOB PILON_TEST_ICONS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "assets/icons/*.svg")
    qt_add_resources(pilon_notes_ui_tests ui_test_icons PREFIX "/qt/qml/Pilon" FILES ${PILON_TEST_ICONS})
    qt_add_resources(pilon_notes_ui_tests document_ui_worker PREFIX "/qt/qml/Pilon" FILES core/documents/workers/document_worker.py)
    qt_add_resources(pilon_notes_ui_tests model_ui_catalog PREFIX "/qt/qml/Pilon" FILES ../addons/assistant/config/assistant-models.json)
    add_test(NAME keybindings_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_keybindings.qml)
    set_tests_properties(keybindings_ui PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software;PILON_NOTES_HANDWRITING=0")
    add_test(NAME controls_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_controls.qml)
    add_test(NAME devices_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_devices.qml)
    add_test(NAME models_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_models.qml)
    add_test(NAME main_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_main.qml)
    add_test(NAME setup_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_setup.qml)
    add_test(NAME accounts_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_accounts.qml)
    add_test(NAME note_paper_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_note_paper.qml)
    add_test(NAME addons_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_addons.qml)
    add_test(NAME widgets_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_widgets.qml)
    add_test(NAME widget_package_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_widget_package.qml)
    add_test(NAME integrations_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_integrations.qml)
    add_test(NAME samples_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_samples.qml)
    add_test(NAME ssh_defaults_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_ssh_defaults.qml)
    add_test(NAME navigation_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_navigation.qml)
    add_test(NAME ui_catalog COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_ui_catalog.qml)
    set_tests_properties(note_paper_ui addons_ui integrations_ui samples_ui ssh_defaults_ui setup_ui accounts_ui navigation_ui ui_catalog PROPERTIES
        ENVIRONMENT "QT_QUICK_BACKEND=software;PILON_NOTES_HANDWRITING=0;PILON_EPHEMERAL_CHAT=1;PILON_CODEX=/nonexistent")
    add_test(NAME ssh_ui COMMAND ${CMAKE_COMMAND} -E env "PATH=${CMAKE_CURRENT_BINARY_DIR}/ssh-test-bin:${CMAKE_CURRENT_BINARY_DIR}/test-tools:$ENV{PATH}"
        $<TARGET_FILE:pilon_notes_ui_tests> -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_ssh.qml)
    set_tests_properties(ssh_ui PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software;PILON_NOTES_HANDWRITING=0;PILON_EPHEMERAL_CHAT=1;PILON_CODEX=/nonexistent")
    set_tests_properties(controls_ui models_ui main_ui PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software;PILON_NOTES_HANDWRITING=0")
    set_property(TEST main_ui APPEND PROPERTY ENVIRONMENT "PILON_EPHEMERAL_CHAT=1;PILON_CODEX=/nonexistent")
    add_test(NAME assistant_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_assistant.qml)
    set_tests_properties(assistant_ui PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software")
    add_test(NAME github_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_github.qml)
    add_test(NAME git_workspace_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_git_workspace.qml)
    add_test(NAME git_adopt_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_git_adopt.qml)
    add_test(NAME git_changes_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_git_changes.qml)
    set_tests_properties(github_ui git_workspace_ui git_adopt_ui git_changes_ui PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software;PILON_NOTES_HANDWRITING=0")
    add_test(NAME files_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/tst_files.qml)
    set_tests_properties(files_ui PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software;PILON_NOTES_HANDWRITING=0")
    add_test(NAME notes_ui COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/notes_ui.qml)
    add_test(NAME notes_no_touch COMMAND pilon_notes_ui_tests -platform offscreen -input ${PILON_TEST_SOURCE_DIR}/tests/notes_no_touch.qml)
    set_tests_properties(notes notes_ui notes_no_touch PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software;PILON_NOTES_HANDWRITING=0;PILON_NOTES_OFFICE=${CMAKE_CURRENT_SOURCE_DIR}/../runtime/libreoffice/program/soffice")
    set_property(TEST notes_no_touch APPEND PROPERTY ENVIRONMENT "PILON_TEST_NO_TOUCH=1")
    find_program(PILON_TEST_PYTHON NAMES python3 REQUIRED)
    add_test(NAME desktop_installer COMMAND ${PILON_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python/desktop_installer_tests.py)
    add_test(NAME android_packaging COMMAND ${PILON_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tests/platform/android/android_packaging_tests.py)
    add_test(NAME release_packaging COMMAND ${PILON_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tests/python/release_packaging_tests.py)
    add_test(NAME runtime_build COMMAND ${PILON_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/../addons/assistant/tests/python/runtime_build_tests.py)
    add_test(NAME speech_speed COMMAND ${PILON_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/../addons/assistant/tests/python/breeze_worker_tests.py)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/litert/bin/python")
        add_test(NAME litert_worker COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/litert/bin/python" ${CMAKE_CURRENT_SOURCE_DIR}/../addons/assistant/tests/python/litert_worker_tests.py)
    endif()
    add_test(NAME handwriting_worker COMMAND ${PILON_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/../addons/notes/tests/python/handwriting_worker_tests.py)
    add_test(NAME browser_worker COMMAND ${PILON_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/../addons/assistant/tests/python/browser_worker_tests.py)

    # An integration test may instantiate real managers. Keep the user's keyring
    # out of every suite; tests that exercise keyring behavior supply their own fake.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-tools")
    configure_file(tests/fixtures/secret-tool.sh "${CMAKE_CURRENT_BINARY_DIR}/test-tools/secret-tool" COPYONLY)
    file(CHMOD "${CMAKE_CURRENT_BINARY_DIR}/test-tools/secret-tool" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    get_property(PILON_TEST_NAMES DIRECTORY PROPERTY TESTS)
    foreach(test_name IN LISTS PILON_TEST_NAMES)
        set_property(TEST ${test_name} APPEND PROPERTY ENVIRONMENT
            "PATH=${CMAKE_CURRENT_BINARY_DIR}/test-tools:$ENV{PATH}")
    endforeach()
endif()
