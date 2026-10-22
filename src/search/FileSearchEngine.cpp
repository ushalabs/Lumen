#include "search/FileSearchEngine.h"

#include "core/AppPaths.h"
#include "core/Logger.h"
#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "extraction/DocumentExtractor.h"
#include "semantic/SemanticEmbeddingEngine.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <sqlite3.h>

namespace lumen::search {

namespace {
int sqliteCancelCallback(void* clientData) {
    const auto* cancelToken = static_cast<const std::atomic_bool*>(clientData);
    if (cancelToken != nullptr && cancelToken->load(std::memory_order_relaxed)) {
        return 1; // Non-zero aborts sqlite3_step with SQLITE_INTERRUPT
    }
    return 0;
}

QString extractParentDir(const QString& fullNativePath) {
    const int lastSep = fullNativePath.lastIndexOf(QLatin1Char('\\'));
    if (lastSep > 0) {
        return fullNativePath.left(lastSep);
    }
    return {};
}

bool passesMetadataFilters(const FileSearchResult& item, const SearchFilters& filters) {
    if (!filters.extensions.isEmpty()) {
        bool extMatch = false;
        for (const QString& ext : filters.extensions) {
            if (item.extension.compare(ext, Qt::CaseInsensitive) == 0) {
                extMatch = true;
                break;
            }
        }
        if (!extMatch) {
            return false;
        }
    }
    if (!filters.pathPrefix.isEmpty()) {
        if (!item.path.startsWith(filters.pathPrefix, Qt::CaseInsensitive)) {
            return false;
        }
    }
    if (filters.minSizeBytes >= 0 && item.size < filters.minSizeBytes) {
        return false;
    }
    if (filters.maxSizeBytes >= 0 && item.size > filters.maxSizeBytes) {
        return false;
    }
    if (filters.modifiedAfterEpoch >= 0 && item.mtime < filters.modifiedAfterEpoch) {
        return false;
    }
    return true;
}

bool isWordBoundaryChar(QChar c) {
    return c == QLatin1Char('_') || c == QLatin1Char('-') ||
           c == QLatin1Char(' ') || c == QLatin1Char('.') ||
           c == QLatin1Char('(') || c == QLatin1Char('[');
}

QString sanitizeFtsWordToken(const QString& rawToken) {
    QString clean;
    clean.reserve(rawToken.size());
    for (QChar ch : rawToken) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('_')) {
            clean.append(ch);
        }
    }
    return clean;
}
} // namespace

FileSearchEngine::FileSearchEngine(const QString& dbFilePath)
    : m_dbFilePath(dbFilePath) {}

FileSearchEngine::~FileSearchEngine() {
    QMutexLocker locker(&m_readMutex);
    closeReadConnectionUnlocked();
}

void FileSearchEngine::setDatabasePath(const QString& dbFilePath) {
    QMutexLocker locker(&m_readMutex);
    if (m_dbFilePath != dbFilePath) {
        closeReadConnectionUnlocked();
        m_dbFilePath = dbFilePath;
    }
}

bool FileSearchEngine::ensureReadConnectionUnlocked() {
    const QString targetPath = m_dbFilePath.isEmpty()
                                   ? core::AppPaths::databaseFilePath()
                                   : m_dbFilePath;
    if (m_readDb != nullptr && m_dbFilePath == targetPath) {
        return true;
    }

    closeReadConnectionUnlocked();
    m_dbFilePath = targetPath;

    if (!QFileInfo::exists(m_dbFilePath)) {
        return false;
    }

    const QByteArray utf8Path = QDir::toNativeSeparators(m_dbFilePath).toUtf8();
    const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    if (sqlite3_open_v2(utf8Path.constData(), &m_readDb, flags, nullptr) != SQLITE_OK || m_readDb == nullptr) {
        closeReadConnectionUnlocked();
        return false;
    }

    sqlite3_busy_timeout(m_readDb, 2000);
    sqlite3_exec(m_readDb, "PRAGMA temp_store = MEMORY; PRAGMA cache_size = -4096;", nullptr, nullptr, nullptr);
    return true;
}

void FileSearchEngine::closeReadConnectionUnlocked() {
    if (m_readDb != nullptr) {
        sqlite3_progress_handler(m_readDb, 0, nullptr, nullptr);
        sqlite3_close_v2(m_readDb);
        m_readDb = nullptr;
    }
}

