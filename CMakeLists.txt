cmake_minimum_required(VERSION 3.16)

project(mycom_mvb_converter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Widgets Multimedia MultimediaWidgets)

add_executable(mycom-archive-build
    src/main.cpp
)

target_link_libraries(mycom-archive-build PRIVATE Qt5::Core)

target_compile_options(mycom-archive-build PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_executable(mycom-viewer
    src/viewer_main.cpp
    src/archive_manifest.cpp
    src/bookmark_store.cpp
)

target_link_libraries(mycom-viewer PRIVATE Qt5::Core Qt5::Widgets Qt5::Multimedia Qt5::MultimediaWidgets)

target_compile_options(mycom-viewer PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

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
