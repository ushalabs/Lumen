# LUMEN — FINAL PRODUCTION ACCEPTANCE TEST

**Project:** Lumen — Offline Intelligent Search for Windows  
**Developer:** UshaLabs  
**Repository:** https://github.com/ushalabs/Lumen  
**Target Platform:** Windows 10 / Windows 11 (x64)  
**Application Version:** `0.5.1` (`FILEVERSION 0,5,1,0`)  
**Git HEAD at Whole-PC Validation:** Branch `WholePC/Search` (branched from `main` @ `2e8815dd6688557463021841c647a530abd2e1c8`) — 2-Tier Whole-PC Indexing, IOCP Watcher, & Schema `v6` (`folders_fts`)  
**Validation Windows:** `2026-11-11 00:02–00:56 +05:00` (`v0.5.1` P1 release suite) & `2026-11-11 16:01–17:18 +05:00` (2-Tier Whole-PC + 1M-Row Scale + IOCP + Schema `v6` + Clean Build & NSIS Repackage)  
**Release Readiness Verdict:** **`CONDITIONAL PASS — RELEASE CANDIDATE`** *(All P1 blockers `DEF-01`–`DEF-03`, startup/tray/Z-order hardening, 2-Tier Whole-PC + Scoped AI + 1M-row `< 50 ms` P95 verification, clean `/W4` Release build, and local NSIS Setup + Portable ZIP repackaging/install checks pass cleanly; external Authenticode signing, optional Git history purge for historical commit `05bc8da`, and documented P2/P3 items remain.)*

---

## 1. Executive Summary

- **Overall Acceptance Verdict:** **`CONDITIONAL PASS — RELEASE CANDIDATE`**
- **Application Version:** `0.5.1` ([CMakeLists.txt](CMakeLists.txt), [assets/app.rc](assets/app.rc), [assets/lumen.manifest](assets/lumen.manifest))
- **Verified Revision:** `2e8815dd6688557463021841c647a530abd2e1c8` (`main` baseline) → branch `WholePC/Search` (**2-Tier Whole-PC Indexing, `schemaVersion = 3` settings, SQLite Schema `v6` `folders_fts`, Win32 IOCP `DirectoryWatcher`, partitioned memory-bounded reconciliation, `FileSearchEngine` split/composite index optimization, and `FileIndexer::runScanAndReconcile` cancellation-reset fix**).
- **Test Environment:** Windows 11 Pro (`10.0.26200`, Build `26200`, 64-bit), 11th Gen Intel(R) Core(TM) i7-1185G7 @ 3.00GHz (`4C/8T`), `15.69 GB` RAM, `WD PC SN5000S SDEPMSJ-512G-1101` NVMe SSD (NTFS, `150.2 GB` free / `476.0 GB`), `1536×864 @ 96 DPI (100%)` display, MSVC `19.44.35229` (`x64`), CMake `3.31.6-msvc6`, Ninja `1.12.1`, Qt `6.8.2`, SQLite `3.47.2`, ONNX Runtime `1.20.20241119.10.5c1b7cc`, PDFium (`chromium/8152`, BSD-3-Clause), Microsoft Edge `157.0.4341.46`.

### Major Capabilities & Fixes Verified