double FileSearchEngine::computeFilenameScore(const QString& fileName,
                                              const QString& extension,
                                              qint64 mtime,
                                              const QString& fullQueryLower,
                                              const QStringList& queryTokensLower,
                                              int* outMatchStart,
                                              int* outMatchLen) {
    const QString nameLower = fileName.toLower();
    const int dotPos = nameLower.lastIndexOf(QLatin1Char('.'));
    const QString stemLower = (dotPos > 0) ? nameLower.left(dotPos) : nameLower;

    double score = 100.0;
    int firstMatchIdx = -1;
    int firstMatchLen = 0;

    // 1. Check full query match against full name or stem
    if (nameLower == fullQueryLower || stemLower == fullQueryLower) {
        score += 1000.0;
        firstMatchIdx = 0;
        firstMatchLen = fullQueryLower.size();
    } else if (nameLower.startsWith(fullQueryLower)) {
        score += 650.0;
        firstMatchIdx = 0;
        firstMatchLen = fullQueryLower.size();
    } else {
        const int idx = nameLower.indexOf(fullQueryLower);
        if (idx >= 0) {
            firstMatchIdx = idx;
            firstMatchLen = fullQueryLower.size();
            if (idx > 0 && isWordBoundaryChar(nameLower.at(idx - 1))) {
                score += 420.0;
            } else {
                score += 260.0 - std::min(80.0, idx * 2.0);
            }
        }
    }

    // 2. Multi-token evaluation
    for (const QString& token : queryTokensLower) {
        if (token.isEmpty()) {
            continue;
        }
        const int tIdx = nameLower.indexOf(token);
        if (tIdx < 0) {
            if (extension.compare(token, Qt::CaseInsensitive) == 0) {
                score += 80.0;
                continue;
            }
            return -1.0; // Candidate does not contain required token
        }
        if (firstMatchIdx < 0) {
            firstMatchIdx = tIdx;
            firstMatchLen = token.size();
        }
        if (tIdx == 0) {
            score += 120.0;
        } else if (isWordBoundaryChar(nameLower.at(tIdx - 1))) {
            score += 75.0;
        } else {
            score += 30.0;
        }
    }

    // 3. Compactness bonus (shorter, tighter filenames rank above deeply suffixed names)
    const int extraChars = std::max(0, static_cast<int>(fileName.size() - fullQueryLower.size()));
    score += std::max(0.0, 90.0 - extraChars * 1.5);

    // 4. Recency tie-breaker bonus (up to +35 points for recent files)
    if (mtime > 1577836800) { // After 2020-01-01
        const double daysSince2020 = static_cast<double>(mtime - 1577836800) / 86400.0;
        score += std::clamp(daysSince2020 * 0.01, 0.0, 35.0);
    }

    if (outMatchStart != nullptr) {
        *outMatchStart = (firstMatchIdx >= 0) ? firstMatchIdx : 0;
    }
    if (outMatchLen != nullptr) {
        *outMatchLen = firstMatchLen;
    }
    return score;
}

SearchFilters FileSearchEngine::filtersForSearchMode(SearchMode mode,
                                                     const SearchFilters& baseFilters) {
    SearchFilters f = baseFilters;
    switch (mode) {
        case SearchMode::All:
            f.includeFilenameMatches = true;
            f.includeContentMatches = true;
            f.includeSemanticMatches = true;
            break;
        case SearchMode::Files:
            f.includeFilenameMatches = true;
            f.includeContentMatches = false;
            f.includeSemanticMatches = false;
            break;
        case SearchMode::Content:
            f.includeFilenameMatches = false;
            f.includeContentMatches = true;
            f.includeSemanticMatches = false;
            break;
        case SearchMode::Semantic:
            f.includeFilenameMatches = false;
            f.includeContentMatches = false;
            f.includeSemanticMatches = true;
            break;
    }
    return f;
}

