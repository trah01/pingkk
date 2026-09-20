cmake_minimum_required(VERSION 3.15)

project(pingkk VERSION 0.1.1 LANGUAGES CXX)

# Keep our executables (including the separately installed CLI) self-contained.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(PINGKK_BUILD_GUI "构建 Qt 图形界面" ON)
file(WRITE "${CMAKE_BINARY_DIR}/pingkk-version.txt" "${PROJECT_VERSION}\n")

configure_file(
    include/pingkk/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/pingkk/version.h
    @ONLY
)
configure_file(
    src/windows/version.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/pingkk-version.rc
    @ONLY
)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(pingkk_core
    src/core.cpp
    src/diagnostics.cpp
)
target_include_directories(pingkk_core PUBLIC
    include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

if(WIN32)
    target_link_libraries(pingkk_core PUBLIC ws2_32 iphlpapi)
endif()

if(APPLE)
    find_library(SYSTEMCONFIGURATION_FRAMEWORK SystemConfiguration REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(pingkk_core PUBLIC
        ${SYSTEMCONFIGURATION_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
    )
endif()

add_executable(pingkk src/cli/main.cpp)
target_link_libraries(pingkk PRIVATE pingkk_core)
if(WIN32)
    target_sources(pingkk PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated/pingkk-version.rc
    )
endif()

if(PINGKK_BUILD_GUI)
    find_package(Qt6 COMPONENTS Widgets QUIET)
    if(Qt6_FOUND)
        set(PINGKK_QT_MAJOR 6)
    else()
        find_package(Qt5 COMPONENTS Widgets QUIET)
        if(Qt5_FOUND)
            set(PINGKK_QT_MAJOR 5)
        endif()
    endif()
    if(DEFINED PINGKK_QT_MAJOR)
        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        if(APPLE)
            set(CMAKE_MACOSX_BUNDLE ON)
        endif()
        add_executable(pingkk_gui
            src/gui/main.cpp
            src/gui/main_window.cpp
            src/gui/main_window.h
            src/gui/resources.qrc
        )
        if(WIN32)
            target_sources(pingkk_gui PRIVATE
                src/gui/windows.rc
                ${CMAKE_CURRENT_BINARY_DIR}/generated/pingkk-version.rc
            )
            set_target_properties(pingkk_gui PROPERTIES
                OUTPUT_NAME pingkk-gui
                WIN32_EXECUTABLE TRUE
            )
        elseif(APPLE)
            set_source_files_properties(assets/pingkk-icon.icns PROPERTIES
                MACOSX_PACKAGE_LOCATION "Resources"
            )
            target_sources(pingkk_gui PRIVATE assets/pingkk-icon.icns)
            set_target_properties(pingkk_gui PROPERTIES
                OUTPUT_NAME pingkk
                MACOSX_BUNDLE_ICON_FILE pingkk-icon.icns
                MACOSX_BUNDLE_BUNDLE_NAME pingkk
                MACOSX_BUNDLE_GUI_IDENTIFIER cn.trah.pingkk
                MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
                MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
            )
        else()
            set_target_properties(pingkk_gui PROPERTIES OUTPUT_NAME pingkk-gui)
        endif()
        target_link_libraries(pingkk_gui PRIVATE
            pingkk_core
            Qt${PINGKK_QT_MAJOR}::Widgets
        )
        add_dependencies(pingkk_gui pingkk)
        if(APPLE)
            add_custom_target(pingkk_bundle_cli ALL
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:pingkk>
                    $<TARGET_FILE_DIR:pingkk_gui>/pingkk-cli
                DEPENDS pingkk pingkk_gui
                COMMENT "将命令行工具放入 macOS 应用包"
            )
        endif()
    else()
        message(STATUS "未找到 Qt，跳过图形界面，仅构建命令行工具")
    endif()
endif()

install(TARGETS pingkk RUNTIME DESTINATION bin)
if(TARGET pingkk_gui)
    install(TARGETS pingkk_gui
        RUNTIME DESTINATION bin
        BUNDLE DESTINATION .
    )
endif()
