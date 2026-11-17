cmake_minimum_required(VERSION 3.21)
project(PilonAccessibilityActivationProbe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 6.12.0 EXACT REQUIRED COMPONENTS Quick Test)
qt_standard_project_setup(REQUIRES 6.12)
qt_add_executable(tst_pilon_accessibility_activation tst_accessibility_activation.cpp)
qt_add_qml_module(tst_pilon_accessibility_activation
    URI PilonAccessibilityProbe VERSION 1.0 QML_FILES Editor.qml)
target_link_libraries(tst_pilon_accessibility_activation PRIVATE Qt6::Quick Qt6::Test)
if(ANDROID)
    target_link_options(tst_pilon_accessibility_activation PRIVATE
        -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
    set_target_properties(tst_pilon_accessibility_activation PROPERTIES
        QT_ANDROID_PACKAGE_NAME org.pilon.AccessibilityActivationProbe
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 37
        QT_ANDROID_COMPILE_SDK_VERSION "37.0"
        QT_ANDROID_SDK_BUILD_TOOLS_REVISION "37.0.0")
    if(PILON_PROBE_PACKAGE_DIR)
        set_target_properties(tst_pilon_accessibility_activation PROPERTIES
            QT_ANDROID_PACKAGE_SOURCE_DIR "${PILON_PROBE_PACKAGE_DIR}")
    endif()
endif()