QString FileSearchEngine::parseInlineQueryAndFilters(const QString& rawQuery,
                                                     const SearchFilters& baseFilters,
                                                     SearchFilters* outFilters) {
    SearchFilters active = baseFilters;
    QStringList remainingWords;

    auto parseSizeBytes = [](const QString& valStr) -> qint64 {
        QString s = valStr.trimmed().toLower();
        qint64 mult = 1;
        if (s.endsWith(QStringLiteral("gb"))) {
            mult = 1024LL * 1024LL * 1024LL;
            s.chop(2);
        } else if (s.endsWith(QStringLiteral("mb"))) {
            mult = 1024LL * 1024LL;
            s.chop(2);
        } else if (s.endsWith(QStringLiteral("kb"))) {
            mult = 1024LL;
            s.chop(2);
        } else if (s.endsWith(QLatin1Char('b'))) {
            s.chop(1);
        }
        bool ok = false;
        const double num = s.toDouble(&ok);
        return (ok && num >= 0.0) ? static_cast<qint64>(num * static_cast<double>(mult)) : -1;
    };

    const QStringList parts = rawQuery.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (part.startsWith(QStringLiteral("ext:"), Qt::CaseInsensitive) && part.size() > 4) {
            const QStringList exts = part.mid(4).split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (QString ext : exts) {
                ext = ext.trimmed().toLower();
                if (ext.startsWith(QLatin1Char('.'))) {
                    ext.remove(0, 1);
                }
                if (!ext.isEmpty()) {
                    active.extensions.append(ext);
                }
            }
            continue;
        }
        if (part.startsWith(QStringLiteral("type:"), Qt::CaseInsensitive) && part.size() > 5) {
            const QString t = part.mid(5).trimmed().toLower();
            if (t == QStringLiteral("doc") || t == QStringLiteral("docs") || t == QStringLiteral("document")) {
                active.extensions.append({QStringLiteral("pdf"), QStringLiteral("docx"), QStringLiteral("doc"),
                                          QStringLiteral("txt"), QStringLiteral("md"), QStringLiteral("rtf")});
            } else if (t == QStringLiteral("code")) {
                active.extensions.append({QStringLiteral("cpp"), QStringLiteral("h"), QStringLiteral("hpp"),
                                          QStringLiteral("c"), QStringLiteral("py"), QStringLiteral("js"),
                                          QStringLiteral("ts"), QStringLiteral("json"), QStringLiteral("rs"),
                                          QStringLiteral("go"), QStringLiteral("cs")});
            } else if (t == QStringLiteral("image") || t == QStringLiteral("img")) {
                active.extensions.append({QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                          QStringLiteral("svg"), QStringLiteral("webp"), QStringLiteral("bmp"),
                                          QStringLiteral("ico"), QStringLiteral("gif")});
            } else if (t == QStringLiteral("sheet") || t == QStringLiteral("spreadsheet")) {
                active.extensions.append({QStringLiteral("xlsx"), QStringLiteral("xls"), QStringLiteral("csv")});
            }
            continue;
        }
        if ((part.startsWith(QStringLiteral("in:"), Qt::CaseInsensitive) && part.size() > 3) ||
            (part.startsWith(QStringLiteral("folder:"), Qt::CaseInsensitive) && part.size() > 7)) {
            const int colonIdx = part.indexOf(QLatin1Char(':'));
            const QString folderToken = QDir::toNativeSeparators(part.mid(colonIdx + 1).trimmed());
            if (!folderToken.isEmpty()) {
                active.pathPrefix = folderToken;
            }
            continue;
        }
        if (part.startsWith(QStringLiteral("size:"), Qt::CaseInsensitive) && part.size() > 5) {
            const QString expr = part.mid(5).trimmed();
            if (expr.startsWith(QLatin1Char('>'))) {
                const qint64 b = parseSizeBytes(expr.mid(1));
                if (b >= 0) {
                    active.minSizeBytes = b;
                }
            } else if (expr.startsWith(QLatin1Char('<'))) {
                const qint64 b = parseSizeBytes(expr.mid(1));
                if (b >= 0) {
                    active.maxSizeBytes = b;
                }
            }
            continue;
        }
        if (part.startsWith(QStringLiteral("modified:"), Qt::CaseInsensitive) && part.size() > 9) {
            const QString mod = part.mid(9).trimmed().toLower();
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            if (mod == QStringLiteral("today") || mod == QStringLiteral("1d") || mod == QStringLiteral("24h")) {
                active.modifiedAfterEpoch = now - 86400;
            } else if (mod == QStringLiteral("week") || mod == QStringLiteral("7d")) {
                active.modifiedAfterEpoch = now - 7 * 86400;
            } else if (mod == QStringLiteral("month") || mod == QStringLiteral("30d")) {
                active.modifiedAfterEpoch = now - 30 * 86400;
            } else if (mod == QStringLiteral("year") || mod == QStringLiteral("365d")) {
                active.modifiedAfterEpoch = now - 365 * 86400;
            }
            continue;
        }
        if (part.startsWith(QStringLiteral("mode:"), Qt::CaseInsensitive) && part.size() > 5) {
            const QString m = part.mid(5).trimmed().toLower();
            if (m == QStringLiteral("files") || m == QStringLiteral("file")) {
                active = filtersForSearchMode(SearchMode::Files, active);
            } else if (m == QStringLiteral("content")) {
                active = filtersForSearchMode(SearchMode::Content, active);
            } else if (m == QStringLiteral("semantic") || m == QStringLiteral("sem")) {
                active = filtersForSearchMode(SearchMode::Semantic, active);
            } else if (m == QStringLiteral("all")) {
                active = filtersForSearchMode(SearchMode::All, active);
            }
            continue;
        }
        if (part.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
            active = filtersForSearchMode(SearchMode::Files, active);
            const QString rest = part.mid(5).trimmed();
            if (!rest.isEmpty()) {
                remainingWords.append(rest);
            }
            continue;
        }
        if (part.startsWith(QStringLiteral("content:"), Qt::CaseInsensitive)) {
            active = filtersForSearchMode(SearchMode::Content, active);
            const QString rest = part.mid(8).trimmed();
            if (!rest.isEmpty()) {
                remainingWords.append(rest);
            }
            continue;
        }
        if (part.startsWith(QStringLiteral("sem:"), Qt::CaseInsensitive) ||
            part.startsWith(QStringLiteral("semantic:"), Qt::CaseInsensitive)) {
            active = filtersForSearchMode(SearchMode::Semantic, active);
            const int colonIdx = part.indexOf(QLatin1Char(':'));
            const QString rest = part.mid(colonIdx + 1).trimmed();
            if (!rest.isEmpty()) {
                remainingWords.append(rest);
            }
            continue;
        }

        remainingWords.append(part);
    }

    if (outFilters != nullptr) {
        *outFilters = active;
    }
    return remainingWords.join(QLatin1Char(' '));
}

