<div align="center">

<img src="assets/branding/lumen_logo.svg" alt="Lumen Logo" width="88" height="88" />

# Lumen

### Lightweight Offline Intelligent Search for Windows

[![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011%20(x64)-0078D4?style=flat-square&logo=windows)](https://github.com/ushalabs/Lumen)
[![Language](https://img.shields.io/badge/Language-C%2B%2B20-00599C?style=flat-square&logo=cplusplus)](https://isocpp.org/)
[![UI](https://img.shields.io/badge/UI-Qt%206%20Widgets%20%2B%20Win32-41CD52?style=flat-square&logo=qt)](https://www.qt.io/)
[![Database](https://img.shields.io/badge/Database-SQLite%203%20(WAL%20%2B%20FTS5)-003B57?style=flat-square&logo=sqlite)](https://www.sqlite.org/)
[![AI](https://img.shields.io/badge/AI-ONNX%20Runtime%20CPU%20(MiniLM%20INT8)-8B5CF6?style=flat-square)](https://onnxruntime.ai/)
[![Privacy](https://img.shields.io/badge/Privacy-100%25%20Offline-10B981?style=flat-square)](https://github.com/ushalabs/Lumen)

**Developed by [UshaLabs](https://github.com/ushalabs)**

</div>

---

## Vision

**Lumen** is a native C++20 desktop file discovery utility for Windows 10 and Windows 11. It combines **Everything-class filename speed**, **full-text document search**, and **on-device semantic retrieval** inside a compact, persistent search bar that lives directly on the Windows desktop.

> **Core Engineering Principle:**  
> *Lumen remains nearly invisible to the operating system when idle while staying immediately accessible on the desktop.*

- **Zero Cloud Dependencies:** No accounts, no telemetry, no external APIs, and no network traffic at runtime.
- **Native Desktop Integration:** A persistent, compact desktop search widget (`400 × 50` px) positioned in the upper-right desktop work area—plus a global keyboard shortcut (`Ctrl+Alt+Space`) from any running application.
- **On-Demand Offline AI:** Bundled INT8-quantized `all-MiniLM-L6-v2` (384-D) model via dynamically loaded ONNX Runtime CPU (`LoadLibraryW` + `ORT_API_MANUAL_INIT`). Never loaded at startup; automatically unloaded after background indexing or search inactivity to reclaim working set pages.
- **Exceptional Resource Efficiency:** Consumes **~0.01% idle CPU** and **~27–30 MB Working Set RAM** (`~5.5 MB` private memory), with **~34–87 ms startup**.

---

## Screenshots

### Persistent Desktop Search Bar (Idle State)
![Lumen Desktop Search Bar — Idle](docs/screenshots/phase0_desktop_bar_idle.png)

### Live Filename Search Results (Virtualized Dropdown)
![Lumen Desktop Search Results — Phase 1](docs/screenshots/phase1_desktop_search_results.png)

### Full-Text Document Content Search (`PDF` Page Number + Highlighted Snippets)
![Lumen Document Content Search Results — Phase 2](docs/screenshots/phase2_content_search_results.png)

### Offline Semantic Search (Zero-Keyword-Overlap Natural Language Discovery)
![Lumen Offline Semantic Search Results — Phase 3](docs/screenshots/phase3_semantic_search_results.png)

### Native On-Demand Settings Dialog
![Lumen Settings Dialog](docs/screenshots/phase0_settings_dialog.png)

---

## Architecture

```mermaid
flowchart LR
    subgraph UI["Native UI Layer (Qt 6 Widgets + Win32)"]
        Bar["DesktopSearchBar\n(400×50 px Desktop Pill)"]
        Popup["SearchPopupWidget\n(Virtualized QListView + Snippets)"]
        Tray["TrayController &\nOn-Demand SettingsDialog"]
    end

    subgraph Platform["Windows Platform Layer"]
        WinInt["DesktopIntegration\n(HWND_BOTTOM + WinEventHook + RegisterHotKey)"]
        Single["SingleInstanceGuard\n(Named Kernel Mutex)"]
        Watcher["DirectoryWatcher\n(ReadDirectoryChangesW OVERLAPPED)"]
    end

    subgraph Engine["Indexing, Extraction, Semantic & Search Engine"]
        Indexer["FileIndexer\n(FindFirstFileExW + EcoQoS Throttling)"]
        Extract["DocumentExtractor + DeflateCodec\n(.txt, .md, .docx, .pdf)"]
        Sem["SemanticEmbeddingEngine + WordPieceTokenizer\n(Lazy LoadLibraryW ONNX CPU + all-MiniLM-L6-v2 INT8)"]
        Search["FileSearchEngine\n(Hybrid Lexical + FTS5 + 384-D Cosine Vector Search)"]
    end

    subgraph Storage["Embedded Storage (%LOCALAPPDATA%\\UshaLabs\\Lumen)"]
        DB[("SQLite 3.47 WAL (Schema v3)\nfiles + files_fts (Trigram)\ndocument_content + document_fts (BM25)\ndocument_chunks (384-D float32 BLOBs)")]
        Cfg["settings.json &\nRotating Logs (1 MB cap)"]
    end

    Bar --> Popup
    Bar --> Search
    WinInt --> Bar
    Watcher --> Indexer
    Indexer --> Extract
    Indexer --> Sem
    Extract --> DB
    Sem --> DB
    Search --> Sem
    Search --> DB
    Tray --> Cfg
```

---

## Development Roadmap & Phase Status

| Phase | Milestone | Status | Key Deliverables |
|---|---|---|---|
| **Phase 0** | **Native Foundation + Desktop Search Bar** | **Completed & Verified** | Native `WIN32` C++20/Qt6 executable, upper-right desktop search bar, Win32 `HWND_BOTTOM` + `Win+D` integration, `RegisterHotKey`, single-instance mutex, system tray, SQLite WAL + FTS5 schema, rotating logger |
| **Phase 1** | **Ultrafast Filename Indexing** | **Completed & Verified** | `FindFirstFileExW` background enumeration, `ReadDirectoryChangesW` real-time watcher, 5,000-row batch SQLite reconciliation, B-tree prefix + FTS5 trigram search engine, relevance ranking, virtualized results UI |
| **Phase 2** | **Document Content Search** | **Completed & Verified** | Zero-external-dependency native extractors for `.txt`, `.md`, `.docx` (ZIP + WordprocessingML), and multi-page `.pdf` (`/FlateDecode` + `BT...ET` parser + page offsets), RFC 1951/1950 Deflate with 16 MB ZIP-bomb cap, FTS5 `unicode61` BM25 content search, highlighted snippets & PDF page badges |
| **Phase 3** | **Offline Semantic Search** | **Completed & Verified** | Lazy-loaded ONNX Runtime CPU inference (`all-MiniLM-L6-v2` INT8 quantized, 384-D), native BERT WordPiece tokenizer, 350-word overlapping document chunker, SQLite `document_chunks` vector store, hybrid lexical + semantic ranking, `findRelatedDocuments`, automatic memory unloading (`_heapmin` + `SetProcessWorkingSetSizeEx`) |
| **Phase 4** | **Complete Desktop UX** | **In Progress** | Search modes (`All`, `Files`, `Content`, `Semantic`), quick file preview, context menu actions, `Find Related Documents`, light/dark/system themes |
| **Phase 5** | **Production Hardening + Microsoft Store** | Planned | Integrity recovery, full settings suite, standalone Windows installer + MSIX package, full benchmark suite |

---

## Verified Performance Benchmarks

### Benchmark Environment
- **Hardware:** HP EliteBook 840 G8 Notebook PC
- **Processor:** 11th Gen Intel Core i7-1185G7 @ 3.00 GHz (4 Cores / 8 Logical Processors)
- **Memory:** 16.0 GB DDR4 (`15.69 GB` usable) · NVMe SSD
- **OS:** Microsoft Windows 11 Pro x64 (`10.0.26200`, Build `26200`)
- **Toolchain:** MSVC `19.44.35229.0` (Visual Studio 2022 v17.14.41, `Release /O2`) · Qt `6.8.2` (`msvc2022_64`)

### Phase 0 — Foundation & Idle Resource Benchmarks

| Operation / Metric | Engineering Target | Measured Result | Status |
|---|---|---|---|
| **Warm Startup to Visible Desktop Bar** | `< 500 ms` | **`33.83 ms` avg** (`32.79 ms` – `36.41 ms` warm; `51.44 ms` cold) | **PASSED** (~14× faster) |
| **Idle CPU (2-Minute Controlled Test)** | `< 0.1%` avg | **`0.0179%` avg** (`171.88 ms` CPU time across `120.06 s`) | **PASSED** (>5.5× under budget) |
| **Idle Working Set RAM (Desktop Bar Visible)** | `< 120 MB` (`< 70 MB` stretch) | **`30.44 MB`** (`5.58 MB` Private Bytes) | **PASSED** (Beats stretch goal) |
| **Standalone `Lumen.exe` Binary Size** | Compact | **`1.45 MB`** (statically embeds SQLite 3.47.2 + FTS5 + native extractors + WordPiece) | **VERIFIED** |

### Phase 1 — 100,000-File Indexing & Search Benchmarks (`phase1_benchmark.exe`)

| Operation / Metric | Engineering Target | Measured Result (100,000 Physical NTFS Files) | Status |
|---|---|---|---|
| **Initial Indexing Time (`100,000` files)** | Fast background scan | **`5.28 s`** (`5,282.11 ms` · **`18,932 files/sec`**) | **PASSED** |
| **Metadata + FTS5 Trigram Index Size** | `< 100 MB` | **`52.55 MB`** (`55,107,584 bytes`) | **PASSED** (47% below target) |
| **Filename Search Latency — P50 (Median)** | Fast | **`4.89 ms`** (200 diverse warm-cache queries) | **PASSED** |
| **Filename Search Latency — P95** | `< 50 ms` | **`10.89 ms`** (Min: `1.41 ms`, P99: `11.36 ms`, Max: `11.99 ms`) | **PASSED** (>4.5× faster) |
| **Indexing Background CPU Usage** | Throttled | **`11.72%` avg** across 8 logical cores (`THREAD_PRIORITY_BELOW_NORMAL` + EcoQoS) | **PASSED** |
| **RAM Usage During / After Indexing** | `< 250 MB` | **`21.73 MB` Working Set** (`Peak: 24.68 MB`, `Private: 8.47 MB`) | **PASSED** (>10× under budget) |
| **Real-Time Incremental Update Latency** | Instant | **Add: `0.92 ms` · Rename: `0.48 ms` · Delete: `0.41 ms`** | **PASSED** |
| **Post-Indexing Idle CPU** | `< 0.1%` | **`0.0000%`** | **PASSED** |
| **Post-Indexing Idle Working Set RAM** | `< 120 MB` | **`27.05 MB`** (`14.35 MB` Private Bytes) | **PASSED** |

### Phase 2 — Document Content Extraction & Full-Text Search Benchmarks (`phase2_benchmark.exe`)

| Operation / Metric | Engineering Target | Measured Result (1,000 Physical Documents: 250 TXT, 250 MD, 250 DOCX, 250 3-Page PDF) | Status |
|---|---|---|---|
| **End-to-End Indexing (Metadata + Content + FTS5)** | Background throttled | **`878.27 ms`** (`1,000 / 1,000` documents · **`1,138 docs/sec`**) | **PASSED** |
| **Markdown (`.md`) Extraction Speed** | Fast | **`0.20 ms / doc`** (`50.55 ms` for 250 docs) | **PASSED** |
| **PDF (`.pdf` 3-page `/FlateDecode`) Extraction Speed** | Fast | **`0.26 ms / doc`** (`66.18 ms` for 250 3-page PDFs with page offset tracking) | **PASSED** |
| **Word (`.docx` ZIP + XML) Extraction Speed** | Fast | **`0.31 ms / doc`** (`77.43 ms` for 250 DOCX archives) | **PASSED** |
| **Text (`.txt`) Extraction Speed** | Fast | **`10.15 ms / doc` cold** (`0.20 ms / doc` warm) | **PASSED** |
| **Full-Text Search Latency — P50 (Median)** | Fast | **`4.70 ms`** (BM25 + `snippet()` + PDF page resolution) | **PASSED** |
| **Full-Text Search Latency — P95** | `< 150 ms` | **`8.70 ms`** (Max: `10.63 ms` across 30 queries) | **PASSED** (>17× faster) |
| **Content Index Storage Growth** | Compact | **`0.98 MB`** (`1.00 KB / document` including FTS5 inverted index) | **PASSED** |
| **Full-Text Search Working Set RAM** | `< 250 MB` | **`18.14 MB` Working Set** (`5.52 MB` Private Bytes) | **PASSED** (>13× under budget) |

### Phase 3 — Offline Semantic Search & ONNX Runtime CPU Benchmarks (`phase3_benchmark.exe`)

| Operation / Metric | Engineering Target | Measured Result (`all-MiniLM-L6-v2` INT8 Quantized, 384-D, CPU Only) | Status |
|---|---|---|---|
| **Lazy ONNX Model Load + First Inference** | On-demand only | **`236.68 ms`** (`onnxruntime.dll` dynamically loaded via `LoadLibraryW`) | **PASSED** |
| **Embedding Generation Throughput (CPU)** | Fast background | **`127.2 chunks/sec`** (**`7.86 ms / chunk`**, `1,965.73 ms` for 250 documents) | **PASSED** |
| **Semantic Search Latency — P50 (Median)** | Fast | **`4.16 ms`** (query embedding + 384-D cosine similarity scan + hybrid rank) | **PASSED** |
| **Semantic Search Latency — P95** | `< 150 ms` | **`5.88 ms`** (Max: `11.21 ms` across 40 natural-language queries) | **PASSED** (>25× faster) |
| **Vector Index Storage Growth** | Compact | **`500.00 KB`** for 250 chunks (`2,048 bytes / chunk` including metadata & preview) | **PASSED** |
| **Peak RAM During Semantic Indexing** | `< 350 MB` | **`61.42 MB` Working Set** (with disabled CPU arena & adaptive throttling) | **PASSED** (5.7× under budget) |
| **Working Set RAM After `unloadModel()`** | Reclaimed to OS | **`0.25 MB` Working Set** (`17.15 MB` Private; `28.68 MB` full GUI idle) | **PASSED** |

---

## Repository Structure

```text
Lumen/
├── CMakeLists.txt                  # C++20 / MSVC / Qt6 build configuration
├── LICENSE                         # UshaLabs copyright & third-party notices
├── README.md                       # Project documentation & verified benchmarks
├── assets/
│   ├── app.rc                      # Native Win32 VERSIONINFO & icon resource
│   ├── branding/lumen_logo.svg     # UshaLabs Lumen vector emblem
│   ├── icons/lumen.ico             # Multi-resolution Windows icon
│   └── models/all-MiniLM-L6-v2/    # Bundled offline INT8 ONNX model (21.9 MB) & vocab.txt
├── src/
│   ├── main.cpp                    # Entry point, single-instance check, CLI harness
│   ├── app/                        # LumenApplication & TrayController
│   ├── core/                       # AppPaths, rotating Logger, SettingsManager
│   ├── database/                   # DatabaseManager (SQLite3 WAL + FTS5 Schema v3)
│   ├── extraction/                 # DocumentExtractor (.txt/.md/.docx/.pdf) & DeflateCodec
│   ├── semantic/                   # SemanticEmbeddingEngine (ONNX CPU), WordPieceTokenizer, DocumentChunker
│   ├── indexing/                   # FileIndexer (metadata + content + semantic passes) & DirectoryWatcher
│   ├── search/                     # FileSearchEngine (Hybrid lexical + FTS5 + 384-D cosine vector search)
│   ├── platform/windows/           # DesktopIntegration & SingleInstanceGuard
│   └── ui/                         # DesktopSearchBar, SearchPopupWidget, SettingsDialog
├── tests/
│   ├── phase0_tests.cpp            # Phase 0 automated QtTest suite (10 tests)
│   ├── phase1_tests.cpp            # Phase 1 automated QtTest suite (7 tests)
│   ├── phase2_tests.cpp            # Phase 2 automated QtTest suite (7 tests)
│   └── phase3_tests.cpp            # Phase 3 automated QtTest suite (6 tests)
├── benchmarks/
│   ├── phase1_benchmark.cpp        # Reproducible 100,000-file NTFS filename benchmark
│   ├── phase2_benchmark.cpp        # Reproducible 1,000-document content & FTS5 benchmark
│   └── phase3_benchmark.cpp        # Reproducible 250-document ONNX CPU semantic benchmark
├── third_party/
│   ├── onnxruntime/                # ONNX Runtime v1.20.1 Windows x64 C/C++ headers & CPU runtime
│   └── sqlite3/                    # Bundled SQLite 3.47.2 amalgamation
└── docs/
    └── screenshots/                # Verified native application screenshots
```

---

## Building from Source

### Prerequisites
- **OS:** Windows 10 or Windows 11 (x64)
- **Compiler:** Visual Studio 2022 Build Tools (MSVC v143 x64 C++ compiler & Windows 10/11 SDK)
- **Build System:** CMake 3.24+ and Ninja
- **UI Framework:** Qt 6.8+ (`msvc2022_64`)

### 1. Configure & Compile (`Release`)

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/msvc2022_64
cmake --build build --config Release
```

### 2. Run Automated Tests (30 Tests across Phases 0, 1, 2, and 3)

```powershell
$env:PATH = "C:\Qt\6.8.2\msvc2022_64\bin;$env:PATH"
ctest --test-dir build --output-on-failure -V
```

### 3. Run the Phase 1, Phase 2 & Phase 3 Benchmarks

```powershell
$env:PATH = "C:\Qt\6.8.2\msvc2022_64\bin;$env:PATH"
.\build\phase1_benchmark.exe
.\build\phase2_benchmark.exe
.\build\phase3_benchmark.exe
```

### 4. Deploy Standalone Release Bundle

```powershell
New-Item -ItemType Directory -Force -Path build\deploy | Out-Null
Copy-Item build\Lumen.exe build\deploy\Lumen.exe -Force
Copy-Item third_party\onnxruntime\lib\*.dll build\deploy\ -Force
C:\Qt\6.8.2\msvc2022_64\bin\windeployqt.exe --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-network build\deploy\Lumen.exe
.\build\deploy\Lumen.exe
```

---

## Privacy & Security Architecture

- **Default Indexing Scope:** Only indexes user-approved personal folders on first run (`Documents`, `Downloads`, `Desktop`) resolved via Windows `SHGetKnownFolderPath`.
- **Automatic Exclusions:** Ignores system directories (`Windows`, `Program Files`, `AppData`), developer caches (`.git`, `node_modules`, `venv`, `.venv`, `__pycache__`, `build`), and sensitive credential files (`.env`, `id_rsa`, `.pem`, `.key`, `.pfx`).
- **Reparse Point Safety:** Skips `FILE_ATTRIBUTE_REPARSE_POINT` directories during enumeration to prevent symlink or NTFS junction loops.
- **Local User-Scoped Storage:** All index databases, settings, and capped logs are stored strictly inside `%LOCALAPPDATA%\UshaLabs\Lumen\`.

---

## License

Copyright © 2026 **UshaLabs**. All rights reserved. See [LICENSE](LICENSE) for details.
