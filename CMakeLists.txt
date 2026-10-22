cmake_minimum_required(VERSION 3.24)

project(Lumen
    VERSION 0.3.0
    DESCRIPTION "Lumen — Lightweight Offline Intelligent Search for Windows"
    HOMEPAGE_URL "https://ushalabs.com"
    LANGUAGES C CXX RC
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(MSVC)
    add_compile_options(/W4 /MP /utf-8 /permissive-)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)

# ------------------------------------------------------------------------------
# Embedded SQLite3 Static Target with FTS5 (and Trigram tokenizer) Enabled
# ------------------------------------------------------------------------------
add_library(lumen_sqlite3 STATIC
    third_party/sqlite3/sqlite3.c
    third_party/sqlite3/sqlite3.h
)

target_include_directories(lumen_sqlite3 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3
)

target_compile_definitions(lumen_sqlite3 PUBLIC
    SQLITE_ENABLE_FTS5=1
    SQLITE_THREADSAFE=2
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_TEMP_STORE=2
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_OMIT_DEPRECATED=1
)

if(MSVC)
    target_compile_options(lumen_sqlite3 PRIVATE /W0)
endif()

# ------------------------------------------------------------------------------
# Lumen Core, Indexing, Extraction, Search & UI Static Library
# ------------------------------------------------------------------------------
set(LUMEN_LIB_SOURCES
    src/core/AppPaths.h
    src/core/AppPaths.cpp
    src/core/Logger.h
    src/core/Logger.cpp
    src/core/SettingsManager.h
    src/core/SettingsManager.cpp
    src/database/DatabaseManager.h
    src/database/DatabaseManager.cpp
    src/extraction/DeflateCodec.h
    src/extraction/DeflateCodec.cpp
    src/extraction/DocumentExtractor.h
    src/extraction/DocumentExtractor.cpp
    src/indexing/DirectoryWatcher.h
    src/indexing/DirectoryWatcher.cpp
    src/indexing/FileIndexer.h
    src/indexing/FileIndexer.cpp
    src/search/FileSearchEngine.h
    src/search/FileSearchEngine.cpp
    src/platform/windows/SingleInstanceGuard.h
    src/platform/windows/SingleInstanceGuard.cpp
    src/platform/windows/DesktopIntegration.h
    src/platform/windows/DesktopIntegration.cpp
    src/ui/BrandingIcon.h
    src/ui/BrandingIcon.cpp
    src/ui/SearchResults/SearchPopupWidget.h
    src/ui/SearchResults/SearchPopupWidget.cpp
    src/ui/DesktopSearchBar/DesktopSearchBar.h
    src/ui/DesktopSearchBar/DesktopSearchBar.cpp
    src/ui/Settings/SettingsDialog.h
    src/ui/Settings/SettingsDialog.cpp
    src/app/TrayController.h
    src/app/TrayController.cpp
    src/app/LumenApplication.h
    src/app/LumenApplication.cpp
)

add_library(lumen_lib STATIC ${LUMEN_LIB_SOURCES})

target_include_directories(lumen_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(lumen_lib PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    lumen_sqlite3
    user32
    shell32
    ole32
    advapi32
    psapi
)

# ------------------------------------------------------------------------------
# Primary Native Windows Executable (WIN32 = No Console Window)
# ------------------------------------------------------------------------------
add_executable(Lumen WIN32
    src/main.cpp
    assets/app.rc
)

target_link_libraries(Lumen PRIVATE
    lumen_lib
)

if(MSVC)
    target_link_options(Lumen PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
    )
endif()

# ------------------------------------------------------------------------------
# Automated Phase 0, Phase 1 & Phase 2 Test Suites + Benchmarks
# ------------------------------------------------------------------------------
enable_testing()

add_executable(phase0_tests
    tests/phase0_tests.cpp
)
target_link_libraries(phase0_tests PRIVATE
    lumen_lib
    Qt6::Test
)
add_test(NAME Phase0Tests COMMAND phase0_tests)

add_executable(phase1_tests
    tests/phase1_tests.cpp
)
target_link_libraries(phase1_tests PRIVATE
    lumen_lib
    Qt6::Test
)
add_test(NAME Phase1Tests COMMAND phase1_tests)

add_executable(phase2_tests
    tests/phase2_tests.cpp
)
target_link_libraries(phase2_tests PRIVATE
    lumen_lib
    Qt6::Test
)
add_test(NAME Phase2Tests COMMAND phase2_tests)

add_executable(phase1_benchmark
    benchmarks/phase1_benchmark.cpp
)
target_link_libraries(phase1_benchmark PRIVATE
    lumen_lib
)

add_executable(phase2_benchmark
    benchmarks/phase2_benchmark.cpp
)
target_link_libraries(phase2_benchmark PRIVATE
    lumen_lib
)
