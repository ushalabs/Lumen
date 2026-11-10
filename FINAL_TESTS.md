# LUMEN — FINAL PRODUCTION ACCEPTANCE TEST

**Project:** Lumen — Offline Intelligent Search for Windows  
**Developer:** UshaLabs  
**Repository:** https://github.com/ushalabs/Lumen  
**Target Platform:** Windows 10 / Windows 11 (x64)  
**Application Version:** `0.5.1` (`FILEVERSION 0,5,1,0`)  
**Tested Baseline & Fix Verification Window:** `2026-11-11 00:02–00:56 +05:00` (`49ab168` baseline → `v0.5.1` stabilization fixes)  
**Release Readiness Verdict:** **`CONDITIONAL PASS — RELEASE CANDIDATE`** *(All P1 release blockers resolved in code/packaging; documented P2/P3 issues, unsigned binary SmartScreen warning, unverified multi-DPI/Office environments, and user-approved Git history rewrite for historical `.pfx` exposure remain.)*

---

## 1. Executive Summary

- **Overall Acceptance Verdict:** **`CONDITIONAL PASS — RELEASE CANDIDATE`**
- **Application Version:** `0.5.1` ([CMakeLists.txt](CMakeLists.txt), [assets/app.rc](assets/app.rc), [assets/lumen.manifest](assets/lumen.manifest))
- **Verified Revision:** Built on `49ab168396262ff391a8bb4c39655f8e46cc427a` + **Lumen v0.5.1 P1 release blocker fixes (`DEF-01`, `DEF-02`, `DEF-03`)**
- **Test Environment:** Windows 11 Pro (`Build 26200`, 64-bit), 11th Gen Intel Core i7-1185G7 (`4C/8T @ 3.00 GHz`), `15.69 GB` RAM, NVMe SSD (NTFS), single `1536×864` display at `125%` scaling (`120 DPI`), MSVC `19.44.35229.0` (`x64`), CMake `3.31.6-msvc6`, Ninja `1.12.1`, Qt `6.8.2`, SQLite `3.47.2`, ONNX Runtime `1.20.20241119.10.5c1b7cc`, PDFium (`chromium/8152`, BSD-3-Clause), NSIS `v3.12`.
- **Test Date:** `2026-11-11`

### P1 Blocker Fixes Verified in v0.5.1

1. **`DEF-01` (Security & Binary Hygiene — RESOLVED IN WORKING TREE/HEAD):** Removed `--capture-screenshots`, `--measure-startup`, `clearAllFiles()` CLI invocation, and all hardcoded developer paths from production [src/main.cpp](src/main.cpp) (relocated into opt-in developer utility [tools/capture_screenshots.cpp](tools/capture_screenshots.cpp) disabled by default via `LUMEN_BUILD_DEV_TOOLS=OFF`). Verified `Lumen.exe` (`2,109,440 B`, `v0.5.1.0`) contains zero developer path literals or dangerous debug switches. Removed `packaging/certs/UshaLabs.pfx` and `UshaLabs.cer` from Git index/HEAD, removed hardcoded fallback paths & plaintext passwords from [packaging/build_msix.ps1](packaging/build_msix.ps1) and [packaging/build_setup.ps1](packaging/build_setup.ps1), and added secret/certificate exclusions to [.gitignore](.gitignore). *(Git history rewrite / force-push to purge historical commit `05bc8da` paused pending explicit owner authorization.)*
2. **`DEF-02` (Real-World PDF Extraction — RESOLVED):** Integrated native Chromium **PDFium** ([src/extraction/PdfiumBackend.h](src/extraction/PdfiumBackend.h), [src/extraction/PdfiumBackend.cpp](src/extraction/PdfiumBackend.cpp), dynamically loaded `pdfium.dll` with mutex serialization and bounded page/character caps) into [DocumentExtractor::extractPdfBytes](src/extraction/DocumentExtractor.cpp), and updated [DocumentExtractor::buildValidPdfBytes](src/extraction/DocumentExtractor.cpp) to emit ISO-compliant font/resource/`xref` PDFs. Real multi-page Chromium/Edge headless PDFs (3 pages `18.10 ms` and 25 pages `23.55 ms`), Unicode (`Café`, `日本語`, `Ελληνικά`, `Кириллица`), tables, multi-column text, exact page attribution, `80/80` multi-threaded parses, and synthetic/encrypted/corrupt/scanned PDFs all passed.
3. **`DEF-03` (Live Directory Rename Indexing — RESOLVED):** Added cross-buffer `OLD_NAME`/`NEW_NAME` pairing + overflow rescan signaling in [DirectoryWatcher](src/indexing/DirectoryWatcher.cpp), collision-safe/case-aware Unicode-offset subtree updates in [DatabaseManager::renamePath](src/database/DatabaseManager.cpp), and pending-upsert flushing + targeted `reconcileDirectorySubtree()` in [FileIndexer](src/indexing/FileIndexer.cpp). Verified **107/107** automated populated, nested, 15-step rapid chain, case-only, and offline/restart directory rename scenarios with **`0` missing, `0` stale results, `PRAGMA integrity_check = ok`** (`P50 = 26.72 ms`, `P95 = 45.67 ms`), and live watcher rename in `3.64 ms`.

### Core Engineering Strengths & Remaining Limitations

- **Verified Performance & Stability (`v0.5.1`):** `0.000%` controlled 2-minute idle CPU (`0 ms / 120.022 s`, `41.04 MB` WS), `5.89 s` 100k file indexing (`17,005 files/sec`, `P95 = 32.70 ms`), `P95 = 2.79 ms` full-text search across 1,000 docs (with live PDFium parsing), `Recall@5/10 = 1.0` & `MRR@10 = 1.0` semantic quality, `1,000` leak-free UI stress cycles, and `4.42 s` silent isolated NSIS `v0.5.1` install/uninstall.
- **Remaining P2/P3 & Unverified Gaps:** P2/P3 defects (`DEF-04` through `DEF-14`, including unsigned Release binaries triggering SmartScreen, `>260`-char NTFS traversal blind spot, multi-token folder+filename search, DOCX headers, linear `O(N)` vector scan above ~55k chunks, and `live_created_id_ed25519` prefix filter gap), physical multi-DPI/multi-monitor/Word-LO/clean-VM checks, and optional Git history rewrite for compromised `UshaLabs.pfx` remain open.

