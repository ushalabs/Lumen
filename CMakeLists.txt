cmake_minimum_required(VERSION 3.24)

project(Lumen
    VERSION 0.5.1
    DESCRIPTION "Lumen - Lightweight Offline Intelligent Search for Windows"
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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

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
# Lumen Core, Indexing, Extraction, Semantic, Search & UI Static Library
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
    src/extraction/EnterpriseExtractors.cpp
    src/extraction/PdfiumBackend.h
    src/extraction/PdfiumBackend.cpp
    src/semantic/WordPieceTokenizer.h
    src/semantic/WordPieceTokenizer.cpp
    src/semantic/DocumentChunker.h
    src/semantic/DocumentChunker.cpp
    src/semantic/SemanticEmbeddingEngine.h
    src/semantic/SemanticEmbeddingEngine.cpp
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
    src/ui/Theme.h
    src/ui/Theme.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/include
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
    assets/lumen.manifest
)

target_link_libraries(Lumen PRIVATE
    lumen_lib
)

if(MSVC)
    target_link_options(Lumen PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>
    )
endif()

set(LUMEN_PDFIUM_DLL "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/bin/pdfium.dll")
if(EXISTS "${LUMEN_PDFIUM_DLL}")
    add_custom_command(TARGET Lumen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LUMEN_PDFIUM_DLL}"
            "$<TARGET_FILE_DIR:Lumen>/pdfium.dll"
        COMMENT "Copying pdfium.dll alongside Lumen.exe"
    )
endif()

option(LUMEN_BUILD_DEV_TOOLS "Build developer-only screenshot and measurement tools" OFF)
if(LUMEN_BUILD_DEV_TOOLS)
    add_executable(capture_screenshots WIN32
        tools/capture_screenshots.cpp
        assets/app.rc
    )
    target_link_libraries(capture_screenshots PRIVATE
        lumen_lib
    )
endif()

# ------------------------------------------------------------------------------
# Automated Benchmark Executables
# ------------------------------------------------------------------------------

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
if(EXISTS "${LUMEN_PDFIUM_DLL}")
    add_custom_command(TARGET phase2_benchmark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LUMEN_PDFIUM_DLL}"
            "$<TARGET_FILE_DIR:phase2_benchmark>/pdfium.dll"
    )
endif()

add_executable(phase3_benchmark
    benchmarks/phase3_benchmark.cpp
)
target_link_libraries(phase3_benchmark PRIVATE
    lumen_lib
)