QVector<FileSearchResult> FileSearchEngine::searchFiles(
    const QString& query,
    const SearchFilters& filters,
    int limit,
    const std::shared_ptr<std::atomic_bool>& cancellationToken,
    double* outElapsedMs) {
    QElapsedTimer timer;
    timer.start();

    QVector<FileSearchResult> finalResults;
    const QString trimmedQuery = query.trimmed();
    if (trimmedQuery.isEmpty() || limit <= 0) {
        if (outElapsedMs) {
            *outElapsedMs = 0.0;
        }
        return finalResults;
    }

    if (cancellationToken && cancellationToken->load(std::memory_order_relaxed)) {
        return finalResults;
    }

    SearchFilters activeFilters = filters;
    const QString cleanedQuery = parseInlineQueryAndFilters(trimmedQuery, filters, &activeFilters);

    QStringList queryTokensLower;
    QStringList ftsTrigTokens;
    QStringList ftsDocTokens;

    const QStringList rawParts = cleanedQuery.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& part : rawParts) {
        const QString lowerPart = part.toLower();
        queryTokensLower.append(lowerPart);
        if (lowerPart.size() >= 3) {
            QString escaped = lowerPart;
            escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            ftsTrigTokens.append(QStringLiteral("\"%1\"").arg(escaped));
        }
        const QString cleanWord = sanitizeFtsWordToken(lowerPart);
        if (cleanWord.size() >= 2) {
            ftsDocTokens.append(QStringLiteral("\"%1\"*").arg(cleanWord));
        }
    }

    const QString effectiveQueryLower = queryTokensLower.join(QLatin1Char(' '));

    QMutexLocker locker(&m_readMutex);
    if (!ensureReadConnectionUnlocked()) {
        if (outElapsedMs) {
            *outElapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        }
        return finalResults;
    }

    if (cancellationToken) {
        sqlite3_progress_handler(m_readDb, 128, sqliteCancelCallback, cancellationToken.get());
    } else {
        sqlite3_progress_handler(m_readDb, 0, nullptr, nullptr);
    }

    QHash<qint64, FileSearchResult> candidateMap;
    const int prefixFetchLimit = std::max(limit * 3, 150);
    const int ftsFetchLimit = std::max(limit * 8, 400);
    const int docFtsFetchLimit = std::max(limit * 4, 160);

    auto collectRow = [&](sqlite3_stmt* stmt) {
        const qint64 id = sqlite3_column_int64(stmt, 0);
        if (candidateMap.contains(id)) {
            return;
        }
        FileSearchResult item;
        item.id = id;
        const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* rawName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* rawExt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        item.size = sqlite3_column_int64(stmt, 4);
        item.mtime = sqlite3_column_int64(stmt, 5);

        item.path = rawPath ? QString::fromUtf8(rawPath) : QString();
        item.name = rawName ? QString::fromUtf8(rawName) : QString();
        item.extension = rawExt ? QString::fromUtf8(rawExt) : QString();
        item.parentDirectory = extractParentDir(item.path);

        if (!passesMetadataFilters(item, activeFilters)) {
            return;
        }

        int matchStart = 0;
        int matchLen = 0;
        const double score = computeFilenameScore(
            item.name, item.extension, item.mtime, effectiveQueryLower, queryTokensLower, &matchStart, &matchLen);
        if (score >= 0.0) {
            item.score = score;
            item.matchStart = matchStart;
            item.matchLength = matchLen;
            item.matchType = SearchResultMatchType::Filename;
            candidateMap.insert(id, std::move(item));
        }
    };

    // Pass 1: B-tree index prefix lookup on idx_files_name (handles 1-2 char queries AND guarantees exact/prefix hits)
    const QString firstToken = queryTokensLower.isEmpty() ? effectiveQueryLower : queryTokensLower.first();
    if (activeFilters.includeFilenameMatches && !firstToken.isEmpty()) {
        const QString upperBound = firstToken + QChar(0xFFFF);
        const char* prefixSql =
            "SELECT id, path, name, extension, size, mtime "
            "FROM files "
            "WHERE name >= ?1 AND name <= ?2 COLLATE NOCASE "
            "LIMIT ?3;";
        database::ScopedStmt stmt;
        if (sqlite3_prepare_v2(m_readDb, prefixSql, -1, stmt.receive(), nullptr) == SQLITE_OK) {
            const QByteArray lowerUtf8 = firstToken.toUtf8();
            const QByteArray upperUtf8 = upperBound.toUtf8();
            sqlite3_bind_text(stmt.get(), 1, lowerUtf8.constData(), lowerUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_text(stmt.get(), 2, upperUtf8.constData(), upperUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_int(stmt.get(), 3, prefixFetchLimit);

            while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
                collectRow(stmt.get());
            }
        }
    }

    // Pass 2: FTS5 trigram substring lookup for queries with >= 3 character token(s)
    if (activeFilters.includeFilenameMatches && !ftsTrigTokens.isEmpty() &&
        (!cancellationToken || !cancellationToken->load(std::memory_order_relaxed))) {
        const QString matchExpr = ftsTrigTokens.join(QStringLiteral(" AND "));
        const char* ftsSql =
            "SELECT f.id, f.path, f.name, f.extension, f.size, f.mtime "
            "FROM files_fts "
            "JOIN files f ON f.id = files_fts.rowid "
            "WHERE files_fts MATCH ?1 "
            "LIMIT ?2;";
        database::ScopedStmt ftsStmt;
        if (sqlite3_prepare_v2(m_readDb, ftsSql, -1, ftsStmt.receive(), nullptr) == SQLITE_OK) {
            const QByteArray matchUtf8 = matchExpr.toUtf8();
            sqlite3_bind_text(ftsStmt.get(), 1, matchUtf8.constData(), matchUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_int(ftsStmt.get(), 2, ftsFetchLimit);

            while (sqlite3_step(ftsStmt.get()) == SQLITE_ROW) {
                collectRow(ftsStmt.get());
            }
        }
    }

    // Pass 3: If user filtered strictly by extension (e.g. "ext:pdf") with no filename tokens
    if (activeFilters.includeFilenameMatches && queryTokensLower.isEmpty() && !activeFilters.extensions.isEmpty() &&
        (!cancellationToken || !cancellationToken->load(std::memory_order_relaxed))) {
        const QString extTarget = activeFilters.extensions.first();
        const char* extSql =
            "SELECT id, path, name, extension, size, mtime "
            "FROM files "
            "WHERE extension = ?1 COLLATE NOCASE "
            "LIMIT ?2;";
        database::ScopedStmt extStmt;
        if (sqlite3_prepare_v2(m_readDb, extSql, -1, extStmt.receive(), nullptr) == SQLITE_OK) {
            const QByteArray extUtf8 = extTarget.toUtf8();
            sqlite3_bind_text(extStmt.get(), 1, extUtf8.constData(), extUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_int(extStmt.get(), 2, prefixFetchLimit);

            while (sqlite3_step(extStmt.get()) == SQLITE_ROW) {
                collectRow(extStmt.get());
            }
        }
    }

    // Pass 4: Full-text document content search via document_fts (unicode61 + BM25 + snippet + page number)
    if (activeFilters.includeContentMatches && !ftsDocTokens.isEmpty() &&
        (!cancellationToken || !cancellationToken->load(std::memory_order_relaxed))) {
        const QString docMatchExpr = ftsDocTokens.join(QStringLiteral(" AND "));
        const QString firstSearchTerm = sanitizeFtsWordToken(firstToken);
        const char* docSql =
            "SELECT f.id, f.path, f.name, f.extension, f.size, f.mtime, "
            "       snippet(document_fts, 0, '«', '»', ' … ', 9) AS snip, "
            "       bm25(document_fts) AS rank_score, "
            "       dc.page_offsets_json, "
            "       CASE WHEN dc.page_offsets_json != '' THEN INSTR(LOWER(dc.body_text), ?2) ELSE 0 END AS match_pos "
            "FROM document_fts "
            "JOIN document_content dc ON dc.file_id = document_fts.rowid "
            "JOIN files f ON f.id = document_fts.rowid "
            "WHERE document_fts MATCH ?1 "
            "LIMIT ?3;";
        database::ScopedStmt docStmt;
        if (sqlite3_prepare_v2(m_readDb, docSql, -1, docStmt.receive(), nullptr) == SQLITE_OK) {
            const QByteArray docMatchUtf8 = docMatchExpr.toUtf8();
            const QByteArray firstTermUtf8 = firstSearchTerm.toUtf8();
            sqlite3_bind_text(docStmt.get(), 1, docMatchUtf8.constData(), docMatchUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_text(docStmt.get(), 2, firstTermUtf8.constData(), firstTermUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_int(docStmt.get(), 3, docFtsFetchLimit);

            while (sqlite3_step(docStmt.get()) == SQLITE_ROW) {
                const qint64 id = sqlite3_column_int64(docStmt.get(), 0);
                const char* rawSnip = reinterpret_cast<const char*>(sqlite3_column_text(docStmt.get(), 6));
                const double bm25Val = sqlite3_column_double(docStmt.get(), 7);
                const char* rawPageJson = reinterpret_cast<const char*>(sqlite3_column_text(docStmt.get(), 8));
                const int matchPos1Based = sqlite3_column_int(docStmt.get(), 9);

                const QString snippetText = rawSnip ? QString::fromUtf8(rawSnip).simplified() : QString();
                const QString pageOffsetsJson = rawPageJson ? QString::fromUtf8(rawPageJson) : QString();
                int matchedPage = 0;
                if (!pageOffsetsJson.isEmpty()) {
                    const int zeroBasedPos = std::max(0, matchPos1Based - 1);
                    matchedPage = extraction::DocumentExtractor::resolvePageForCharOffset(pageOffsetsJson, zeroBasedPos);
                }

                auto existingIt = candidateMap.find(id);
                if (existingIt != candidateMap.end()) {
                    existingIt->snippet = snippetText;
                    existingIt->matchedPage = matchedPage;
                    existingIt->matchType = SearchResultMatchType::FilenameAndContent;
                    existingIt->score += 140.0 + std::clamp(-bm25Val * 12.0, 0.0, 120.0);
                    continue;
                }

                FileSearchResult item;
                item.id = id;
                const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(docStmt.get(), 1));
                const char* rawName = reinterpret_cast<const char*>(sqlite3_column_text(docStmt.get(), 2));
                const char* rawExt = reinterpret_cast<const char*>(sqlite3_column_text(docStmt.get(), 3));
                item.size = sqlite3_column_int64(docStmt.get(), 4);
                item.mtime = sqlite3_column_int64(docStmt.get(), 5);

                item.path = rawPath ? QString::fromUtf8(rawPath) : QString();
                item.name = rawName ? QString::fromUtf8(rawName) : QString();
                item.extension = rawExt ? QString::fromUtf8(rawExt) : QString();
                item.parentDirectory = extractParentDir(item.path);

                if (!passesMetadataFilters(item, activeFilters)) {
                    continue;
                }

                item.snippet = snippetText;
                item.matchedPage = matchedPage;
                item.matchType = SearchResultMatchType::Content;

                double contentScore = 240.0 + std::clamp(-bm25Val * 18.0, 0.0, 180.0);
                if (item.mtime > 1577836800) {
                    const double daysSince2020 = static_cast<double>(item.mtime - 1577836800) / 86400.0;
                    contentScore += std::clamp(daysSince2020 * 0.008, 0.0, 25.0);
                }
                item.score = contentScore;
                candidateMap.insert(id, std::move(item));
            }
        }
    }

    // Pass 5: Offline Semantic Vector Search over document_chunks (when chunks exist in DB)
    if (activeFilters.includeSemanticMatches && effectiveQueryLower.size() >= 3 &&
        (!cancellationToken || !cancellationToken->load(std::memory_order_relaxed))) {
        bool hasChunksInDb = false;
        {
            database::ScopedStmt checkChunksStmt;
            if (sqlite3_prepare_v2(m_readDb, "SELECT 1 FROM document_chunks LIMIT 1;", -1, checkChunksStmt.receive(), nullptr) == SQLITE_OK &&
                sqlite3_step(checkChunksStmt.get()) == SQLITE_ROW) {
                hasChunksInDb = true;
            }
        }

        if (hasChunksInDb) {
            const std::vector<float> queryVec =
                semantic::SemanticEmbeddingEngine::instance().embedText(effectiveQueryLower);
            if (queryVec.size() == semantic::SemanticEmbeddingEngine::kEmbeddingDimension &&
                (!cancellationToken || !cancellationToken->load(std::memory_order_relaxed))) {
                const char* chunkSql =
                    "SELECT ch.file_id, ch.page_number, ch.snippet_preview, ch.embedding, "
                    "       f.path, f.name, f.extension, f.size, f.mtime "
                    "FROM document_chunks ch "
                    "JOIN files f ON f.id = ch.file_id;";
                database::ScopedStmt chunkStmt;
                if (sqlite3_prepare_v2(m_readDb, chunkSql, -1, chunkStmt.receive(), nullptr) == SQLITE_OK) {
                    constexpr int kExpectedBlobBytes =
                        semantic::SemanticEmbeddingEngine::kEmbeddingDimension * static_cast<int>(sizeof(float));

                    while (sqlite3_step(chunkStmt.get()) == SQLITE_ROW) {
                        const int blobBytes = sqlite3_column_bytes(chunkStmt.get(), 3);
                        if (blobBytes != kExpectedBlobBytes) {
                            continue;
                        }
                        const auto* chunkVec = reinterpret_cast<const float*>(sqlite3_column_blob(chunkStmt.get(), 3));
                        const float sim = semantic::SemanticEmbeddingEngine::cosineSimilarity(
                            queryVec.data(), chunkVec, semantic::SemanticEmbeddingEngine::kEmbeddingDimension);

                        if (sim < activeFilters.minSemanticSimilarity) {
                            continue;
                        }

                        const qint64 fileId = sqlite3_column_int64(chunkStmt.get(), 0);
                        const int pageNum = sqlite3_column_int(chunkStmt.get(), 1);
                        const char* rawPreview = reinterpret_cast<const char*>(sqlite3_column_text(chunkStmt.get(), 2));
                        const QString previewText = rawPreview ? QString::fromUtf8(rawPreview) : QString();

                        auto existingIt = candidateMap.find(fileId);
                        if (existingIt != candidateMap.end()) {
                            if (sim > existingIt->semanticSimilarity) {
                                existingIt->semanticSimilarity = sim;
                                if (existingIt->snippet.isEmpty()) {
                                    existingIt->snippet = previewText;
                                    existingIt->matchedPage = (pageNum > 1) ? pageNum : existingIt->matchedPage;
                                }
                            }
                            if (existingIt->matchType != SearchResultMatchType::Semantic &&
                                existingIt->matchType != SearchResultMatchType::Hybrid) {
                                existingIt->matchType = SearchResultMatchType::Hybrid;
                                existingIt->score += static_cast<double>(sim) * 130.0;
                            } else if (existingIt->matchType == SearchResultMatchType::Semantic) {
                                const double newSemScore = 160.0 + static_cast<double>(sim) * 220.0;
                                if (newSemScore > existingIt->score) {
                                    existingIt->score = newSemScore;
                                    existingIt->snippet = previewText;
                                    existingIt->matchedPage = (pageNum > 1) ? pageNum : 0;
                                }
                            }
                            continue;
                        }

                        FileSearchResult item;
                        item.id = fileId;
                        const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(chunkStmt.get(), 4));
                        const char* rawName = reinterpret_cast<const char*>(sqlite3_column_text(chunkStmt.get(), 5));
                        const char* rawExt = reinterpret_cast<const char*>(sqlite3_column_text(chunkStmt.get(), 6));
                        item.size = sqlite3_column_int64(chunkStmt.get(), 7);
                        item.mtime = sqlite3_column_int64(chunkStmt.get(), 8);

                        item.path = rawPath ? QString::fromUtf8(rawPath) : QString();
                        item.name = rawName ? QString::fromUtf8(rawName) : QString();
                        item.extension = rawExt ? QString::fromUtf8(rawExt) : QString();
                        item.parentDirectory = extractParentDir(item.path);

                        if (!passesMetadataFilters(item, activeFilters)) {
                            continue;
                        }

                        item.matchType = SearchResultMatchType::Semantic;
                        item.snippet = previewText;
                        item.matchedPage = (pageNum > 1) ? pageNum : 0;
                        item.semanticSimilarity = sim;

                        // Semantic-only score: ranks meaningfully alongside content hits without outranking exact filename matches
                        double semScore = 160.0 + static_cast<double>(sim) * 220.0;
                        if (item.mtime > 1577836800) {
                            const double daysSince2020 = static_cast<double>(item.mtime - 1577836800) / 86400.0;
                            semScore += std::clamp(daysSince2020 * 0.006, 0.0, 20.0);
                        }
                        item.score = semScore;
                        candidateMap.insert(fileId, std::move(item));
                    }
                }
            }
        }
    }

    // Pass 6: Filter-only query fallback (e.g. "ext:pdf", "type:doc", "modified:today", "size:>100kb")
    if (queryTokensLower.isEmpty() && candidateMap.isEmpty()) {
        const bool hasMetadataFilter = !activeFilters.extensions.isEmpty() ||
                                       !activeFilters.pathPrefix.isEmpty() ||
                                       activeFilters.minSizeBytes >= 0 ||
                                       activeFilters.maxSizeBytes >= 0 ||
                                       activeFilters.modifiedAfterEpoch >= 0;
        if (hasMetadataFilter) {
            const char* filterSql =
                "SELECT id, path, name, extension, size, mtime "
                "FROM files "
                "ORDER BY mtime DESC LIMIT ?1;";
            database::ScopedStmt filterStmt;
            if (sqlite3_prepare_v2(m_readDb, filterSql, -1, filterStmt.receive(), nullptr) == SQLITE_OK) {
                sqlite3_bind_int(filterStmt.get(), 1, std::max(limit * 8, 400));
                while (sqlite3_step(filterStmt.get()) == SQLITE_ROW) {
                    const qint64 fid = sqlite3_column_int64(filterStmt.get(), 0);
                    FileSearchResult item;
                    item.id = fid;
                    const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(filterStmt.get(), 1));
                    const char* rawName = reinterpret_cast<const char*>(sqlite3_column_text(filterStmt.get(), 2));
                    const char* rawExt = reinterpret_cast<const char*>(sqlite3_column_text(filterStmt.get(), 3));
                    item.size = sqlite3_column_int64(filterStmt.get(), 4);
                    item.mtime = sqlite3_column_int64(filterStmt.get(), 5);
                    item.path = rawPath ? QString::fromUtf8(rawPath) : QString();
                    item.name = rawName ? QString::fromUtf8(rawName) : QString();
                    item.extension = rawExt ? QString::fromUtf8(rawExt) : QString();
                    item.parentDirectory = extractParentDir(item.path);

                    if (!passesMetadataFilters(item, activeFilters)) {
                        continue;
                    }
                    item.score = 100.0 + std::clamp(static_cast<double>(item.mtime - 1577836800) / 86400.0 * 0.01, 0.0, 35.0);
                    candidateMap.insert(fid, std::move(item));
                    if (candidateMap.size() >= limit) {
                        break;
                    }
                }
            }
        }
    }

    sqlite3_progress_handler(m_readDb, 0, nullptr, nullptr);

    if (cancellationToken && cancellationToken->load(std::memory_order_relaxed)) {
        return {};
    }

    const QStringList pinnedList = core::SettingsManager::instance().pinnedFiles();
    QSet<QString> pinnedSetLower;
    for (const QString& p : pinnedList) {
        pinnedSetLower.insert(QDir::toNativeSeparators(p).toLower());
    }

    finalResults.reserve(candidateMap.size());
    for (auto it = candidateMap.begin(); it != candidateMap.end(); ++it) {
        FileSearchResult item = std::move(it.value());
        if (!pinnedSetLower.isEmpty() &&
            pinnedSetLower.contains(QDir::toNativeSeparators(item.path).toLower())) {
            item.isPinned = true;
            item.score += 500.0;
        }
        finalResults.append(std::move(item));
    }

    const int sortCount = std::min(static_cast<int>(finalResults.size()), limit);
    std::partial_sort(
        finalResults.begin(),
        finalResults.begin() + sortCount,
        finalResults.end(),
        [](const FileSearchResult& a, const FileSearchResult& b) {
            if (std::abs(a.score - b.score) > 1e-4) {
                return a.score > b.score;
            }
            if (a.mtime != b.mtime) {
                return a.mtime > b.mtime;
            }
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });

    if (finalResults.size() > limit) {
        finalResults.resize(limit);
    }

    if (outElapsedMs != nullptr) {
        *outElapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    }
    return finalResults;
}

QVector<FileSearchResult> FileSearchEngine::findRelatedDocuments(
    qint64 sourceFileId,
    int limit,
    double* outElapsedMs) {
    QElapsedTimer timer;
    timer.start();

    QVector<FileSearchResult> results;
    if (sourceFileId <= 0 || limit <= 0) {
        if (outElapsedMs) {
            *outElapsedMs = 0.0;
        }
        return results;
    }

    QMutexLocker locker(&m_readMutex);
    if (!ensureReadConnectionUnlocked()) {
        return results;
    }

    constexpr int kDim = semantic::SemanticEmbeddingEngine::kEmbeddingDimension;
    constexpr int kExpectedBlobBytes = kDim * static_cast<int>(sizeof(float));

    // Compute L2-normalized centroid of the source document's chunks
    std::vector<float> sourceCentroid(kDim, 0.0f);
    int sourceChunkCount = 0;
    {
        database::ScopedStmt srcStmt;
        if (sqlite3_prepare_v2(m_readDb,
                               "SELECT embedding FROM document_chunks WHERE file_id = ?1;",
                               -1,
                               srcStmt.receive(),
                               nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(srcStmt.get(), 1, sourceFileId);
            while (sqlite3_step(srcStmt.get()) == SQLITE_ROW) {
                if (sqlite3_column_bytes(srcStmt.get(), 0) != kExpectedBlobBytes) {
                    continue;
                }
                const auto* vec = reinterpret_cast<const float*>(sqlite3_column_blob(srcStmt.get(), 0));
                for (int d = 0; d < kDim; ++d) {
                    sourceCentroid[static_cast<size_t>(d)] += vec[d];
                }
                ++sourceChunkCount;
            }
        }
    }

    if (sourceChunkCount == 0) {
        if (outElapsedMs) {
            *outElapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        }
        return results;
    }

    double normSq = 0.0;
    for (float v : sourceCentroid) {
        normSq += static_cast<double>(v) * static_cast<double>(v);
    }
    const float norm = static_cast<float>(std::sqrt(normSq));
    if (norm > 1e-12f) {
        for (float& v : sourceCentroid) {
            v /= norm;
        }
    }

    // Score all other documents in document_chunks
    QHash<qint64, FileSearchResult> candidateMap;
    {
        const char* relSql =
            "SELECT ch.file_id, ch.page_number, ch.snippet_preview, ch.embedding, "
            "       f.path, f.name, f.extension, f.size, f.mtime "
            "FROM document_chunks ch "
            "JOIN files f ON f.id = ch.file_id "
            "WHERE ch.file_id != ?1;";
        database::ScopedStmt relStmt;
        if (sqlite3_prepare_v2(m_readDb, relSql, -1, relStmt.receive(), nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(relStmt.get(), 1, sourceFileId);
            while (sqlite3_step(relStmt.get()) == SQLITE_ROW) {
                if (sqlite3_column_bytes(relStmt.get(), 3) != kExpectedBlobBytes) {
                    continue;
                }
                const auto* vec = reinterpret_cast<const float*>(sqlite3_column_blob(relStmt.get(), 3));
                const float sim = semantic::SemanticEmbeddingEngine::cosineSimilarity(
                    sourceCentroid.data(), vec, kDim);
                if (sim < 0.20f) {
                    continue;
                }

                const qint64 fid = sqlite3_column_int64(relStmt.get(), 0);
                const int pageNum = sqlite3_column_int(relStmt.get(), 1);
                const char* rawPreview = reinterpret_cast<const char*>(sqlite3_column_text(relStmt.get(), 2));
                const QString previewText = rawPreview ? QString::fromUtf8(rawPreview) : QString();

                auto it = candidateMap.find(fid);
                if (it != candidateMap.end()) {
                    if (sim > it->semanticSimilarity) {
                        it->semanticSimilarity = sim;
                        it->score = static_cast<double>(sim) * 100.0;
                        it->snippet = previewText;
                        it->matchedPage = (pageNum > 1) ? pageNum : 0;
                    }
                    continue;
                }

                FileSearchResult item;
                item.id = fid;
                const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(relStmt.get(), 4));
                const char* rawName = reinterpret_cast<const char*>(sqlite3_column_text(relStmt.get(), 5));
                const char* rawExt = reinterpret_cast<const char*>(sqlite3_column_text(relStmt.get(), 6));
                item.size = sqlite3_column_int64(relStmt.get(), 7);
                item.mtime = sqlite3_column_int64(relStmt.get(), 8);
                item.path = rawPath ? QString::fromUtf8(rawPath) : QString();
                item.name = rawName ? QString::fromUtf8(rawName) : QString();
                item.extension = rawExt ? QString::fromUtf8(rawExt) : QString();
                item.parentDirectory = extractParentDir(item.path);
                item.matchType = SearchResultMatchType::Semantic;
                item.snippet = previewText;
                item.matchedPage = (pageNum > 1) ? pageNum : 0;
                item.semanticSimilarity = sim;
                item.score = static_cast<double>(sim) * 100.0;
                candidateMap.insert(fid, std::move(item));
            }
        }
    }

    results.reserve(candidateMap.size());
    for (auto it = candidateMap.begin(); it != candidateMap.end(); ++it) {
        results.append(std::move(it.value()));
    }

    std::sort(results.begin(), results.end(), [](const FileSearchResult& a, const FileSearchResult& b) {
        return a.score > b.score;
    });
    if (results.size() > limit) {
        results.resize(limit);
    }

    if (outElapsedMs) {
        *outElapsedMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
    }
    return results;
}

QString FileSearchEngine::fetchDocumentPreviewText(qint64 fileId,
                                                   const QString& filePath,
                                                   int maxChars) {
    if (maxChars <= 0) {
        return {};
    }

    // 1. Check indexed document_content table first (zero disk I/O beyond SQLite page cache)
    if (fileId > 0) {
        QMutexLocker locker(&m_readMutex);
        if (ensureReadConnectionUnlocked()) {
            database::ScopedStmt stmt;
            if (sqlite3_prepare_v2(m_readDb,
                                   "SELECT SUBSTR(body_text, 1, ?2), page_count "
                                   "FROM document_content WHERE file_id = ?1;",
                                   -1,
                                   stmt.receive(),
                                   nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt.get(), 1, fileId);
                sqlite3_bind_int(stmt.get(), 2, maxChars);
                if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
                    const char* rawText = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
                    const int pages = sqlite3_column_int(stmt.get(), 1);
                    if (rawText != nullptr) {
                        QString body = QString::fromUtf8(rawText).trimmed();
                        if (!body.isEmpty()) {
                            if (pages > 1) {
                                return QStringLiteral("[%1 pages] %2").arg(pages).arg(body);
                            }
                            return body;
                        }
                    }
                }
            }
        }
    }

    // 2. Fallback: fast bounded inspection of the physical file
    if (filePath.isEmpty()) {
        return {};
    }
    const QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile()) {
        return QStringLiteral("File is no longer present on disk.");
    }

    const QString ext = fi.suffix().toLower();
    if (ext == QStringLiteral("png") || ext == QStringLiteral("jpg") ||
        ext == QStringLiteral("jpeg") || ext == QStringLiteral("bmp") ||
        ext == QStringLiteral("ico") || ext == QStringLiteral("svg") ||
        ext == QStringLiteral("webp") || ext == QStringLiteral("gif")) {
        QImageReader reader(filePath);
        const QSize sz = reader.size();
        if (sz.isValid()) {
            return QStringLiteral("Image Document • Resolution: %1 × %2 px • Format: %3")
                .arg(sz.width())
                .arg(sz.height())
                .arg(ext.toUpper());
        }
        return QStringLiteral("Image Document (%1)").arg(ext.toUpper());
    }

    if (ext == QStringLiteral("txt") || ext == QStringLiteral("md") ||
        ext == QStringLiteral("json") || ext == QStringLiteral("csv") ||
        ext == QStringLiteral("log") || ext == QStringLiteral("ini") ||
        ext == QStringLiteral("yaml") || ext == QStringLiteral("yml") ||
        ext == QStringLiteral("xml") || ext == QStringLiteral("cpp") ||
        ext == QStringLiteral("h") || ext == QStringLiteral("py") ||
        ext == QStringLiteral("js") || ext == QStringLiteral("ts")) {
        QFile f(filePath);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray sample = f.read(std::min<qint64>(2048, maxChars * 3));
            f.close();
            if (!sample.contains('\0')) {
                return QString::fromUtf8(sample).trimmed().left(maxChars);
            }
        }
    }

    return QStringLiteral("Binary / Archive File (.%1) • %2 bytes")
        .arg(ext.isEmpty() ? QStringLiteral("file") : ext)
        .arg(fi.size());
}

} // namespace lumen::search