1. **`DEF-01` (Security & Binary Hygiene — RESOLVED IN WORKING TREE/HEAD):** Removed `--capture-screenshots`, `--measure-startup`, `clearAllFiles()` CLI invocation, and hardcoded developer paths from production [src/main.cpp](src/main.cpp) (isolated into opt-in dev utility [tools/capture_screenshots.cpp](tools/capture_screenshots.cpp), `LUMEN_BUILD_DEV_TOOLS=OFF`). Post-final-edit `Lumen.exe` (`2,196,992 B`, `v0.5.1.0`) PE audit confirmed zero developer path literals (`<USER_PROFILE>`), zero `--capture-screenshots`/`UshaLabs.pfx` strings, and zero network/debug DLL imports. *(Historical commit `05bc8da` `.pfx` purge in Git history awaits explicit owner authorization.)*
2. **`DEF-02` (Real-World PDF Extraction — RESOLVED & RE-VERIFIED):** Native Chromium **PDFium** ([PdfiumBackend.h](src/extraction/PdfiumBackend.h), [PdfiumBackend.cpp](src/extraction/PdfiumBackend.cpp)) verified post-Whole-PC edits on both live headless Microsoft Edge `157.0.4341.46` 2-page PDF (`pages=2, chars=229, extract_ms=281.26 ms`, Page 2 `matchedPage=2`) and `250 × 3p` multi-page compressed PDFs (`5.20 ms/doc`, `P95 = 14.41 ms` FTS5).
3. **`DEF-03` (Live & Offline Directory Rename Indexing — RESOLVED & RE-VERIFIED WITH IOCP + `v6` `folders_fts`):** Verified **150 automated directory renames** (`100` rapid live populated/nested renames + `30` rapid multi-hop chain renames settling in `1,085 ms` via Win32 IOCP + `20` offline restart renames via `FileIndexer::runScanAndReconcile`), confirming **`0` missing, `0` stale results** across SQLite `files`, `files_fts`, and `folders_fts`. Uncovered and fixed a synchronous reuse bug where calling `FileIndexer::runScanAndReconcile()` after `FileIndexer::stop()` aborted immediately on stale `m_cancelRequested`.
4. **2-Tier Whole-PC Indexing Architecture (NEW — IMPLEMENTED & VERIFIED):**
   - **Tier 1 (Files & Folders — Whole PC):** [IndexingPolicy](src/indexing/IndexingPolicy.cpp) scans all fixed internal drives (`DRIVE_FIXED`: `C:\`, `D:\`, etc.) and Start Menu folders including `C:\Program Files`, `C:\Program Files (x86)`, `C:\Qt`, `C:\xampp` while skipping OS/dev/cache/credential junk (`C:\Windows`, `C:\ProgramData` [except Start Menu], `AppData`, hidden dot-dirs like `.git`/`.gradle`/`.vscode`/`.cache`, `node_modules`, `build`, `Temp`, `Cache`/`ShaderCache`/`GPUCache`, `pagefile.sys`/`hiberfil.sys`/`swapfile.sys`, `NTUSER.DAT*`, credentials). Verified on live host: **`151,303` items (`17,417` dirs) crawled in `37.11 s`**, **`25.27 MB` peak WS**, **`73.30 MB` DB**, and **`steam` resolves `C:\Program Files (x86)\Steam` (`0.34 ms`) + `steam.exe` (`1.54 ms`)**.
   - **Tier 2 (Content & Semantic AI — Strictly Scoped):** Content FTS (`document_content` / `document_fts`) and MiniLM-L6-v2 embeddings (`document_chunks`) are restricted strictly to user `watchedFolders` (`Documents`, `Downloads`, `Desktop`, `Pictures`, `Videos`, `Music` + user additions in [SettingsDialog](src/ui/Settings/SettingsDialog.cpp)). Verified zero out-of-scope AI rows under `C:\Program Files`, `C:\Qt`, `C:\xampp`, and simulated third-party PDFs, plus verified automatic pruning via [`DatabaseManager::pruneContentAndSemanticOutsideRoots`](src/database/DatabaseManager.cpp).
5. **Win32 IOCP Multi-Target Live Watcher + Partitioned Memory-Bounded Reconciliation (NEW — VERIFIED):** Replaced `WaitForMultipleObjects` (`63`-handle cap) with `CreateIoCompletionPort` / `GetQueuedCompletionStatus` ([DirectoryWatcher](src/indexing/DirectoryWatcher.cpp)) using shallow (`watchSubtree=false`) root watches + dynamic subtree auto-attach + deep watches, achieving **`0.00649%` CPU and `4.75 MB` WS (`30.19 MB` Priv) over a 30s live whole-PC IOCP idle window**, plus **`275.04 ms` offline partitioned subtree cleanup** on a `40,080`-item physical dataset.
6. **Million-Row Query Scalability — SQLite Schema `v5` + `v6` (`folders_fts`) (NEW — VERIFIED):** Composite `(is_dir, name COLLATE NOCASE)` index (`v5`), dedicated trigger-maintained `folders_fts` trigram virtual table (`v6`), split `runExactPass`, and removing a `folders_fts` subquery-flattening `ORDER BY LENGTH(path)` scan cut 1,000,000-row search latency from `>1.1 s` (pre-v5) and `241.5 ms` P95 (pre-flatten fix) down to **`P50 = 16.44 ms`, `P95 = 25.12 ms`, `Max = 31.22 ms` (`WS = 29.38 MB`, `DB = 364.00 MB`)**.

### Diagnosis of Prior 3-Hour QA Hang (`task-3146`) & Resolution

Earlier during development testing (`task-3146`, stopped after >3 hours at `12:47:18` inside `TEST B`), the QA executable hung indefinitely right after `TEST B`'s live query printout due to two issues fixed in the current codebase:
1. **Shutdown Deadlock (`DirectoryWatcher::stopWatching()`):** Cancelling overlapped IOCP `ReadDirectoryChangesW` handles previously called blocking `GetOverlappedResult(..., TRUE)` when `OVERLAPPED::hEvent == nullptr`; because Win32 IOCP handles deliver completions to the completion port rather than signaling an `hEvent`, that call blocked permanently. Fixed with `CancelIoEx` + `CloseHandle` + a bounded `300 ms` `GetQueuedCompletionStatus` drain loop ([src/indexing/DirectoryWatcher.cpp](src/indexing/DirectoryWatcher.cpp)).
2. **Pre-Optimization Full-Scan Latency (`folders_ms = 101.61 ms`, `files_ms = 226.79 ms` in `task-3146.log`):** Resolved via split exact queries (`idx_files_name`), `v5` composite `idx_files_dir_name`, and `v6` `folders_fts` (`folders_ms` dropped `299×` to **`0.34 ms`**, `files_ms` dropped `147×` to **`1.54 ms`** on `151,303` live items). Post-fix QA suites finish completely without hanging (`qa_wholepc_scale`: full cold scan + 250 AI docs + 30s idle + 100k–1M stress + 40k physical tests completed cleanly with `exit=0`; `qa_v052_full_checks`: completed cleanly with `exit=0`).

---

## 2. Project Architecture Review

### Actual Technology Stack

| Subsystem | Inspected Implementation (Current Working Tree) |
|---|---|
| **Language & Build** | C++20 (`/std:c++20 /W4 /MP /utf-8 /permissive-`), MSVC `v143` toolset (`19.44.35229`), CMake `3.31.6-msvc6` + Ninja ([CMakeLists.txt](CMakeLists.txt)) |
| **UI & Desktop Shell** | Qt `6.8.2` (`Qt6Core`, `Qt6Gui`, `Qt6Widgets`) + Win32 logon Task Scheduler (`\UshaLabs\Lumen`), `StartupApproved\Run`, tray recovery (`TaskbarCreated`), and Progman/WorkerW desktop Z-order ([DesktopIntegration.cpp](src/platform/windows/DesktopIntegration.cpp), [SettingsDialog.cpp](src/ui/Settings/SettingsDialog.cpp)) |
| **Database & FTS** | Amalgamated SQLite `3.47.2` ([sqlite3.c](third_party/sqlite3/sqlite3.c)) with `SQLITE_ENABLE_FTS5=1`, WAL mode (`cache_size = -4096`), **Schema `v6`**: `files`, `idx_files_name`, `idx_files_dir_name`, `files_fts` (trigram), **`folders_fts` (trigram, trigger-maintained on `is_dir = 1`)**, `document_content`, `document_fts` (`unicode61`), `document_chunks` |
| **PDF & Document Extraction** | Dynamic runtime `LoadLibraryW("pdfium.dll")` (`chromium/8152`, BSD-3-Clause) in [PdfiumBackend](src/extraction/PdfiumBackend.cpp); custom RFC 1950/1951 Inflate ([DeflateCodec](src/extraction/DeflateCodec.cpp)) + OOXML/CSV/code/media parsers ([DocumentExtractor](src/extraction/DocumentExtractor.cpp), [EnterpriseExtractors](src/extraction/EnterpriseExtractors.cpp)) |
| **Semantic AI** | Dynamic `LoadLibraryW("onnxruntime.dll")` (`1.20.20241119.10.5c1b7cc`), C++ WordPiece tokenizer ([WordPieceTokenizer](src/semantic/WordPieceTokenizer.cpp)), `all-MiniLM-L6-v2` INT8 quantized (`384-D`), scoped exclusively to `watchedFolders` |
| **Whole-PC Crawl & Live IOCP** | Partitioned memory-bounded NTFS crawl ([FileIndexer::scanSubtreeBounded](src/indexing/FileIndexer.cpp)), centralized policy ([IndexingPolicy](src/indexing/IndexingPolicy.cpp)), Win32 IOCP multi-target watcher (`CreateIoCompletionPort`, `128 KB` buffers, dynamic root-child attach, `15 s` overflow cooldown) ([DirectoryWatcher](src/indexing/DirectoryWatcher.cpp)) |
| **Settings & Migration** | `SettingsManager` JSON **Schema `v3`**: adds `indexing.indexWholePc = true` and automatically migrates legacy `v2` ignore blacklists so `"Windows"`, `"Program Files"`, `"Program Files (x86)"` are handled accurately by path policy ([SettingsManager.cpp](src/core/SettingsManager.cpp)) |

### Feature Reality Check

| Feature / Requirement | Verified Implementation Status | Status |
|---|---|---|
| **Whole-PC File & Folder Search (`C:\`, `D:\`, Program Files, Start Menu)** | Crawls all `DRIVE_FIXED` drives + Start Menu; excludes OS/dev/cache/swap/hive/credential junk; live `steam` query resolves `C:\Program Files (x86)\Steam` + `steam.exe` | **PASS** |
| **AI Strictly Scoped to User Library Folders** | Content FTS & MiniLM vectors index only `watchedFolders`; verified 0 chunks/content rows from `Program Files`, `Qt`, `xampp`, and external vendor files; auto-prunes removed roots | **PASS** |
| **Auto-Discover New Folders & Apps Live Anywhere on PC** | IOCP shallow watches on drive roots / user homes emit `directorySubtreeAdded` -> reconcile new folder + attach deep watch dynamically (`NewSteamInstall\steam_new.exe` verified) | **PASS** |
| **100% Offline, Zero Network Dependencies** | `dumpbin /imports` on `Lumen.exe` confirms zero `WS2_32.dll`, `WINHTTP.dll`, `WININET.dll`, or `Qt6Network.dll` imports | **PASS** |
| **Logon Startup, Persistent Tray & Desktop Z-Order** | Default `startWithWindows = true`, Windows Task Scheduler `\UshaLabs\Lumen`, close-to-tray Alt+F4 guard, `WS_EX_TOOLWINDOW`, `TaskbarCreated` recovery | **PASS** |
| **Settings UI Whole-PC Toggle & Live Stats** | [SettingsDialog](src/ui/Settings/SettingsDialog.cpp) checkbox (`Index files & folders across all PC drives`), `"Content & Semantic AI folders"` caption, live DB counts verified | **PASS** |
| **Clean Build, NSIS Setup & Portable ZIP Packages** | Clean Ninja Release build (`48/48`) repackaged via [build_setup.ps1](packaging/build_setup.ps1) (`29.29 MB` Setup + `35.05 MB` ZIP); isolated `/ISOLATEDTEST /S` install (`29` files) & uninstall (`0` leftovers) passed | **PASS** *(Unsigned `DEF-10`)* |

---

## 3. Test Environment

| Component | Measured Value (`2026-11-11 16:14 +05:00`) |
|---|---|
| **Operating System** | Microsoft Windows 11 Pro (`Version 10.0.26200`, Build `26200`, x64) |
| **Processor** | 11th Gen Intel(R) Core(TM) i7-1185G7 @ 3.00GHz (`4` cores / `8` logical processors) |
| **Physical RAM** | `15.69 GB` |
| **Storage & Volume `C:`** | `WD PC SN5000S SDEPMSJ-512G-1101` (NVMe SSD), NTFS, `150.2 GB` free / `476.0 GB` total |
| **Display Configuration** | `1536×864 @ 96 DPI (100%)` |
| **Compiler & Build Tools** | MSVC `19.44.35229` (`x64`), `CMake 3.31.6-msvc6`, Ninja |
| **Qt / Database / AI / PDF** | Qt `6.8.2` (`msvc2022_64`), SQLite `3.47.2` (amalgamated, FTS5 enabled), ONNX Runtime `1.20.20241119.10.5c1b7cc`, PDFium (`chromium/8152`), `msedge.exe 157.0.4341.46` |

All QA suites ran inside isolated temporary directories (`QTemporaryDir`, `AppPaths::setAppDataRootOverride()`) leaving personal `%LOCALAPPDATA%\UshaLabs\Lumen`, user files, and Task Scheduler (`\UshaLabs\Lumen`) completely untouched (the Settings UI smoke check avoided invoking OS Task Scheduler writes).

### Unverified / Not Re-Run Environments

- Physical `125%` / `150%` multi-monitor switching, MS Word / LibreOffice PDFs (`BLOCKED` — not installed), clean non-dev Windows VM without VS/Qt, and `10`/`30`-minute endurance runs remain `NOT TESTED`.
- From the earlier `v0.5.1` baseline (`2026-11-11 00:56 +05:00`): the `1,000`-cycle interactive DesktopSearchBar animation loop, `120 s` GUI idle window, `34`-case extended extraction matrix, and `100k`-chunk brute-force semantic scan were **not re-executed** after the Whole-PC changes (repackaged NSIS isolated install/uninstall was re-verified on `2026-11-11 17:18 +05:00`).

---

## 4. Build & Binary Hygiene Results

Release build invoked via clean out-of-tree VS 2022 BuildTools `vcvars64.bat` + CMake/Ninja (`-DLUMEN_BUILD_DEV_TOOLS=OFF`, `--config Release`, `/std:c++20 /W4 /MP /utf-8 /permissive-`):

| Artifact / Check | Measured Result (`task-3387` / `task-3391` / Clean Rebuild `task-3426` & Packaging `task-3446`) |
|---|---|
| **Clean Release Compilation** | **PASS** — `48/48` steps, `0` errors, `2` pre-existing `C4310` warnings in `DeflateCodec.cpp` (`0` new warnings across all Whole-PC/`v6` files) |
| **NSIS Setup & Portable ZIP (`build_setup.ps1`)** | **PASS** — `Lumen-v0.5.1-Setup-x64.exe` (`30,711,599 B` / `29.29 MB`), `Lumen-v0.5.1-windows-x64.zip` (`36,749,574 B` / `35.05 MB`), `SHA256SUMS.txt` generated locally (`dist/` & `releases/`); isolated `/ISOLATEDTEST /S` install (`16,271.09 ms`, `29` files incl. `Lumen.exe`, `pdfium.dll`, `onnxruntime.dll`, `model_quantized.onnx`) & uninstall (`0` leftovers, `exit=0`) passed |
| **`Lumen.exe` Binary Metadata** | `2,196,992` bytes (`2.10 MB`), `FileVersion 0.5.1.0`, `ProductVersion 0.5.1.0` |
| **Leaked String Audit (`Lumen.exe`)** | `Contains '--capture-screenshots': False`, `Contains 'UshaLabs.pfx': False`, `Contains '<USER_PROFILE>': False` |
| **PE Import Table (`dumpbin /imports`)** | `Qt6Widgets.dll`, `Qt6Gui.dll`, `Qt6Core.dll`, Win32 (`USER32`, `SHELL32`, `ole32`, `OLEAUT32`, `ADVAPI32`, `KERNEL32`, `GDI32`), MSVC CRT (`MSVCP140`, `VCRUNTIME140*`, `api-ms-win-crt-*`); **zero network or debug CRT DLLs** |

---

## 5. Desktop Shell, Startup & UI Integration Results

| Verification Check | Executed Suite | Measured Outcome | Status |
|---|---|---|---|
| **Startup Default & JSON Precedence** | `verify_startup` | Missing `settings.json` defaults `startWithWindows = true`; explicit `false`/`true` overrides load cleanly | **PASS** |
| **Win32 Task Scheduler Logon CRUD** | `verify_startup` | Isolated test task `Lumen_TestVerify_Tmp` registered w/ `--startup`, queried back cleanly, removed cleanly | **PASS** |
| **Taskbar Exclusion, Close Guard & Rest Z-Order** | `verify_startup` | `WS_EX_TOOLWINDOW` set (`!= 0`), `WS_EX_APPWINDOW` unset (`== 0`), close without exit guard leaves bar visible, `desktopRestInsertAfter()` returns valid anchor `!= HWND_BOTTOM` | **PASS** |
| **Tray Recovery (`TaskbarCreated`)** | `verify_startup` | `TrayController` initialized cleanly and survived simulated Explorer restart (`recreateTrayAfterExplorerRestart()`) | **PASS** |
| **Settings Whole-PC Toggle & Persistence** | `qa_v052_full_checks` (`Test 4`) | `SettingsDialog` offscreen bind confirmed `"all PC drives"` checkbox tracks `indexWholePc` (`true -> false -> true`), caption `"Content & Semantic AI folders"` and `"Save Changes"` present, JSON round-trip persisted | **PASS** |
| **Interactive 6-State `74↔506↔926 px` & `1,000` Cycles** | `v0.5.1` (`00:56`) baseline | `0` failures, `46.54 MB` final WS, `367` handles *(not re-run post-Whole-PC edits)* | **CARRIED (`v0.5.1`)** |

---

## 6. Whole-PC Crawling, Live IOCP & Multi-Scale Search Results

### Real Physical PC Whole-PC Crawl + Scoped AI + 30s Live IOCP Idle (`qa_wholepc_scale` `Test B`)

| Metric | Measured Result (`task-3391`, Post-Final-Edit) |
|---|---|
| **Scan Roots Discovered** | `3` (`C:\`, `C:\ProgramData\Microsoft\Windows\Start Menu`, `<USER_PROFILE>\AppData\Roaming\Microsoft\Windows\Start Menu`) |
| **Cold Whole-PC Crawl** | **`151,303` items** (`17,417` directories) in **`37,108.3 ms`** (`4,077.33 items/sec`), **`11.78%` avg CPU**, Peak WS **`25.27 MB`**, Private **`8.57 MB`**, DB **`73.30 MB`** |
| **Exclusion Enforcement** | `C:\Windows\* = 0`, `*\AppData\Local\Temp\* = 0`, `<USER_PROFILE>\.gradle\* = 0`, `pagefile.sys`/`hiberfil.sys = 0` |
| **Real App Lookup (`"steam"`)** | `All`: `20` hits in **`4.59 ms`** · `Folders`: **`0.34 ms`** (`C:\Program Files (x86)\Steam` found) · `Files`: **`1.54 ms`** (`steam.exe` found) |
| **Warm Partitioned Rescan + Scoped `250`-Doc Content/Semantic** | Total **`38,422 ms`** (`content_docs=250` in `3,784.35 ms`; `sem_docs=250`, `523` chunks in `27,549.6 ms`), Cur WS **`2.45 MB`**, Peak WS **`101.98 MB`**, Private **`28.74 MB`** |
| **AI Scoping Verification** | Out-of-scope `document_content` and `document_chunks` rows under `C:\Program Files*`, `C:\Qt\*`, `C:\xampp\*` = **`0`** |
| **`30 s` Live Whole-PC IOCP Idle** | CPU **`0.00649%`**, Working Set **`4.75 MB`**, Private RAM **`30.19 MB`** |

### Multi-Scale Filename & Folder Latency (`100k` to `1,000,000` Rows — `qa_wholepc_scale` `Test C`)

Seeded with `20,000` noisy `steam_api_asset_*.dll` rows before exact `Steam`, `steam.exe`, and `Steam.lnk` targets plus balanced files (`95%`) and folders (`5%` — `5,000` folders per term at 1M rows) across 10 query terms (`200` queries rotating `All`, `Files`, `Folders` at each scale):

| Dataset Scale | Cumulative Insert | DB Size | Process WS | `"steam"` All + Top-3 Verified | `"invoice"` (`All / Files / Folders`) | `200`-Query `P50 / P95 / P99 / Max` |
|---|---|---|---|---|---|---|
| **100,000 rows** | `8.35 s` | `40.10 MB` | `28.38 MB` | `9.55 ms` (PASS) | `6.33 / 1.15 / 2.21 ms` | `6.04 / 10.54 / 12.23 / 16.65 ms` |
| **250,000 rows** | `25.58 s` | `94.12 MB` | `28.73 MB` | `16.50 ms` (PASS) | `19.75 / 4.09 / 19.42 ms` | `14.72 / 21.67 / 25.90 / 26.99 ms` |
| **500,000 rows** | `54.27 s` | `183.85 MB` | `29.36 MB` | `21.19 ms` (PASS) | `24.32 / 3.64 / 19.32 ms` | `16.84 / 27.34 / 33.30 / 34.14 ms` |
| **1,000,000 rows** | `109.79 s` | `364.00 MB` | `29.38 MB` | `21.16 ms` (PASS) | `21.44 / 3.53 / 23.17 ms` | **`16.44 / 25.12 / 27.39 / 31.22 ms`** |

- **Key Optimization Finding:** Separating `folders_fts` (`v6`) initially hit a SQLite subquery-flattening trap from wrapping `LIMIT` inside `ORDER BY LENGTH(path) ASC`, causing `1M` P95 to hit `241.47 ms` (`steam_ms = 256.32 ms`). Removing that redundant outer `ORDER BY` in `runFtsPass` (since C++ `folderHierarchyCmp` sorts `candidateMap`) reduced `1,000,000`-row latency to **`25.12 ms` P95 (`16.44 ms` P50, `31.22 ms` Max)** at **`29.38 MB` Working Set**.

### Partitioned Physical Reconciliation, Shallow Watch & Rename Regression (`Test D` & `qa_v052_full_checks` `Test 2`)

- **40k Partitioned Physical Delete (`qa_wholepc_scale` `Test D`):** Indexed `40,080` physical files across `80` directories (`> 35,000` partition threshold) in `3,098.81 ms`; deleting `Bucket_42` (`501` items) offline removed all `Bucket_42` entries on second partitioned scan in **`275.04 ms`** (`filesDeleted = 1` top-level missing dir entry -> recursive subtree delete, verified `0` remaining DB rows).
- **Dynamic Shallow-Watch Attach (`Test D`):** Creating `NewSteamInstall` under a shallow-watched parent (`watchSubtree=false`) emitted `directorySubtreeAdded`, dynamically armed deep watching, and captured subsequent `steam_new.exe` creation live.
- **150 Populated/Nested Directory Renames Under IOCP + `folders_fts` (`qa_v052_full_checks` `Test 2`):**
  - `100` rapid live directory renames (`ProjOrig_00..99 -> ProjRenamed_00..99`) + `30` consecutive rapid multi-hop chain renames (`ProjRenamed_00..09 -> ChainA -> ChainB -> ProjFinal_00..09`) settled via IOCP in **`1,085 ms`**.
  - `20` additional offline renames (`ProjRenamed_80..99 -> ProjOffline_80..99`) reconciled cleanly via `FileIndexer::runScanAndReconcile()`.
  - Final state: `400/400` DB rows intact, `0` stale `ProjOrig`/`ChainA`/`ChainB`/`ProjRenamed_99` entries, `0` missing files or folders across FTS5 `Files` and `Folders` queries.

### Phase 1 Official Benchmark (`phase1_benchmark.exe`, Post-Final-Edit `task-3391`)

| Metric | Target | Measured Result (`100,100` Physical NTFS Files, `101` Dirs) | Status |
|---|---|---|---|
| **Initial Scan Time & Rate** | Fast background | **`33.98 s`** (`2,946 files/sec`, `11.59%` avg CPU across 8 logical cores) | **PASS** |
| **Database Index Size** | `< 100 MB` | **`58.10 MB`** (`60,923,904` bytes) | **PASS** |
| **Working Set / Private Post-Idx** | Low footprint | **`21.74 MB` WS** (Peak `24.72 MB`), **`8.45 MB` Priv** | **PASS** |
| **`200`-Query Search Latency** | `< 50 ms` P95 | Min `0.85 ms` · **`P50 7.45 ms`** · **`P95 17.28 ms`** · `P99 19.02 ms` · Max `21.12 ms` (`7,020` hits) | **PASS** |
| **Incremental Watcher Ops** | Responsive | Add `345.29 ms` · Rename `3.70 ms` · Delete `1.77 ms` | **PASS** |
| **`10 s` Post-Index Idle** | `< 0.1%` CPU | **`0.0000%` CPU**, **`28.68 MB` WS**, **`14.18 MB` Priv** | **PASS** |

*(Note: `phase1_benchmark.exe` generates and scans 100,100 physical NTFS files inside a fresh `QTemporaryDir`; initial throughput varies with Windows Defender / NTFS metadata cache state.)*

---

## 7. Document Extraction & Content Search Results

### Edge PDFium + Scoped AI Integration (`qa_v052_full_checks` `Test 3`, `task-3387`)

- **Real Browser PDF (`msedge.exe 157.0.4341.46` Headless Print):** Generated 2-page PDF (`edge_telemetry_architecture.pdf`), extracted cleanly via `PdfiumBackend` (`pages=2, chars=229, extract_ms=281.26 ms`). Full-text `SearchMode::Content` matched Page 2 token `"HyperionVectorConsolidation"` with **`matchedPage = 2`** and non-empty contextual snippet.
- **Tier-1 vs Tier-2 Isolation:** Out-of-scope Tier-1 file (`C:\Program Files\ThirdPartySuite\vendor_manual.pdf`) returned `1` hit in `SearchMode::Files` and **`0` rows** in `document_content` and `document_chunks`. Removing a watched folder's synthetic file via `pruneContentAndSemanticOutsideRoots()` cleanly pruned `1` stale content entry.

### Phase 2 Official Benchmark (`phase2_benchmark.exe`, Post-Final-Edit `task-3391`)

| Metric (`1,000` Docs: 250 TXT, 250 MD, 250 DOCX, 250 × 3p PDF) | Measured Result |
|---|---|
| **TXT Extraction (`250` docs)** | `358.21 ms` (`1.43 ms/doc`) |
| **MD Extraction (`250` docs)** | `96.58 ms` (`0.39 ms/doc`) |
| **DOCX Extraction (`250` docs)** | `124.26 ms` (`0.50 ms/doc`) |
| **PDF Extraction (`250 × 3p` docs via PDFium)** | `1,300.46 ms` (`5.20 ms/doc`) |
| **End-to-End Content Indexing + DB Growth** | `2,155.37 ms` total · **`1.11 MB` DB (`1.132 KB/doc`)** |
| **Full-Text Search Latency (`30` queries, `1,500` hits)** | **`P50 9.06 ms`** · **`P95 14.41 ms`** (Target `< 150 ms`) · `Max 34.24 ms` |
| **Process Memory** | **`22.25 MB` WS**, **`6.63 MB` Private** |

---

## 8. Semantic AI Results (`all-MiniLM-L6-v2` INT8 ONNX CPU)

### Isolated Semantic + `unloadModel()` Verification (`qa_v052_full_checks` `Test 3`, `task-3387`)

- Natural-language paraphrase query `"gathering sensor data pipelines and compressing neural vector embeddings"` ranked the Edge telemetry architecture PDF `#1` (`topSim = 0.532777`, `547.32 ms` including cold ONNX session load).
- Calling `SemanticEmbeddingEngine::unloadModel()` unloaded the ONNX session cleanly (`isSessionLoaded() == false`) and reduced private process memory from **`49.21 MB -> 16.29 MB`**.

### Phase 3 Official Benchmark (`phase3_benchmark.exe`, Post-Final-Edit `task-3391`)

| Metric (`250` Multi-Domain Docs / Chunks) | Measured Result |
|---|---|
| **Lazy Model Load + First Inference** | `807.63 ms` |
| **Batch Semantic Embedding (`250` chunks)** | `4,130.63 ms` (**`60.5 chunks/sec`**, `16.52 ms/chunk`) |
| **Semantic Search Latency (`40` NL queries, `800` hits)** | **`P50 11.47 ms`** · **`P95 15.81 ms`** (Target `< 150 ms`) · `Max 40.40 ms` |
| **Vector Storage Growth** | `500.00 KB` (`2,048.0 bytes/chunk`) |
| **Memory Profile (`WS` / `Priv`)** | Pre-load: `14.20 / 2.69 MB` → Peak: **`60.33 MB WS`** (Target `< 350 MB`) → Post-`unloadModel()`: **`0.24 MB WS / 17.63 MB Priv`** |

*(Prior `v0.5.1` scaling measurement on `100k` vectors remains valid: `O(N)` linear SQLite scan exceeds `150 ms` above ~55k vectors (`262.42 ms` P95 at 100k — `DEF-07` P2). Because Tier-2 Semantic AI is now strictly scoped to user Library folders with default `5,000`-file cap, real-world vector counts remain well within interactive sub-30ms bounds.)*

---

## 9. Database Integrity & Migration Results

- **Settings JSON Migration (`v2 -> v3`, `Test A`):** Automatically migrated legacy `schemaVersion = 2` settings JSON by enabling `indexWholePc = true` and stripping `"Windows"`, `"Program Files"`, and `"Program Files (x86)"` from `ignoredDirectories` while preserving `"AppData"`, `".git"`, `"node_modules"`.
- **SQLite Migration (`v4 -> v6`, `qa_v052_full_checks` `Test 1`):** Upgraded an authentic `v4` schema DB (`50` folders, `200` files, `schema_migrations` 1..4) cleanly to `schemaVersion() == 6`:
  - Created composite `idx_files_dir_name` (`v5`).
  - Created `folders_fts` FTS5 trigram virtual table + insert/delete/update triggers (`v6`) and backfilled exactly `50` folders (`0` file rows leaked into `folders_fts`).
  - Verified `renamePath`, `upsertFile`, and `removeFileByPath` kept `folders_fts` synchronized (`PRAGMA integrity_check = ok`).
- **Low Disk Space Conditions:** `NOT TESTED`.

---

## 10. Security, Privacy & Distribution Status

- **Offline Guarantee:** `Lumen.exe` (`2,196,992 B`, `v0.5.1.0`) imports zero networking DLLs.
- **Credential & OS Hygiene in Whole-PC Index:** `IndexingPolicy` skips `C:\Windows`, `C:\ProgramData` (except Start Menu), `AppData`, hidden dot-dirs, `pagefile.sys`/`hiberfil.sys`/`swapfile.sys`, `NTUSER.DAT*`, and credential patterns (`.env*`, `id_rsa*`, `id_ed25519*`, `*.pem`, `*.key`, `*.pfx`). Verified zero leaks in live whole-PC `151,303`-item scan.
- **NSIS Packaging:** Cleanly repackaged on `2026-11-11 17:18 +05:00` via [build_setup.ps1](packaging/build_setup.ps1) + NSIS `3.12` after deleting old `build/`, `dist/`, and `releases/` artifacts: `Lumen-v0.5.1-Setup-x64.exe` (`30,711,599 B` / `29.29 MB`, SHA256 `7d1a3036bee3bcecabf2981ee946fd3040ad7ad34a341ba3dfadd22e63ab3add`) and `Lumen-v0.5.1-windows-x64.zip` (`36,749,574 B` / `35.05 MB`, SHA256 `40236336969fee5b24a60cf52359e69d6038cd0eb9aac3923455a7085736d81e`). Isolated `/ISOLATEDTEST /S` install (`29` files in `16,271.09 ms`) and silent uninstall (`0` leftover files, `exit=0`) passed cleanly (`DEF-10` unsigned unless `$env:LUMEN_SIGNING_PFX` is supplied). Local packages remain excluded from Git tracking via `.gitignore`.
- **Git History Exposure (`DEF-01`):** Historical commit `05bc8da` still retains legacy `packaging/certs/UshaLabs.pfx` in Git history until repository owner runs or approves history rewrite (`git filter-repo` + force-push).

---

## 11. Known Open P2/P3 Issues & Limitations

| ID | Severity | Status & Scope Notes |
|---|---|---|
| **DEF-04** | **P2** | NTFS `FindFirstFileExW` traversal skips paths exceeding `MAX_PATH` (`260` chars) where extended `\\?\` prefix isn't handled for non-reconcile edge cases (note `FileIndexer::toLongPath` wraps `\\?\` for child probes; full deep >260 enumeration still needs explicit test). |
| **DEF-05** | **P2** | Multi-token folder + filename cross-column queries (e.g. `"Quarterly Financial"` across dir + file) return `0` hits. |
| **DEF-06** | **P2** | DOCX extraction reads `word/document.xml` only; `word/header*.xml` / `footer*.xml` ignored; legacy `.doc`/`.xls`/`.pptx` unsupported; no OCR on scanned PDFs. |
| **DEF-07** | **P2** | Brute-force `O(N)` cosine scan exceeds `150 ms` if user raises cap above ~55k semantic chunks (mitigated operationally by Tier-2 scoping to Library folders + `5,000`-file default cap). |
| **DEF-08** | **P2** | `5,000`-file content/semantic caps and `10 MB` max document limit should be clearly surfaced/documented in README/Settings; missing custom model path falls back to bundled model. |
| **DEF-10** | **P2** | Local Whole-PC `29.29 MB` NSIS Setup & `35.05 MB` ZIP pass functional checks, while release binaries remain Authenticode `NotSigned` without an external EV/OV cert (`$env:LUMEN_SIGNING_PFX`). |
| **DEF-11 / 12 / 13** | **P3** | `Ctrl+D` pin clears list selection (`DEF-11`); Inspector omits cosine score badge (`DEF-12`); Dark theme toggle not implemented (`DEF-13`). |
| **Hardware / VM Coverage** | **Info** | Physical `125%`/`150%` scaling, multi-monitor, clean non-dev VM, native MS Word / LibreOffice PDFs, and 10/30-min endurance remain `NOT TESTED`. |

*(Note: Previously open `DEF-09` for `live_created_id_ed25519` prefix filtering was closed by [`IndexingPolicy::shouldSkipFile`](src/indexing/IndexingPolicy.cpp), verified by `IndexingPolicy::shouldSkipFile(..., "id_ed25519_work", ...)` in `qa_wholepc_scale` `Test A`.)*

---

## 12. Release Verdict & Recommended Next Steps

### Verdict: **`CONDITIONAL PASS — RELEASE CANDIDATE`**

The **2-Tier Whole-PC Indexing Architecture**, **Win32 IOCP Multi-Target Live Watcher**, **SQLite Schema `v6` (`folders_fts`) Million-Row Optimization**, **Logon Startup / Tray / Desktop Z-Order hardening**, **clean Release build & NSIS/ZIP repackaging**, and **all P1 fixes (`DEF-01`–`DEF-03`)** passed acceptance verification:
- Real PC cold crawl (`151,303` items): **`37.11 s`**, **`25.27 MB` peak WS**, **`73.30 MB` DB**, live `steam` lookup in **`0.34 ms` (Folders) / `1.54 ms` (Files)**.
- Live Whole-PC IOCP idle (`30 s`): **`0.00649%` CPU**, **`4.75 MB` WS**.
- `1,000,000`-row synthetic DB: **`16.44 ms` P50 / `25.12 ms` P95 / `31.22 ms` Max**, **`29.38 MB` WS**, **`364.00 MB` DB**.
- `150` directory renames (`100` live IOCP + `30` chain + `20` offline), schema `v4 -> v6` migration, Edge `157` headless PDF extraction (`PdfiumBackend`), scoped MiniLM ONNX semantic search (`unloadModel()` -> `0.24 MB` WS in Phase 3 / `16.29 MB` Priv in Edge test), `SettingsDialog` Whole-PC binding, and isolated `29.29 MB` NSIS Setup install/uninstall (`29` files, `0` leftovers) all passed with `0` failures.

### Recommended Actions Before Public Distribution

1. **Merge & Release Readiness:** Review branch `WholePC/Search` before merging into `main`.
2. **Production Signing & History Hygiene:** Inject trusted Authenticode credentials (`$env:LUMEN_SIGNING_PFX` / `$env:LUMEN_SIGNING_PASSWORD`) at release build time (`DEF-10`) and execute owner-approved Git history purge for historical commit `05bc8da` (`DEF-01`).
