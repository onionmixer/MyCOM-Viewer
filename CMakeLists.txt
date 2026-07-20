cmake_minimum_required(VERSION 3.16)

project(mycom_mvb_converter VERSION 0.7.1 LANGUAGES CXX)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTORCC ON)

# Source files include Korean UI and recovery strings. MSVC otherwise interprets
# UTF-8 source through the active Windows code page, which can corrupt literals
# and even produce false unterminated-string errors.
if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING)
endif()

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Widgets Multimedia MultimediaWidgets)

set(MYCOM_VIEWER_SOURCES
    src/viewer_main.cpp
    src/archive_manifest.cpp
    src/archive_tool_locator.cpp
    src/bookmark_store.cpp
    resources/app.qrc
)

add_executable(mycom-archive-build
    src/main.cpp
    src/iso9660_extractor.cpp
)

target_link_libraries(mycom-archive-build PRIVATE Qt5::Core)

target_compile_options(mycom-archive-build PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_executable(mycom-viewer ${MYCOM_VIEWER_SOURCES})

target_link_libraries(mycom-viewer PRIVATE Qt5::Core Qt5::Widgets Qt5::Multimedia Qt5::MultimediaWidgets)

target_compile_options(mycom-viewer PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

# Native application metadata. The executable stays a normal command-line
# program on Linux, while Windows and macOS receive their native app wrappers.
if(WIN32)
    set(MYCOM_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/mycom-viewer.ico")
    set(MYCOM_WINDOWS_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/mycom-viewer.manifest")
    configure_file(
        packaging/windows/mycom-viewer.rc.in
        "${CMAKE_CURRENT_BINARY_DIR}/mycom-viewer.rc"
        @ONLY
    )
    target_sources(mycom-viewer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/mycom-viewer.rc")
    target_link_options(mycom-viewer PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${MYCOM_WINDOWS_MANIFEST}"
    )
    set_target_properties(mycom-viewer PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
    set(MYCOM_MACOS_ICONSET "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/mycom-viewer.iconset")
    set(MYCOM_MACOS_ICON "${CMAKE_CURRENT_BINARY_DIR}/mycom-viewer.icns")
    add_custom_command(
        OUTPUT "${MYCOM_MACOS_ICON}"
        COMMAND iconutil -c icns "${MYCOM_MACOS_ICONSET}" -o "${MYCOM_MACOS_ICON}"
        DEPENDS
            "${MYCOM_MACOS_ICONSET}/icon_16x16.png"
            "${MYCOM_MACOS_ICONSET}/icon_16x16@2x.png"
            "${MYCOM_MACOS_ICONSET}/icon_32x32.png"
            "${MYCOM_MACOS_ICONSET}/icon_32x32@2x.png"
            "${MYCOM_MACOS_ICONSET}/icon_128x128.png"
            "${MYCOM_MACOS_ICONSET}/icon_128x128@2x.png"
            "${MYCOM_MACOS_ICONSET}/icon_256x256.png"
            "${MYCOM_MACOS_ICONSET}/icon_256x256@2x.png"
            "${MYCOM_MACOS_ICONSET}/icon_512x512.png"
            "${MYCOM_MACOS_ICONSET}/icon_512x512@2x.png"
        COMMENT "Creating MYCOM Viewer macOS icon"
        VERBATIM
    )
    add_custom_target(mycom-macos-icon DEPENDS "${MYCOM_MACOS_ICON}")
    add_dependencies(mycom-viewer mycom-macos-icon)
    set_source_files_properties("${MYCOM_MACOS_ICON}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(mycom-viewer PRIVATE "${MYCOM_MACOS_ICON}")
    set_source_files_properties(README.txt LICENSE THIRD_PARTY_NOTICES.md
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(mycom-viewer PRIVATE README.txt LICENSE THIRD_PARTY_NOTICES.md)
    set_target_properties(mycom-viewer PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "MYCOM Viewer"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.onionmixer.mycomviewer"
        MACOSX_BUNDLE_ICON_FILE "mycom-viewer.icns"
    )
endif()

include(GNUInstallDirs)

set(MYCOM_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
set(MYCOM_INSTALL_DOCDIR "${CMAKE_INSTALL_DATADIR}/doc/mycom-viewer")
set(MYCOM_VIEWER_BUNDLE_DESTINATION ".")
if(APPLE)
    # The PKG installs the GUI application in the conventional location, while
    # preserving the builder as a command-line tool under /usr/local.
    set(MYCOM_INSTALL_BINDIR "usr/local/bin")
    set(MYCOM_INSTALL_DOCDIR "usr/local/share/doc/mycom-viewer")
    set(MYCOM_VIEWER_BUNDLE_DESTINATION "Applications")
endif()

install(TARGETS mycom-archive-build mycom-viewer
    RUNTIME DESTINATION "${MYCOM_INSTALL_BINDIR}"
    BUNDLE DESTINATION "${MYCOM_VIEWER_BUNDLE_DESTINATION}"
    COMPONENT Runtime
)
install(FILES
    README.md
    README.txt
    CHANGELOG.md
    LICENSE
    THIRD_PARTY_NOTICES.md
    licenses/qt/LICENSE.LGPL-3.0.txt
    licenses/qt/LICENSE.GPL-3.0.txt
    DESTINATION "${MYCOM_INSTALL_DOCDIR}"
    COMPONENT Runtime
)
install(FILES resources/icons/mycom-viewer.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" COMPONENT Runtime)

if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/mycom-viewer.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT Runtime)
endif()

option(MYCOM_DEPLOY_QT_RUNTIME "Deploy the Qt runtime during a native package install" ON)
if(MYCOM_DEPLOY_QT_RUNTIME AND (WIN32 OR APPLE))
    get_filename_component(MYCOM_QT_PREFIX "${Qt5Core_DIR}/../../.." ABSOLUTE)
endif()
if(WIN32 AND MYCOM_DEPLOY_QT_RUNTIME)
    find_program(MYCOM_WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt windeployqt.exe
        HINTS "${MYCOM_QT_PREFIX}/bin"
    )
    if(NOT MYCOM_WINDEPLOYQT_EXECUTABLE)
        message(FATAL_ERROR
            "windeployqt was not found. Set MYCOM_DEPLOY_QT_RUNTIME=OFF for a developer-only install, "
            "or make the Qt bin directory available while configuring a release build.")
    endif()
    configure_file(
        cmake/DeployQtRuntime.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/DeployQtRuntime.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeployQtRuntime.cmake" COMPONENT Runtime)
elseif(APPLE AND MYCOM_DEPLOY_QT_RUNTIME)
    find_program(MYCOM_MACDEPLOYQT_EXECUTABLE
        NAMES macdeployqt
        HINTS "${MYCOM_QT_PREFIX}/bin"
    )
    if(NOT MYCOM_MACDEPLOYQT_EXECUTABLE)
        message(FATAL_ERROR
            "macdeployqt was not found. Set MYCOM_DEPLOY_QT_RUNTIME=OFF for a developer-only install, "
            "or make the Qt bin directory available while configuring a release build.")
    endif()
    set(MYCOM_MACOS_ARCHIVE_BUILD_RELATIVE "${MYCOM_INSTALL_BINDIR}/mycom-archive-build")
    configure_file(
        cmake/DeployMacQtRuntime.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/DeployMacQtRuntime.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeployMacQtRuntime.cmake" COMPONENT Runtime)
endif()

option(MYCOM_INSTALL_ARCHIVE "Install a prebuilt MYCOM archive with the viewer" OFF)
set(MYCOM_ARCHIVE_DIR "" CACHE PATH "Prebuilt mycom-archive directory to install")
if(MYCOM_INSTALL_ARCHIVE)
    if(NOT MYCOM_ARCHIVE_DIR OR NOT EXISTS "${MYCOM_ARCHIVE_DIR}/manifest.json")
        message(FATAL_ERROR "MYCOM_INSTALL_ARCHIVE requires MYCOM_ARCHIVE_DIR with manifest.json")
    endif()
    install(DIRECTORY "${MYCOM_ARCHIVE_DIR}/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/mycom-viewer/archive" COMPONENT Runtime)
endif()

set(CPACK_PACKAGE_NAME "MYCOM-Viewer")
set(CPACK_PACKAGE_VENDOR "onionmixer")
set(CPACK_PACKAGE_CONTACT "onionmixer")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "MYCOM archive builder and Qt5 viewer")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_STRIP_FILES TRUE)
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.txt")
# Windows and Debian ship a single payload. macOS overrides this below because
# productbuild needs an explicit component package in its Distribution XML.
set(CPACK_MONOLITHIC_INSTALL ON)

if(WIN32)
    set(CPACK_GENERATOR "NSIS")
    set(CPACK_PACKAGE_FILE_NAME "mycom-viewer-${PROJECT_VERSION}-win64")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "MYCOM Viewer")
    set(CPACK_NSIS_DISPLAY_NAME "MYCOM Viewer")
    set(CPACK_NSIS_PACKAGE_NAME "MYCOM Viewer")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/mycom-viewer.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/mycom-viewer.ico")
    # Keep these as '/' paths. CPack serializes them into CMake code, where
    # unescaped Windows backslashes would be parsed as invalid escape sequences.
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin/mycom-viewer.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_IGNORE_LICENSE_PAGE ON)
    # CPack prefixes this option with CPACK_NSIS_EXECUTABLES_DIRECTORY (bin).
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "../share/doc/mycom-viewer/README.txt")
    set(CPACK_NSIS_MENU_LINKS
        "share/doc/mycom-viewer/README.txt" "MYCOM Viewer README")
    list(APPEND CPACK_NSIS_MENU_LINKS
        "share/doc/mycom-viewer/LICENSE" "MYCOM Viewer License"
        "share/doc/mycom-viewer/THIRD_PARTY_NOTICES.md" "Third-party notices")
    set(CPACK_PACKAGE_EXECUTABLES "mycom-viewer;MYCOM Viewer")
elseif(APPLE)
    set(CPACK_GENERATOR "productbuild")
    set(CPACK_MONOLITHIC_INSTALL OFF)
    set(CPACK_COMPONENTS_ALL Runtime)
    set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "MYCOM Viewer")
    set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "MYCOM Viewer application and archive builder")
    set(CPACK_COMPONENT_RUNTIME_REQUIRED TRUE)
    set(CPACK_PACKAGE_FILE_NAME "mycom-viewer-${PROJECT_VERSION}-macos")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/")
    set(CPACK_PRODUCTBUILD_IDENTIFIER "com.onionmixer.mycomviewer")
    set(CPACK_PRODUCTBUILD_DOMAINS ON)
    set(CPACK_PRODUCTBUILD_DOMAINS_ANYWHERE OFF)
    set(CPACK_PRODUCTBUILD_DOMAINS_USER OFF)
    set(CPACK_PRODUCTBUILD_DOMAINS_ROOT ON)
else()
    set(CPACK_GENERATOR "DEB")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_DEBIAN_PACKAGE_NAME "mycom-viewer")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_RELEASE "1")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "onionmixer")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
        "MYCOM archive builder and Qt5 viewer")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()
