#include "search/FileSearchEngine.h"

#include "core/AppPaths.h"
#include "core/Logger.h"
#include "database/DatabaseManager.h"
#include "extraction/DocumentExtractor.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QMutexLocker>
#include <QRegularExpression>

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
    QStringList queryTokensLower;
    QStringList ftsTrigTokens;
    QStringList ftsDocTokens;

    const QStringList rawParts = trimmedQuery.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& part : rawParts) {
        if (part.startsWith(QStringLiteral("ext:"), Qt::CaseInsensitive) && part.size() > 4) {
            QString ext = part.mid(4).trimmed().toLower();
            if (ext.startsWith(QLatin1Char('.'))) {
                ext.remove(0, 1);
            }
            if (!ext.isEmpty()) {
                activeFilters.extensions.append(ext);
            }
            continue;
        }
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

    const QString effectiveQueryLower = queryTokensLower.isEmpty()
                                            ? trimmedQuery.toLower()
                                            : queryTokensLower.join(QLatin1Char(' '));

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
    if (!firstToken.isEmpty()) {
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
    if (!ftsTrigTokens.isEmpty() &&
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
    if (queryTokensLower.isEmpty() && !activeFilters.extensions.isEmpty() &&
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

    sqlite3_progress_handler(m_readDb, 0, nullptr, nullptr);

    if (cancellationToken && cancellationToken->load(std::memory_order_relaxed)) {
        return {};
    }

    finalResults.reserve(candidateMap.size());
    for (auto it = candidateMap.begin(); it != candidateMap.end(); ++it) {
        finalResults.append(std::move(it.value()));
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

} // namespace lumen::search