---

## 2. Project Architecture Review

### Actual Technology Stack

| Subsystem | Inspected Implementation (`v0.5.1`) |
|---|---|
| **Language & Build** | C++20 (`/std:c++20 /W4 /MP /utf-8 /permissive-`), MSVC `v143` toolset (`19.44.35229.0`), CMake + Ninja ([CMakeLists.txt](CMakeLists.txt)) |
| **UI & Desktop Shell** | Qt `6.8.2` (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`, shared DLL deployment via `windeployqt`) + Win32 integration in [src/platform/windows/DesktopIntegration.cpp](src/platform/windows/DesktopIntegration.cpp) |
| **Database & FTS** | Amalgamated SQLite `3.47.2` ([third_party/sqlite3/sqlite3.c](third_party/sqlite3/sqlite3.c)) with `SQLITE_ENABLE_FTS5=1`, `SQLITE_THREADSAFE=2`, `SQLITE_OMIT_LOAD_EXTENSION=1`; WAL mode, FTS5 `trigram` (`files_fts`) + `unicode61` (`document_fts`) |
| **PDF & Document Extraction** | Dynamic runtime `LoadLibraryW("pdfium.dll")` (`chromium/8152`, BSD-3-Clause/MIT) in [PdfiumBackend](src/extraction/PdfiumBackend.cpp) for modern PDFs (`/ObjStm`, `/ToUnicode` CMaps, Flate/LZW, page text); custom RFC 1950/1951 Inflate ([DeflateCodec](src/extraction/DeflateCodec.cpp)) + OOXML/CSV/tokenizer/media parsers ([DocumentExtractor](src/extraction/DocumentExtractor.cpp), [EnterpriseExtractors](src/extraction/EnterpriseExtractors.cpp)) |
| **Semantic AI** | Dynamic `LoadLibraryW("onnxruntime.dll")` (`1.20.20241119.10.5c1b7cc`, API `v20`), C++ WordPiece tokenizer ([WordPieceTokenizer](src/semantic/WordPieceTokenizer.cpp)), `all-MiniLM-L6-v2` INT8 quantized (`22,972,370 B`), linear SQLite BLOB cosine scan |
| **Filesystem Monitoring** | Initial traversal via Win32 `FindFirstFileExW`; real-time monitoring via overlapped Win32 `ReadDirectoryChangesW` with cross-buffer rename pairing, overflow rescan signaling, and targeted subtree reconciliation ([DirectoryWatcher](src/indexing/DirectoryWatcher.cpp), [FileIndexer](src/indexing/FileIndexer.cpp)) |
| **Packaging** | NSIS `3.12` per-user installer ([Lumen.nsi](packaging/Lumen.nsi), [build_setup.ps1](packaging/build_setup.ps1)), portable `.zip`, unsigned experimental MSIX script ([build_msix.ps1](packaging/build_msix.ps1)) |

### Main Subsystems & Feature Reality Check

| Feature / Claim ([README.md](README.md)) | Source Inspection & Runtime Verification (`v0.5.1`) | Status vs. Docs |
|---|---|---|
| **100% Offline, Zero Network Dependencies** | Zero `WS2_32.dll`, `WINHTTP.dll`, `WININET.dll`, or `Qt6Network.dll` imports in `Lumen.exe`, `onnxruntime.dll`, or `pdfium.dll`; zero HTTP client code in `src/` | Verified statically & via PE IAT |
| **Tray + Borderless Pink Icon, Never on Taskbar** | `WS_EX_TOOLWINDOW = 1`, `WS_EX_APPWINDOW = 0`, `HWND_BOTTOM` idle Z-order, `TaskbarCreated` recovery, tray shutdown cleanup | Verified via HWND styles & runtime |
| **Progressive Slide-Out (`56×56` → `488 px` → `908 px`)** | Smooth animation across all 6 states; results open with `hasSelection = 0` at `506 px` host (`488 px` card) and expand to `926 px` (`908 px` card) on selection | Verified at runtime |
| **Find Any File Regardless of Size** | 64-bit `qint64` size (`5 GiB` sparse `.iso` indexed cleanly); `>260`-char deep paths fail initial scan (`DEF-04`) | Verified (`>4 GiB` passes); `DEF-04` P2 remains |
| **5-Tier Folder Hierarchy** | Strict ordering Pinned → Desktop (`tier=0`) → Drive/watched root (`tier=2, depth=0`) → Depth `1..N` ascending | Verified |
| **PDF & Multi-Format Content Extraction** | Real Chromium/Edge PDFs (`/ObjStm` + CMap), Unicode/multi-column/table PDFs, TXT/MD, DOCX body, XLSX/XLSM/CSV/TSV, lexical code tokens, media metadata | **PDF fixed (`DEF-02`)**; `.doc`/`.xls`/`.pptx` & DOCX headers unsupported |
| **Content / Semantic Index Caps** | Hardcoded default caps: `maxContentIndexedFiles = 5000`, `maxSemanticFiles = 5000`, `maxDocumentSizeMb = 10` | Undocumented in README (`DEF-08` P2) |
| **Dark Theme Support** | `ThemeTokens::get(bool)` ignores argument (`Theme.cpp`); no Settings toggle | Stubbed only (`DEF-13` P3, not claimed in README) |

---

## 3. Test Environment

| Component | Measured / Verified Value |
|---|---|
| **Operating System** | Microsoft Windows 11 Pro (`Build 26200`, 64-bit / x64) |
| **Processor** | 11th Gen Intel(R) Core(TM) i7-1185G7 @ 3.00GHz (`4` physical cores, `8` logical processors) |
| **Physical RAM** | `15.69 GB` |
| **Primary Storage** | `WD PC SN5000S SDEPMSJ-512G-1101` (NVMe SSD, NTFS volume `C:`, `475.96 GB` total) |
| **Display Configuration** | `1` monitor, `1536×864` resolution (`1536×816` working area), `120 DPI` (`125%` scaling) |
| **Compiler & Build Tools** | MSVC `19.44.35229.0` (`cl.exe` x64, VS 2022 BuildTools `17.14.41`), `CMake 3.31.6-msvc6`, `Ninja 1.12.1` |
| **Runtime Libraries** | `Qt 6.8.2` (`msvc2022_64`), `SQLite 3.47.2` (`3047002`), `ONNX Runtime 1.20.20241119.10.5c1b7cc`, `PDFium chromium/8152` (`7,830,016 B`), `all-MiniLM-L6-v2` (`22,972,370 B` + `231,508 B` vocab), `NSIS v3.12` |

### Methodology & Environmental Limitations

- All tests ran in isolated temporary directories (`QTemporaryDir`, `AppPaths::setAppDataRootOverride()`) using clean out-of-tree Release builds. User files, personal index (`<LOCAL_APPDATA>\UshaLabs\Lumen`), and the running desktop tray instance remained untouched.
- **Unverified Hardware/Tool Environments:** `100%` / `150%` scaling, `1920×1080` / `1366×768` resolutions, multi-monitor / primary-monitor switches (`NOT TESTED`); MS Word & LibreOffice native PDFs (`BLOCKED` — neither installed); clean non-dev VM without Qt/VS installed (`NOT TESTED`); `10`/`30`-minute idle endurance (`NOT TESTED`); live packet-capture monitoring (`NOT TESTED`).

---

## 4. Build & Packaging Results

Reproducible clean out-of-tree Release build for `v0.5.1`:

```powershell
cmake -S <REPO_ROOT> -B <TEMP_QA_ROOT>\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/msvc2022_64
cmake --build <TEMP_QA_ROOT>\build --config Release
```

| Metric | v0.5.0 Baseline | v0.5.1 Verified Build |
|---|---|---|
| **CMake Config + Build** | `6.0 + 0.3 s`, `78.6 s` (`46` steps) | `7.0 + 0.4 s`, `103.56 s` (`47` steps, incl. PDFium deploy) |
| **Compiler Warnings / Errors** | `2` (`C4310` `DeflateCodec.cpp`), `0` errors | `2` (`C4310` `DeflateCodec.cpp`), `0` errors (`0` new warnings) |
| **Dev-Tools Opt-In Build (`LUMEN_BUILD_DEV_TOOLS=ON`)** | Mixed into `Lumen.exe` | Builds separate `capture_screenshots.exe` cleanly; excluded from `Lumen.exe` |
| **`Lumen.exe` Size & Version** | `2,102,272 B` (`0.5.0.0`) | `2,109,440 B` (`FileVersion 0.5.1.0`, `ProductVersion 0.5.1.0`) |
| **Benchmark Executables** | `phase1: 2,072,576 B` · `phase2: 2,064,896 B` · `phase3: 2,051,072 B` | `phase1: 2,095,104 B` · `phase2: 2,087,936 B` · `phase3: 2,074,624 B` |
| **Staged NSIS Deploy Size** | `24` files, `59.48 MB` | `26` files (adds `pdfium.dll` + `LICENSE.pdfium.txt`), `~67.0 MB` staged |
| **NSIS Installer / Portable ZIP (`v0.5.1`)** | `26.42 MB` Setup / `31.30 MB` ZIP | **`29.26 MB` Setup** / **`35.01 MB` Portable ZIP** (`build_setup.ps1`) |
| **PE Imports (`dumpbin /dependents Lumen.exe`)** | Zero network / debug DLLs | Identical: `Qt6{Widgets,Gui,Core}.dll`, Win32 system DLLs, VC140 CRT; zero network/debug DLLs |

---

## 5. GUI & Desktop Integration Results (`v0.5.1`)

| Verification Area | v0.5.1 Measured Result | Status |
|---|---|---|
| **Win32 Tray & Taskbar Exclusion** | `WS_EX_TOOLWINDOW = 1`, `WS_EX_APPWINDOW = 0`, `WS_EX_TOPMOST` toggles off/on; `TaskbarCreated` handled | **PASS** |
| **Startup Latency (`UI_STARTUP_SHOW_MS`)** | `29.65 ms` (v0.5.1 isolated UI init+show) | **PASS** |
| **Progressive 6-State Transitions** | State A `74×74` (`29.64 MB` WS) → State B `506×74` (`35.00 MB`) → State C `506×614` (`hasSelection=0`, `54.41 MB`) → State D `926×614` (`hasSelection=1`, `59.02 MB`) → State E `926→506` → State F `74×74` (`1.17 MB` after OS trim) | **PASS** |
| **`120` Transition Loops** | `0` failures (`42.50 MB` WS, `12.00 MB` Priv, `366` handles) | **PASS** |
| **Position Persistence & Hotkey** | `(140, 120)` restored (`slidesLeft=0`); `Ctrl+Alt+F9` registers (`reg=1`); contended `Ctrl+Alt+Space` fails gracefully | **PASS** |
| **Keyboard, Mouse Scroll, Context Menu, `CF_HDROP`** | `Down/Up`, `Tab/Shift+Tab` cycle all 5 modes, `Enter` activates (`activated=1`), `Ctrl+C` (`cf_hdrop=1`), `Ctrl+Shift+C`, `Ctrl+R` (`src='AuditSolarNotes.txt', count=2`), `Ctrl+P`, mouse scroll (`0→300`), `8`-item context menu. (`Ctrl+D` still clears selection on async refresh: `DEF-11` P3) | **PARTIAL** (`DEF-11`) |
| **Inspector PDF Page & Content Metadata** | `AuditPdfReport.pdf` via PDFium shows `matchedPage = 3` and extracted snippet cleanly | **PASS** |
| **DPI / Multi-Monitor / Themes** | `125%` (`1536×864`) verified; `100%`/`150%`/multi-mon `NOT TESTED`; Dark theme stubbed (`DEF-13`) | **PARTIAL** |

---

## 6. Filename & Folder Indexing Results (`v0.5.1`)

### Multi-Scale Physical NTFS Indexing & Query Latency (`qa_suite_indexing` v0.5.1)

| Target Scale | Enumerated / Upserted (`Dirs`) | Scan Time (`files/sec`) | Avg CPU | Peak WS | DB Size | Search P50 / P95 / P99 / Max (`200` queries) |
|---|---|---|---|---|---|---|
| **10k** | `9,807` (`108` dirs) | `449.23 ms` (`21,830.6/s`) | `10.43%` | `23.47 MB` | `6.38 MB` | `1.74` / `2.60` / `3.33` / `4.36 ms` |
| **50k** | `48,106` (`107` dirs) | `2,677.23 ms` (`17,968.6/s`) | `11.96%` | `25.10 MB` | `27.82 MB` | `14.63` / `24.10` / `39.97` / `63.83 ms` |
| **100k** | `96,106` (`107` dirs) | `6,213.43 ms` (`15,467.5/s`) | `11.44%` | `27.30 MB` | `51.34 MB` | `25.12` / **`32.04`** / `33.24` / `34.55 ms` |

### `DEF-03` Live Directory Rename & Watcher Latency Verification (`v0.5.1`)

- **Live Single-File/Folder Operations (`qa_suite_indexing` v0.5.1):** `new_file = 7.60 ms`, `rename_file = 2.19 ms`, `move_between_watched_roots = 3.82 ms`, `move_outside_watched = 1.60 ms`, `delete_file = 17.33 ms`, `new_folder_with_child = 10.17 ms`, **`rename_folder_checks_child_paths = 3.64 ms (ok=1, verified=1)`** *(was `5,010.93 ms` timeout in v0.5.0)*, `delete_folder_with_child = 1.87 ms`.
- **107-Trial Directory Rename Regression Suite (`qa_v051_verify`):**
  - `100/100` populated & multi-level nested folder renames (standard names, spaces + dots `v1.0 → v2.5`, UTF-8 `Café`): **all `100` verified zero missing files/folders, zero stale DB paths (`p50 = 26.72 ms, p95 = 45.67 ms`)**.
  - `15`-step rapid consecutive chain rename (`ChainStep_00 → ... → ChainStep_15`): **converged (`ok = 1`), zero deadlocks, zero stale paths, child FTS5 content preserved (`RapidChainUniqueContentMarker_9988`)**.
  - Case-only NTFS rename (`CaseMixDir → caseMIXdir`): **passed (`ok = 1`)**.
  - `5/5` offline/interrupted-indexing restart reconciliations (folders renamed + children mutated while `FileIndexer` stopped, followed by fresh `FileIndexer` restart scan): **`5/5` passed (`0` stale, `0` missing)**.
  - Final SQLite consistency after all `107` scenarios: **`PRAGMA integrity_check = ok`**.
- **Pre-Existing P2 Indexing Limitations (Unchanged by Choice):** `Quarterly Financial` multi-token folder+file miss (`DEF-05`), `>260`-char deep NTFS enumeration (`DEF-04`), and `live_created_id_ed25519` prefix match (`DEF-09`). Exact `.env`, `id_rsa`, `id_ed25519`, `*.key`, `*.pem`, `*.pfx` exclusions (`0` hits) and junction loops (`0` hits) pass.

---

## 7. Document Extraction Results (`v0.5.1`)

### `DEF-02` PDFium Verification + `34`-Case Format Suite

| Test Case | v0.5.0 Result | v0.5.1 Verified Result (`PDFium`) |
|---|---|---|
| **Edge Headless PDF (`3p`, 2-Column, Table, `Café`/`日本語`/`Ελληνικά`/`Кириллица`)** | `FAIL` (`0` chars) | **PASS** — `100,330 B`, `3` pages, `704` chars, `18.10 ms`, page attribution **`p1=1, p2=2, p3=3`**, table/col2/Unicode all `1` |
| **Edge Headless PDF (`25p`, Large Real Browser Print)** | Not tested | **PASS** — `108,916 B`, `25` pages, `4,862` chars, `23.55 ms`, **`page25_resolved = 25`** |
| **Edge Headless PDF (`qa_suite_extraction`, `2p`, `58,564 B`)** | `FAIL` (`0` chars) | **PASS** — `actualOk = 1, tokenFound = 1, pages = 2, chars = 344, ms = 18.43, resolvedPage = 2` |
| **End-to-End Indexed Edge PDF Content Search (`FileSearchEngine`)** | `FAIL` (`0` hits) | **PASS** — Content query `"PageThreeFinalAuditMarker"` returned hits with **`matchedPage = 3`** |
| **8-Thread Concurrent PDF Extraction (`8 × 10` docs)** | Not tested | **PASS** — **`80/80` succeeded (`fail = 0`)**, thread-safe mutex guard verified |
| **Synthetic Flate `3p`, Plain `1p`, Large `120p` PDFs** | Passed toy parser only | **PASS** — Extracted with ISO `xref` generator (`120p`: `83,878` chars / `70.08 ms`, `SYN_120P`: `4,210` chars) |
| **Password-Protected / Corrupt / Scanned PDFs** | Rejected cleanly | **PASS** — Graceful rejections (`"PDF is password-protected."`, `"Invalid or corrupted PDF document."`, `"No extractable text found in PDF."`) |
| **MS Word & LibreOffice Native PDFs** | `BLOCKED` | **`BLOCKED`** (Word / LibreOffice not installed on test host) |
| **Full `34`-Case Extraction Summary (`qa_suite_extraction`)** | `27` ok / `7` rej | **`28` succeeded / `6` rejected** (`P50 = 10.42 ms`, `P95 = 42.11 ms`); only `docx_header_check` (`DEF-06` P2) fails body-only DOCX expectation, plus `5` intentional security/corrupt/scanned rejections |

---

## 8. Semantic AI Results (`v0.5.1`)

Unchanged architecture verified clean against regressions (`qa_suite_semantic` v0.5.1):
- **Model Validation & Lifecycle:** `cold_load = 126.46 ms`, `384-D` finite normalized vectors; 5 repeated unload/reload cycles stable (`0.25–0.26 MB` WS unloaded / `43.11–44.46 MB` WS loaded).
- **Relevance (`160` Docs / `8` Domains / `24` Paraphrased Queries):** `251.66 chunks/sec`, **`Recall@5 = 1.0`, `Recall@10 = 1.0`, `Precision@5 = 1.0`, `MRR@10 = 1.0`**, **`P50 = 1.91 ms`, `P95 = 2.40 ms`**.
- **Vector Scan Scalability (`O(N)` Linear Scan):** `1k`: `P50 2.27 ms / P95 3.00 ms`; `10k`: `P50 23.69 ms / P95 25.85 ms`; `50k`: `P50 117.79 ms / P95 137.08 ms`; **`100k`: `P50 226.00 ms / P95 262.42 ms` (`Max 292.60 ms`, DB `218.85 MB`)** — still exceeds `< 150 ms` at `100k` (`DEF-07` P2).

---

## 9. Performance & Resource Benchmarks (`v0.5.1`)

| Metric | Target | v0.5.1 Measured Result | Status |
|---|---|---|---|
| **UI Startup to Visible Desktop Bar** | `< 500 ms` | **`29.65 ms`** (`qa_suite_remaining`) | **PASS** |
| **Idle CPU (2-Minute Controlled UI Idle)** | `< 0.1%` | **`0.000%`** (`0 ms` CPU time over `120.022 s`, `qa_db_ui_stress`) | **PASS** |
| **Idle Working Set RAM (2-Min Controlled)** | `< 120 MB` | **`41.04 MB` WS** (`12.60 MB` Priv, `342` Handles) | **PASS** |
| **Filename Indexing (`100,100` rows, Phase 1)** | Fast background | **`5.89 s`** (`17,005 files/sec`, `11.35%` CPU, Peak `24.78 MB`, DB `53.25 MB`) | **PASS** |
| **Filename Search Latency (`100k`, `200` queries)** | `< 50 ms` P95 | **Phase 1: `27.06 ms` P50 / `32.70 ms` P95** · **Physical NTFS: `25.12 ms` P50 / `32.04 ms` P95** | **PASS** |
| **Full-Text Search Latency (`1,000` Docs w/ PDFium)** | `< 150 ms` P95 | **`1.65 ms` P50 / `2.79 ms` P95 / `6.95 ms` Max** (`phase2_benchmark`) | **PASS** |
| **Semantic Search Latency (`250` – `10k` Chunks)** | `< 150 ms` P95 | `2.40 ms` (`160`), `4.09 ms` (`250`), `3.00 ms` (`1k`), `25.85 ms` (`10k`) | **PASS** |
| **Semantic Search Latency (`100k` Chunks)** | `< 150 ms` P95 | **`262.42 ms` P95** (`DEF-07` P2 linear scan limit) | **FAIL (`100k` P2)** |
| **Semantic Indexing Peak RAM (`250` Chunks)** | `< 350 MB` | **`61.07 MB` WS** (`0.25 MB` after `unloadModel()`) | **PASS** |
| **`1,000`-Cycle UI Stress Endurance** | Zero leaks/crashes | `0` crashes, `46.54 MB` WS, `14.61 MB` final Priv, `367` Handles (`+15`) | **PASS** |
| **`10` / `30`-Minute Idle Endurance** | `< 0.1%` | Not executed | **NOT TESTED** |

---

## 10. Reliability & Stress Testing (`v0.5.1`)

- **Progressive Transitions (`120` cycles):** `0` failures (`42.50 MB` WS, `366` handles).
- **GUI + Search + Mode Switch (`1,000` cycles):** Bounded plateau (`45.36 MB` at cycle 250 → `46.54 MB` at cycle 1000, final `367` handles vs baseline `352`).
- **Directory Rename Chaos (`107` trials incl. `15`-step chain + offline restart):** `107/107` passed, zero deadlocks (`m_incrementalMutex` / `m_writeMutex`), `0` stale rows.
- **Multi-Threaded PDFium (`80` concurrent extractions across `8` threads):** `80/80` passed with zero crashes or heap corruption.
- **Process Termination Recovery:** `4,500` committed rows recovered post-`TerminateProcess`, `PRAGMA integrity_check = ok`.

---

## 11. Database Integrity Results (`v0.5.1`)

- **Concurrency (`10k` inserts + `21` parallel read queries):** `integrity_pre = ok`, `after_clear = 0`, `integrity_post_clear = ok`.
- **Hard Crash Recovery:** `4,500` rows survived `TerminateProcess`, `integrity = ok`.
- **Schema Migration (`v1 → v4`):** Upgraded cleanly (`version = 4, has_is_dir = 1, rows_preserved = 1, integrity = ok`).
- **Rename Subtree + FTS5 Integrity:** Verified `107/107` directory renames updated `files` + `files_fts` + preserved `document_content`/`document_chunks` linkage (`PRAGMA integrity_check = ok`).
- **Low Disk Space Conditions:** `NOT TESTED`.

---

## 12. Security & Privacy Review (`v0.5.1`)

| Security / Privacy Control | v0.5.1 Verification | Result |
|---|---|---|
| **Release PE Binary Cleanliness (`DEF-01`)** | Verified `Lumen.exe`: `<USER_PROFILE> = False`, `--capture-screenshots = False`, `--measure-startup = False`, `clearAllFiles = False`, `<LEGACY_SIGNING_SECRET> = False`, zero telemetry/API strings | **PASS** |
| **Offline Execution & Import Audit** | Zero network DLLs on `Lumen.exe`, `onnxruntime.dll`, and `pdfium.dll` (`KERNEL32`/`ADVAPI32`/`GDI32`/`USER32` only) | **PASS** |
| **Working Tree Secrets Cleanup (`DEF-01`)** | Deleted `packaging/certs/UshaLabs.pfx` & `UshaLabs.cer`; added `.gitignore` exclusions; removed hardcoded password from `build_msix.ps1` and local path from `build_setup.ps1` | **PASS (HEAD)** — *Git history purge awaiting user approval* |
| **Junction Loop & ZIP Bomb Caps** | `mklink /J` junction `0` hits; `>16 MB` DOCX bomb rejected cleanly | **PASS** |
| **Credential Filenames** | Exact `.env`, `id_rsa`, `id_ed25519`, `*.key`, `*.pem`, `*.pfx` excluded (`0` hits); `live_created_id_ed25519` still indexes (`DEF-09` P2) | **PARTIAL (`DEF-09` P2)** |

---

## 13. Installer & Windows Distribution Review (`v0.5.1`)

1. **NSIS Standalone Installer (`Lumen-v0.5.1-Setup-x64.exe`):** Built cleanly via [packaging/build_setup.ps1](packaging/build_setup.ps1) (`29.26 MB` Setup, `35.01 MB` portable ZIP). Isolated `/S /ISOLATEDTEST` install completed in `4.42 s` (`exit = 0`, `29` files including **`pdfium.dll`**, `onnxruntime.dll`, `Qt6*.dll`, `models\all-MiniLM-L6-v2\*`, `8` VC143 CRT DLLs, `ver = 0.5.1`, `pub = UshaLabs`); isolated uninstall removed all app/shortcut/registry items cleanly (`remaining_non_uninst = 0`, `reg_exists = False`). Still Authenticode **`NotSigned`** unless external `$env:LUMEN_SIGNING_PFX` / `$env:LUMEN_SIGNING_PASSWORD` are supplied at release time (`DEF-10` P2).
2. **MSIX & Store Readiness (`build_msix.ps1`):** Externalized signing credentials cleanly (`-CertPfxPath`, `$env:LUMEN_SIGNING_PASSWORD`, `-SkipSigning`) and added `pdfium.dll` bundling; remains **not Store-ready** (`DEF-10`: omits MSVC CRT DLLs, requires `runFullTrust` + Store Publisher CN).

---

## 14. Defect Register (`v0.5.0` → `v0.5.1`)

| ID | Severity | Component | v0.5.1 Status & Verification Notes |
|---|---|---|---|
| **DEF-01** | **P1 — High** | [src/main.cpp](src/main.cpp), [packaging/build_setup.ps1](packaging/build_setup.ps1), [packaging/build_msix.ps1](packaging/build_msix.ps1), [.gitignore](.gitignore) | **RESOLVED in code/HEAD** (`Lumen.exe` verified zero debug/screenshot flags, zero local paths, zero `clearAllFiles()`; certs removed from HEAD; externalized signing params). **Action needed by repo owner:** Approve Git history rewrite/force-push to erase historical commit `05bc8da` and revoke exposed cert. |
| **DEF-02** | **P1 — High** | [src/extraction/PdfiumBackend.cpp](src/extraction/PdfiumBackend.cpp), [src/extraction/DocumentExtractor.cpp](src/extraction/DocumentExtractor.cpp) | **RESOLVED & VERIFIED** via native PDFium (`chromium/8152`). Edge 3p/25p multi-column/table/Unicode PDFs + ISO `xref` synthetic PDFs + 80 concurrent MT extractions + end-to-end FTS page `3` lookup all pass. |
| **DEF-03** | **P1 — High** | [src/indexing/DirectoryWatcher.cpp](src/indexing/DirectoryWatcher.cpp), [src/database/DatabaseManager.cpp](src/database/DatabaseManager.cpp), [src/indexing/FileIndexer.cpp](src/indexing/FileIndexer.cpp) | **RESOLVED & VERIFIED** across `107/107` populated, nested, 15-chain rapid, case-only, and offline-restart rename tests (`p50 = 26.72 ms`) plus `3.64 ms` watcher verification (`PRAGMA integrity_check = ok`). |
| **DEF-04** | **P2 — Medium** | [src/indexing/FileIndexer.cpp](src/indexing/FileIndexer.cpp) | **OPEN (Out of v0.5.1 scope)** — Initial traversal still misses `>260`-char NTFS deep paths. |
| **DEF-05** | **P2 — Medium** | [src/search/FileSearchEngine.cpp](src/search/FileSearchEngine.cpp) | **OPEN (Out of v0.5.1 scope)** — Multi-token folder+basename queries return `0` hits. |
| **DEF-06** | **P2 — Medium** | [src/extraction/DocumentExtractor.cpp](src/extraction/DocumentExtractor.cpp) | **OPEN (Out of v0.5.1 scope)** — DOCX `word/header*.xml` / `footer*.xml` ignored. |
| **DEF-07** | **P2 — Medium** | [src/search/FileSearchEngine.cpp](src/search/FileSearchEngine.cpp) | **OPEN (Out of v0.5.1 scope)** — `O(N)` brute-force vector scan exceeds `150 ms` at `100k` chunks (`262.42 ms` P95). |
| **DEF-08** | **P2 — Medium** | [src/semantic/SemanticEmbeddingEngine.cpp](src/semantic/SemanticEmbeddingEngine.cpp), [src/core/SettingsManager.cpp](src/core/SettingsManager.cpp) | **OPEN (Out of v0.5.1 scope)** — Undocumented `5,000`-file content/semantic caps; missing-model override falls back to bundled dir. |
| **DEF-09** | **P2 — Medium** | [src/indexing/DirectoryWatcher.cpp](src/indexing/DirectoryWatcher.cpp), [src/indexing/FileIndexer.cpp](src/indexing/FileIndexer.cpp) | **OPEN (Out of v0.5.1 scope)** — `live_created_id_ed25519` prefix gap leaks into live index. |
| **DEF-10** | **P2 — Medium** | [packaging/Lumen.nsi](packaging/Lumen.nsi), [packaging/build_msix.ps1](packaging/build_msix.ps1) | **PARTIALLY OPEN** — Installer functional but unsigned until external CA cert supplied; MSIX lacks CRT & Store identity. |
| **DEF-11** | **P3 — Low** | [src/ui/DesktopSearchBar/DesktopSearchBar.cpp](src/ui/DesktopSearchBar/DesktopSearchBar.cpp#L1387-L1397) | **OPEN (Out of v0.5.1 scope)** — `Ctrl+D` pin clears selection (`pre=1 → post=0`). |
| **DEF-12** | **P3 — Low** | [src/ui/SearchResults/SearchPopupWidget.cpp](src/ui/SearchResults/SearchPopupWidget.cpp#L1310-L1515) | **OPEN (Out of v0.5.1 scope)** — Inspector omits semantic similarity score badge. |
| **DEF-13** | **P3 — Low** | [src/ui/Theme.cpp](src/ui/Theme.cpp), [src/ui/Settings/SettingsDialog.cpp](src/ui/Settings/SettingsDialog.cpp) | **OPEN (Out of v0.5.1 scope)** — Dark theme unimplemented/stubbed. |
| **DEF-14** | **P3 — Low** | [src/extraction/DeflateCodec.cpp](src/extraction/DeflateCodec.cpp#L395-L396), [benchmarks/phase2_benchmark.cpp](benchmarks/phase2_benchmark.cpp) | **OPEN (Out of v0.5.1 scope)** — Two `C4310` warnings in `DeflateCodec.cpp`; Phase 2 warm-cache sequencing artifact. |

---

## 15. Acceptance Criteria Matrix (`v0.5.1`)

| Requirement | Test Performed | Expected Result | Actual Result | Status |
|---|---|---|---|---|
| **Clean MSVC Release Build (`0.5.1`)** | Out-of-tree Ninja Release build + opt-in dev tools build | `0` errors, clean DLL IAT, `0.5.1.0` version | `47/47` steps (`103.56 s`), `0` errors | **PASS** |
| **Security & Release Binary Hygiene (`DEF-01`)** | PE ASCII/UTF-16 audit, repo secret grep, `.gitignore` review | No test wipe flag, local paths, or repo secrets | `Lumen.exe` clean; certs purged from HEAD (history rewrite awaits user ok) | **PASS** *(History rewrite pending user ok)* |
| **System Tray, Taskbar Exclusion & Hotkey** | `GWL_EXSTYLE`, position persistence, `TaskbarCreated`, `Ctrl+Alt+F9` | Zero taskbar presence, styles & anchor persist | All Win32 checks pass | **PASS** |
| **Progressive 6-State UI & Transitions** | States A–F dimensions + `120` cycles + `1,000` stress cycles | `74px ↔ 506px ↔ 926px`, `0` crashes/leaks | `0` failures, stable handles (`352→367`) | **PASS** |
| **Keyboard, Mouse Scroll, Context Menu, `CF_HDROP`** | Shortcuts, wheel, `8`-action context menu, Win32 clipboard | Expected actions & Explorer `CF_HDROP` | Pass except `Ctrl+D` selection clear (`DEF-11`) | **PARTIAL** |
| **Multi-Scale Indexing (`10k–100k`)** | Physical NTFS + Phase 1 (`100,100` rows) + `200` queries | `< 50 ms` P95 search | Phase 1 `32.70 ms` P95; Physical `32.04 ms` P95 | **PASS** |
| **Live Folder Rename Indexing (`DEF-03`)** | `107` populated/nested/chain-15/case/offline-restart trials + watcher | `0` missing, `0` stale, integrity `ok` | **`107/107` PASS** (`p50 = 26.72 ms`), watcher `3.64 ms` | **PASS** |
| **Real-World PDF Extraction (`DEF-02`)** | Edge 2p/3p/25p PDFs + 80x MT + synthetic/enc/bad/scanned | Accurate page text & graceful failures | Edge `3p`/`25p`/`120p`/MT/index all pass; Word/LO unavailable | **PASS** *(Word/LO **BLOCKED**)* |
| **Non-PDF Extraction (`DOCX/XLSX/CSV/Code/Media`)** | Bounded 27 non-PDF test cases + ZIP bomb | Safe, accurate text & metadata | Body/sheets/code/media pass; DOCX headers missed (`DEF-06`) | **PARTIAL** |
| **Offline ONNX Validation & Relevance** | MiniLM INT8, `5` unload/reload cycles, `160` docs / `24` queries | `384-D` normalized, `0` leak, relevant hits | `Recall@10 = 1.0`, `MRR@10 = 1.0`, `0.25 MB` unloaded WS | **PASS** |
| **Semantic Scalability (`100k` Chunks)** | Brute-force cosine scan `1k..100k` | `< 150 ms` P95 | `25.85 ms` (`10k`), `137.08 ms` (`50k`), **`262.42 ms` (`100k`)** | **FAIL (`100k` P2)** |
| **Idle CPU (`<0.1%`) & RAM (`<120 MB`)** | `120 s` controlled UI idle | `< 0.1%` CPU, `< 120 MB` WS | **`0.000%` CPU**, **`41.04 MB` WS** (`10/30m` **NOT TESTED**) | **PASS** |
| **SQLite WAL Integrity, Concurrency & Crash Recovery** | `10k` concurrent writes+reads, `TerminateProcess`, `v1→v4` migration | `integrity_check = ok`, zero corruption | `4,500` rows survived crash, `ok` (`Low Disk` **NOT TESTED**) | **PASS** |
| **Display Scaling (`100%`/`150%`), Multi-Monitor & Themes** | `125%` (`1536×864`) tested; other displays unavailable | Full matrix validated | Only `125%` tested; Dark theme ignored | **NOT TESTED** / **PARTIAL** |
| **NSIS Packaging & Clean Install** | Built `v0.5.1` Setup + ZIP, silent isolated install/uninstall | Self-contained install/uninstall (`pdfium.dll` incl.) | `4.42 s` install (`29` files), `0` leftover files; binaries `NotSigned` | **PARTIAL** (`NotSigned` P2) |

---

## 16. Performance Regression Assessment (`v0.5.0` → `v0.5.1`)

| Metric | Prior README Baseline | v0.5.0 Pre-Fix | v0.5.1 Post-Fix Measured | Regression Verdict |
|---|---|---|---|---|
| **UI Startup (`UI_STARTUP_SHOW_MS`)** | `33.83 ms` avg | `23.06 ms` | `29.65 ms` | **No Regression** (`< 35 ms`, >16× under 500 ms target) |
| **Idle CPU (`2-Minute`)** | `0.0179%` | `0.000%` | **`0.000%`** (`120.022 s`) | **No Regression** |
| **Idle WS RAM (`2-Minute`)** | `30.44 MB` | `41.31 MB` | **`41.04 MB`** | **No Regression** |
| **100k Phase 1 Indexing** | `5.28 s` | `6.54 s` | **`5.89 s`** (`17,005/s`) | **Improved vs v0.5.0** |
| **100k Phase 1 Search P95** | `10.89 ms` | `35.05 ms` | **`32.70 ms`** (Physical NTFS `32.04 ms`) | **No Regression vs v0.5.0** |
| **Phase 2 PDF Extraction (`250 × 3p`)** | `—` | `8.98 ms/doc` (toy regex) | **`10.21 ms/doc` (real PDFium)** | **Acceptable** (+1.23 ms/doc for full ISO PDF spec compliance) |
| **Phase 2 FTS5 Query P50 / P95** | `4.70 / 8.70 ms` | `2.11 / 3.28 ms` | **`1.65 / 2.79 ms`** | **Improved** |
| **Phase 3 Semantic P50 / P95 (`250` chunks)** | `4.16 / 5.88 ms` | `3.74 / 4.91 ms` | **`3.26 / 4.09 ms`** | **Improved** |

---

## 17. Known Limitations

1. **No OCR Engine:** Scanned/image-only PDFs return `"No extractable text found in PDF."`.
2. **Legacy Binary Office & Presentations:** `.doc`, `.xls`, and `.pptx` document bodies are unsupported; DOCX extraction reads `word/document.xml` only (headers/footers ignored, `DEF-06`).
3. **Regex Lexical Code Indexing:** Source files use token extraction rather than compiler ASTs.
4. **Default 5,000-File Caps & Linear Vector Scan:** Content & semantic indexing halt at `5,000` eligible files / `10 MB` max doc size by default; semantic queries exceed `150 ms` P95 around `>55k` vectors (`DEF-07`).
5. **Unverified Environments:** Multi-DPI (`100%`, `150%`), `1920×1080` / `1366×768`, multi-monitor, MS Word / LibreOffice PDFs, `10`/`30`-min idle endurance, clean non-dev VM, and packet-capture trace.
6. **Git History Secret Exposure:** Commit `05bc8da` still contains legacy `UshaLabs.pfx` in reachable Git history until owner approves a force-push history rewrite.

---

## 18. Release Readiness Verdict

### Verdict: **`CONDITIONAL PASS — RELEASE CANDIDATE`**

All three **P1 release blockers (`DEF-01`, `DEF-02`, `DEF-03`)** have been resolved and verified on clean Windows x64 Release builds with zero performance regressions:
- `Lumen.exe` (`0.5.1.0`) is clean of developer flags/paths and `UshaLabs.pfx` + plaintext passwords are removed from `main` HEAD (pending optional history rewrite approval).
- Native PDFium (`chromium/8152`) reliably extracts modern Chromium/Edge multi-page, multi-column, table, and Unicode PDFs with accurate page offsets.
- Live and offline populated/nested folder renames passed `107/107` automated tests (`0` stale/missing entries, `PRAGMA integrity_check = ok`).

Remaining `P2`/`P3` items (`DEF-04`–`DEF-14`, including external Authenticode signing before public distribution) and unverified hardware/Office matrices justify Release Candidate status prior to broad public launch.

---

## 19. Prioritized Remediation Plan (Post-v0.5.1 RC)

1. **[Security — Owner Approval Needed]** Execute `git filter-repo` (or BFG) + coordinated force-push on `origin/main` to purge `packaging/certs/UshaLabs.pfx` and commit `05bc8da` password history; revoke the exposed certificate permanently.
2. **[Release Engineering — Public Distribution]** Sign `Lumen.exe` and `Lumen-v0.5.1-Setup-x64.exe` via an EV/OV trusted Authenticode certificate injected cleanly through `$env:LUMEN_SIGNING_PFX` / `$env:LUMEN_SIGNING_PASSWORD` (`DEF-10`).
3. **[P2 Stabilization Batch]** Fix extended `\\?\` `FindFirstFileExW` traversal (`DEF-04`), multi-token folder+basename queries (`DEF-05`), DOCX headers/footers (`DEF-06`), credential glob patterns `id_ed25519*` / `id_rsa*` (`DEF-09`), strict missing-model error mode + documented index caps (`DEF-08`), and scalable ANN vectors if raising `5,000`-file semantic cap (`DEF-07`).
4. **[Validation Expansion]** Run multi-DPI (`100%`/`150%`), multi-monitor, native MS Word / LibreOffice PDF, clean-VM (no Qt/VS), and MSIX Store WACK checks (`DEF-10`–`DEF-14`).

---

## 20. Final Project Assessment

- **Current Application Maturity:** **Release Candidate (`v0.5.1`).** Resolving `DEF-01`–`DEF-03` closes the primary security hygiene, real-world PDF compatibility, and filesystem rename synchronization gaps identified in `v0.5.0`.
- **Engineering Strengths:** Tiny static/runtime footprint, `0.000%` idle CPU, `41 MB` idle WS, fast physical NTFS indexing (`5.89 s` / `100k`), accurate PDFium text extraction (`18–24 ms` real browser PDFs, thread-safe), deterministic SQLite WAL + FTS5 subtree rename handling, and strong offline ONNX relevance (`Recall@10 = 1.0`, `0.25 MB` unload reclaim).
- **Architecture Weaknesses & Scalability:** Linear `O(N)` vector scan above `~55k` chunks, hardcoded `5,000`-file content/semantic defaults, non-AST code indexing, `>260`-char initial crawl gap.
- **Performance & Maintainability:** All latency and resource budgets hold cleanly across UI (`29.65 ms` startup, `1,000` stress cycles), filename (`32.04–32.70 ms` P95), FTS (`2.79 ms` P95), and interactive semantic (`<= 25.85 ms` P95 at `10k`) workloads. Codebase modularity improved further by isolating [PdfiumBackend](src/extraction/PdfiumBackend.cpp) and [tools/capture_screenshots.cpp](tools/capture_screenshots.cpp).
- **Security Posture & Distribution:** Runtime network isolation (`0` net DLLs) and working-tree secrets cleanliness are solid; final pre-GA gates are owner-approved history cleanup for the historical `UshaLabs.pfx` commit and trusted Authenticode code signing for NSIS/portable deliverables.