include(CPack)

enable_testing()

add_executable(mycom-bookmark-store-test
    src/bookmark_store_test.cpp
    src/bookmark_store.cpp
)

target_link_libraries(mycom-bookmark-store-test PRIVATE Qt5::Core)

target_compile_options(mycom-bookmark-store-test PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_test(NAME mycom-bookmark-store COMMAND mycom-bookmark-store-test)

add_executable(mycom-archive-manifest-test
    src/archive_manifest_test.cpp
    src/archive_manifest.cpp
)

target_link_libraries(mycom-archive-manifest-test PRIVATE Qt5::Core)

target_compile_options(mycom-archive-manifest-test PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_test(NAME mycom-archive-manifest COMMAND mycom-archive-manifest-test)

add_executable(mycom-archive-tool-locator-test
    src/archive_tool_locator_test.cpp
    src/archive_tool_locator.cpp
)

target_link_libraries(mycom-archive-tool-locator-test PRIVATE Qt5::Core)

target_compile_options(mycom-archive-tool-locator-test PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_test(NAME mycom-archive-tool-locator COMMAND mycom-archive-tool-locator-test)

add_executable(mycom-iso9660-extractor-test
    src/iso9660_extractor_test.cpp
    src/iso9660_extractor.cpp
)

target_link_libraries(mycom-iso9660-extractor-test PRIVATE Qt5::Core)

target_compile_options(mycom-iso9660-extractor-test PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_test(NAME mycom-iso9660-extractor COMMAND mycom-iso9660-extractor-test)

add_test(NAME mycom-archive-iso-integration
    COMMAND ${CMAKE_COMMAND}
        -DARCHIVE_BUILD=$<TARGET_FILE:mycom-archive-build>
        -DARCHIVE_WORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/mycom-iso-integration-archive
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/archive_iso_integration.cmake
)

set_tests_properties(mycom-archive-iso-integration PROPERTIES
    LABELS "integration;iso"
    TIMEOUT 1800
)
