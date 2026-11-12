#include "database/DatabaseManager.h"

#include "core/Logger.h"
#include "extraction/DocumentExtractor.h"
#include "indexing/IndexingPolicy.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

#include <algorithm>
#include <sqlite3.h>

namespace lumen::database {

namespace {
QString buildPathScopeClause(const QStringList& allowedRoots,
                             const QString& columnExpr,
                             int firstBindIndex,
                             QVector<QByteArray>* outBindings) {
    QStringList rootClauses;
    int bindIdx = firstBindIndex;
    for (const QString& rawRoot : allowedRoots) {
        const QString clean = indexing::IndexingPolicy::normalizeNativePath(rawRoot);
        if (clean.isEmpty()) {
            continue;
        }
        QString prefix = clean;
        if (!prefix.endsWith(QLatin1Char('\\'))) {
            prefix.append(QLatin1Char('\\'));
        }
        QString upper = prefix;
        upper[upper.size() - 1] = QLatin1Char(']');

        outBindings->append(clean.toUtf8());
        const int idxExact = bindIdx++;
        outBindings->append(prefix.toUtf8());
        const int idxLow = bindIdx++;
        outBindings->append(upper.toUtf8());
        const int idxHigh = bindIdx++;

        rootClauses.append(
            QStringLiteral("(%1 = ?%2 COLLATE NOCASE OR (%1 >= ?%3 AND %1 < ?%4))")
                .arg(columnExpr)
                .arg(idxExact)
                .arg(idxLow)
                .arg(idxHigh));
    }

    if (rootClauses.isEmpty()) {
        return QStringLiteral("(0)");
    }
    return QStringLiteral("(") + rootClauses.join(QStringLiteral(" OR ")) + QStringLiteral(")");
}
} // namespace

ScopedStmt::~ScopedStmt() {
    reset();
}

ScopedStmt& ScopedStmt::operator=(ScopedStmt&& other) noexcept {
    if (this != &other) {
        reset();
        m_stmt = other.m_stmt;
        other.m_stmt = nullptr;
    }
    return *this;
}

void ScopedStmt::reset() noexcept {
    if (m_stmt != nullptr) {
        sqlite3_finalize(m_stmt);
        m_stmt = nullptr;
    }
}

DatabaseManager& DatabaseManager::instance() {
    static DatabaseManager s_instance;
    return s_instance;
}

DatabaseManager::~DatabaseManager() {
    close();
}

bool DatabaseManager::initialize(const QString& dbFilePath) {
    QMutexLocker locker(&m_writeMutex);

    if (m_db != nullptr) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }

    m_dbFilePath = QDir::cleanPath(dbFilePath);
    const QFileInfo fi(m_dbFilePath);
    QDir dir = fi.dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        core::Logger::error(QStringLiteral("Database"), QStringLiteral("Failed to create database directory."));
        return false;
    }

    const QByteArray utf8Path = QDir::toNativeSeparators(m_dbFilePath).toUtf8();
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE;
    const int rc = sqlite3_open_v2(utf8Path.constData(), &m_db, flags, nullptr);
    if (rc != SQLITE_OK || m_db == nullptr) {
        const QString errMsg = m_db ? QString::fromUtf8(sqlite3_errmsg(m_db)) : QStringLiteral("Out of memory");
        core::Logger::error(QStringLiteral("Database"),
                            QStringLiteral("sqlite3_open_v2 failed (%1): %2").arg(rc).arg(errMsg));
        if (m_db != nullptr) {
            sqlite3_close_v2(m_db);
            m_db = nullptr;
        }
        return false;
    }

    sqlite3_busy_timeout(m_db, 5000);

    if (!applyPragmasUnlocked()) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
        return false;
    }

    if (!applyMigrationsUnlocked()) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
        return false;
    }

    core::Logger::info(QStringLiteral("Database"),
                       QStringLiteral("Initialized SQLite database (schema v%1) at %2.")
                           .arg(querySchemaVersionUnlocked())
                           .arg(m_dbFilePath));
    return true;
}

void DatabaseManager::close() {
    QMutexLocker locker(&m_writeMutex);
    if (m_db != nullptr) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
        core::Logger::info(QStringLiteral("Database"), QStringLiteral("Closed SQLite database connection."));
    }
}

bool DatabaseManager::isOpen() const {
    QMutexLocker locker(&m_writeMutex);
    return m_db != nullptr;
}

QString DatabaseManager::databaseFilePath() const {
    QMutexLocker locker(&m_writeMutex);
    return m_dbFilePath;
}

bool DatabaseManager::executeSqlUnlocked(const char* sql, QString* outError) {
    if (m_db == nullptr) {
        return false;
    }
    char* rawErr = nullptr;
    const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &rawErr);
    if (rc != SQLITE_OK) {
        const QString errStr = rawErr ? QString::fromUtf8(rawErr) : QString::fromUtf8(sqlite3_errmsg(m_db));
        if (rawErr != nullptr) {
            sqlite3_free(rawErr);
        }
        if (outError != nullptr) {
            *outError = errStr;
        }
        core::Logger::error(QStringLiteral("Database"),
                            QStringLiteral("SQL execution failed (%1): %2").arg(rc).arg(errStr));
        return false;
    }
    return true;
}

bool DatabaseManager::applyPragmasUnlocked() {
    const char* pragmaSql =
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA foreign_keys = ON;"
        "PRAGMA temp_store = MEMORY;"
        "PRAGMA mmap_size = 0;"
        "PRAGMA cache_size = -4096;";
    return executeSqlUnlocked(pragmaSql);
}

int DatabaseManager::querySchemaVersionUnlocked() const {
    if (m_db == nullptr) {
        return 0;
    }

    ScopedStmt checkTableStmt;
    const char* checkSql =
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='schema_migrations' LIMIT 1;";
    if (sqlite3_prepare_v2(m_db, checkSql, -1, checkTableStmt.receive(), nullptr) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(checkTableStmt.get()) != SQLITE_ROW) {
        return 0;
    }

    ScopedStmt verStmt;
    const char* verSql = "SELECT COALESCE(MAX(version), 0) FROM schema_migrations;";
    if (sqlite3_prepare_v2(m_db, verSql, -1, verStmt.receive(), nullptr) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(verStmt.get()) == SQLITE_ROW) {
        return sqlite3_column_int(verStmt.get(), 0);
    }
    return 0;
}

bool DatabaseManager::applyMigrationsUnlocked() {
    const char* bootstrapSql =
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "    version INTEGER PRIMARY KEY NOT NULL,"
        "    applied_at INTEGER NOT NULL,"
        "    description TEXT NOT NULL"
        ");";
    if (!executeSqlUnlocked(bootstrapSql)) {
        return false;
    }

    int currentVersion = querySchemaVersionUnlocked();
    if (currentVersion >= kCurrentSchemaVersion) {
        return true;
    }

    if (currentVersion < 1) {
        const char* migrationV1Sql =
            "BEGIN IMMEDIATE TRANSACTION;"
            "CREATE TABLE IF NOT EXISTS files ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    path TEXT NOT NULL UNIQUE,"
            "    name TEXT NOT NULL,"
            "    extension TEXT NOT NULL DEFAULT '',"
            "    size INTEGER NOT NULL DEFAULT 0,"
            "    mtime INTEGER NOT NULL DEFAULT 0,"
            "    usn INTEGER NOT NULL DEFAULT 0"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_files_name ON files(name COLLATE NOCASE);"
            "CREATE INDEX IF NOT EXISTS idx_files_extension ON files(extension COLLATE NOCASE);"
            "CREATE INDEX IF NOT EXISTS idx_files_mtime ON files(mtime DESC);"
            "CREATE INDEX IF NOT EXISTS idx_files_usn ON files(usn);"
            "CREATE TABLE IF NOT EXISTS indexing_checkpoints ("
            "    root_path TEXT PRIMARY KEY NOT NULL,"
            "    last_usn INTEGER NOT NULL DEFAULT 0,"
            "    last_scan_time INTEGER NOT NULL DEFAULT 0,"
            "    status TEXT NOT NULL DEFAULT 'idle'"
            ");"
            "CREATE VIRTUAL TABLE IF NOT EXISTS files_fts USING fts5("
            "    name,"
            "    extension,"
            "    content='files',"
            "    content_rowid='id',"
            "    tokenize='trigram'"
            ");"
            "CREATE TRIGGER IF NOT EXISTS trg_files_ai AFTER INSERT ON files BEGIN "
            "    INSERT INTO files_fts(rowid, name, extension) VALUES (new.id, new.name, new.extension); "
            "END;"
            "CREATE TRIGGER IF NOT EXISTS trg_files_ad AFTER DELETE ON files BEGIN "
            "    INSERT INTO files_fts(files_fts, rowid, name, extension) VALUES ('delete', old.id, old.name, old.extension); "
            "END;"
            "CREATE TRIGGER IF NOT EXISTS trg_files_au AFTER UPDATE ON files BEGIN "
            "    INSERT INTO files_fts(files_fts, rowid, name, extension) VALUES ('delete', old.id, old.name, old.extension); "
            "    INSERT INTO files_fts(rowid, name, extension) VALUES (new.id, new.name, new.extension); "
            "END;";

        if (!executeSqlUnlocked(migrationV1Sql)) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        ScopedStmt insertVerStmt;
        const char* insertVerSql =
            "INSERT INTO schema_migrations(version, applied_at, description) VALUES (?1, ?2, ?3);";
        if (sqlite3_prepare_v2(m_db, insertVerSql, -1, insertVerStmt.receive(), nullptr) != SQLITE_OK) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        const qint64 nowEpoch = QDateTime::currentSecsSinceEpoch();
        sqlite3_bind_int(insertVerStmt.get(), 1, 1);
        sqlite3_bind_int64(insertVerStmt.get(), 2, nowEpoch);
        sqlite3_bind_text(insertVerStmt.get(), 3, "Phase 0 initial metadata and filename FTS5 schema", -1, SQLITE_STATIC);

        if (sqlite3_step(insertVerStmt.get()) != SQLITE_DONE) {
            stmt_fail_v1:
            insertVerStmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        insertVerStmt.reset();

        if (!executeSqlUnlocked("COMMIT;")) {
            goto stmt_fail_v1;
        }
        currentVersion = 1;
    }

    if (currentVersion < 2) {
        const char* migrationV2Sql =
            "BEGIN IMMEDIATE TRANSACTION;"
            "CREATE TABLE IF NOT EXISTS document_content ("
            "    file_id INTEGER PRIMARY KEY NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
            "    mtime INTEGER NOT NULL DEFAULT 0,"
            "    page_count INTEGER NOT NULL DEFAULT 1,"
            "    char_count INTEGER NOT NULL DEFAULT 0,"
            "    body_text TEXT NOT NULL DEFAULT '',"
            "    page_offsets_json TEXT NOT NULL DEFAULT ''"
            ");"
            "CREATE VIRTUAL TABLE IF NOT EXISTS document_fts USING fts5("
            "    body_text,"
            "    content='document_content',"
            "    content_rowid='file_id',"
            "    tokenize='unicode61 remove_diacritics 2'"
            ");"
            "CREATE TRIGGER IF NOT EXISTS trg_doc_content_ai AFTER INSERT ON document_content BEGIN "
            "    INSERT INTO document_fts(rowid, body_text) VALUES (new.file_id, new.body_text); "
            "END;"
            "CREATE TRIGGER IF NOT EXISTS trg_doc_content_ad AFTER DELETE ON document_content BEGIN "
            "    INSERT INTO document_fts(document_fts, rowid, body_text) VALUES ('delete', old.file_id, old.body_text); "
            "END;"
            "CREATE TRIGGER IF NOT EXISTS trg_doc_content_au AFTER UPDATE ON document_content BEGIN "
            "    INSERT INTO document_fts(document_fts, rowid, body_text) VALUES ('delete', old.file_id, old.body_text); "
            "    INSERT INTO document_fts(rowid, body_text) VALUES (new.file_id, new.body_text); "
            "END;";

        if (!executeSqlUnlocked(migrationV2Sql)) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        ScopedStmt insertVerStmt;
        const char* insertVerSql =
            "INSERT INTO schema_migrations(version, applied_at, description) VALUES (?1, ?2, ?3);";
        if (sqlite3_prepare_v2(m_db, insertVerSql, -1, insertVerStmt.receive(), nullptr) != SQLITE_OK) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        const qint64 nowEpoch = QDateTime::currentSecsSinceEpoch();
        sqlite3_bind_int(insertVerStmt.get(), 1, 2);
        sqlite3_bind_int64(insertVerStmt.get(), 2, nowEpoch);
        sqlite3_bind_text(insertVerStmt.get(),
                          3,
                          "Phase 2 document_content and external-content document_fts schema",
                          -1,
                          SQLITE_STATIC);

        if (sqlite3_step(insertVerStmt.get()) != SQLITE_DONE) {
            insertVerStmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        insertVerStmt.reset();

        if (!executeSqlUnlocked("COMMIT;")) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        currentVersion = 2;
    }

    if (currentVersion < 3) {
        const char* migrationV3Sql =
            "BEGIN IMMEDIATE TRANSACTION;"
            "CREATE TABLE IF NOT EXISTS document_chunks ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
            "    chunk_index INTEGER NOT NULL,"
            "    page_number INTEGER NOT NULL DEFAULT 1,"
            "    mtime INTEGER NOT NULL DEFAULT 0,"
            "    snippet_preview TEXT NOT NULL DEFAULT '',"
            "    embedding BLOB NOT NULL,"
            "    UNIQUE(file_id, chunk_index)"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_document_chunks_file_id ON document_chunks(file_id);";

        if (!executeSqlUnlocked(migrationV3Sql)) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        ScopedStmt insertVerStmt;
        const char* insertVerSql =
            "INSERT INTO schema_migrations(version, applied_at, description) VALUES (?1, ?2, ?3);";
        if (sqlite3_prepare_v2(m_db, insertVerSql, -1, insertVerStmt.receive(), nullptr) != SQLITE_OK) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        const qint64 nowEpoch = QDateTime::currentSecsSinceEpoch();
        sqlite3_bind_int(insertVerStmt.get(), 1, 3);
        sqlite3_bind_int64(insertVerStmt.get(), 2, nowEpoch);
        sqlite3_bind_text(insertVerStmt.get(),
                          3,
                          "Phase 3 document_chunks semantic vector storage schema",
                          -1,
                          SQLITE_STATIC);

        if (sqlite3_step(insertVerStmt.get()) != SQLITE_DONE) {
            insertVerStmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        insertVerStmt.reset();

        if (!executeSqlUnlocked("COMMIT;")) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        currentVersion = 3;
    }

    if (currentVersion < 4) {
        const char* migrationV4Sql =
            "BEGIN IMMEDIATE TRANSACTION;"
            "ALTER TABLE files ADD COLUMN is_dir INTEGER NOT NULL DEFAULT 0;"
            "CREATE INDEX IF NOT EXISTS idx_files_is_dir ON files(is_dir);";

        if (!executeSqlUnlocked(migrationV4Sql)) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        ScopedStmt insertVerStmt;
        const char* insertVerSql =
            "INSERT INTO schema_migrations(version, applied_at, description) VALUES (?1, ?2, ?3);";
        if (sqlite3_prepare_v2(m_db, insertVerSql, -1, insertVerStmt.receive(), nullptr) != SQLITE_OK) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        const qint64 nowEpoch = QDateTime::currentSecsSinceEpoch();
        sqlite3_bind_int(insertVerStmt.get(), 1, 4);
        sqlite3_bind_int64(insertVerStmt.get(), 2, nowEpoch);
        sqlite3_bind_text(insertVerStmt.get(),
                          3,
                          "Phase 5 directory/folder indexing schema (is_dir)",
                          -1,
                          SQLITE_STATIC);

        if (sqlite3_step(insertVerStmt.get()) != SQLITE_DONE) {
            insertVerStmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        insertVerStmt.reset();

        if (!executeSqlUnlocked("COMMIT;")) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        currentVersion = 4;
    }

    if (currentVersion < 5) {
        const char* migrationV5Sql =
            "BEGIN IMMEDIATE TRANSACTION;"
            "CREATE INDEX IF NOT EXISTS idx_files_dir_name ON files(is_dir, name COLLATE NOCASE);";

        if (!executeSqlUnlocked(migrationV5Sql)) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        ScopedStmt insertVerStmt;
        const char* insertVerSql =
            "INSERT INTO schema_migrations(version, applied_at, description) VALUES (?1, ?2, ?3);";
        if (sqlite3_prepare_v2(m_db, insertVerSql, -1, insertVerStmt.receive(), nullptr) != SQLITE_OK) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        const qint64 nowEpoch = QDateTime::currentSecsSinceEpoch();
        sqlite3_bind_int(insertVerStmt.get(), 1, 5);
        sqlite3_bind_int64(insertVerStmt.get(), 2, nowEpoch);
        sqlite3_bind_text(insertVerStmt.get(),
                          3,
                          "Composite (is_dir, name NOCASE) index for instant million-row prefix search",
                          -1,
                          SQLITE_STATIC);

        if (sqlite3_step(insertVerStmt.get()) != SQLITE_DONE) {
            insertVerStmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        insertVerStmt.reset();

        if (!executeSqlUnlocked("COMMIT;")) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        currentVersion = 5;
    }

    if (currentVersion < 6) {
        const char* migrationV6Sql =
            "BEGIN IMMEDIATE TRANSACTION;"
            "CREATE VIRTUAL TABLE IF NOT EXISTS folders_fts USING fts5("
            "    name,"
            "    extension,"
            "    content='files',"
            "    content_rowid='id',"
            "    tokenize='trigram'"
            ");"
            "CREATE TRIGGER IF NOT EXISTS trg_folders_ai AFTER INSERT ON files WHEN new.is_dir = 1 BEGIN "
            "    INSERT INTO folders_fts(rowid, name, extension) VALUES (new.id, new.name, new.extension); "
            "END;"
            "CREATE TRIGGER IF NOT EXISTS trg_folders_ad AFTER DELETE ON files WHEN old.is_dir = 1 BEGIN "
            "    INSERT INTO folders_fts(folders_fts, rowid, name, extension) VALUES ('delete', old.id, old.name, old.extension); "
            "END;"
            "CREATE TRIGGER IF NOT EXISTS trg_folders_au_del AFTER UPDATE ON files WHEN old.is_dir = 1 BEGIN "
            "    INSERT INTO folders_fts(folders_fts, rowid, name, extension) VALUES ('delete', old.id, old.name, old.extension); "
            "END;"
            "CREATE TRIGGER IF NOT EXISTS trg_folders_au_ins AFTER UPDATE ON files WHEN new.is_dir = 1 BEGIN "
            "    INSERT INTO folders_fts(rowid, name, extension) VALUES (new.id, new.name, new.extension); "
            "END;"
            "INSERT INTO folders_fts(rowid, name, extension) SELECT id, name, extension FROM files WHERE is_dir = 1;";

        if (!executeSqlUnlocked(migrationV6Sql)) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        ScopedStmt insertVerStmt;
        const char* insertVerSql =
            "INSERT INTO schema_migrations(version, applied_at, description) VALUES (?1, ?2, ?3);";
        if (sqlite3_prepare_v2(m_db, insertVerSql, -1, insertVerStmt.receive(), nullptr) != SQLITE_OK) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        const qint64 nowEpoch = QDateTime::currentSecsSinceEpoch();
        sqlite3_bind_int(insertVerStmt.get(), 1, 6);
        sqlite3_bind_int64(insertVerStmt.get(), 2, nowEpoch);
        sqlite3_bind_text(insertVerStmt.get(),
                          3,
                          "Dedicated folders_fts trigram index for sub-10ms 1M-row folder search",
                          -1,
                          SQLITE_STATIC);

        if (sqlite3_step(insertVerStmt.get()) != SQLITE_DONE) {
            insertVerStmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        insertVerStmt.reset();

        if (!executeSqlUnlocked("COMMIT;")) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
    }

    return true;
}

int DatabaseManager::schemaVersion() const {
    QMutexLocker locker(&m_writeMutex);
    return querySchemaVersionUnlocked();
}

QString DatabaseManager::journalMode() const {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return {};
    }
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, "PRAGMA journal_mode;", -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return {};
    }
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt.get(), 0);
        return text ? QString::fromUtf8(reinterpret_cast<const char*>(text)).toLower() : QString();
    }
    return {};
}

bool DatabaseManager::tableExists(const QString& tableName) const {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }
    ScopedStmt stmt;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1 LIMIT 1;";
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return false;
    }
    const QByteArray utf8Name = tableName.toUtf8();
    sqlite3_bind_text(stmt.get(), 1, utf8Name.constData(), utf8Name.size(), SQLITE_TRANSIENT);
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

bool DatabaseManager::indexExists(const QString& indexName) const {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }
    ScopedStmt stmt;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?1 LIMIT 1;";
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return false;
    }
    const QByteArray utf8Name = indexName.toUtf8();
    sqlite3_bind_text(stmt.get(), 1, utf8Name.constData(), utf8Name.size(), SQLITE_TRANSIENT);
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

QStringList DatabaseManager::tableColumns(const QString& tableName) const {
    QMutexLocker locker(&m_writeMutex);
    QStringList cols;
    if (m_db == nullptr) {
        return cols;
    }

    for (const QChar c : tableName) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('_')) {
            return cols;
        }
    }

    const QByteArray sql = QStringLiteral("PRAGMA table_info(%1);").arg(tableName).toUtf8();
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, sql.constData(), -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return cols;
    }
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const unsigned char* colName = sqlite3_column_text(stmt.get(), 1);
        if (colName != nullptr) {
            cols.append(QString::fromUtf8(reinterpret_cast<const char*>(colName)));
        }
    }
    return cols;
}

qint64 DatabaseManager::fileCount() const {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return 0;
    }
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM files;", -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return sqlite3_column_int64(stmt.get(), 0);
    }
    return 0;
}

qint64 DatabaseManager::documentContentCount() const {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return 0;
    }
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM document_content;", -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return sqlite3_column_int64(stmt.get(), 0);
    }
    return 0;
}

qint64 DatabaseManager::documentChunkCount() const {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return 0;
    }
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM document_chunks;", -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return sqlite3_column_int64(stmt.get(), 0);
    }
    return 0;
}

qint64 DatabaseManager::semanticEmbeddedFileCount() const {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return 0;
    }
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(DISTINCT file_id) FROM document_chunks;", -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return sqlite3_column_int64(stmt.get(), 0);
    }
    return 0;
}

qint64 DatabaseManager::countExistingFilesUnderRoot(const QString& rootDirectory) const {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return 0;
    }

    QString normalizedRoot = indexing::IndexingPolicy::normalizeNativePath(rootDirectory);
    if (normalizedRoot.isEmpty()) {
        return 0;
    }
    if (!normalizedRoot.endsWith(QLatin1Char('\\'))) {
        normalizedRoot.append(QLatin1Char('\\'));
    }

    QString upperBound = normalizedRoot;
    upperBound[upperBound.size() - 1] = QLatin1Char(']');

    const char* sql = "SELECT COUNT(*) FROM files WHERE path >= ?1 AND path < ?2;";
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return 0;
    }

    const QByteArray lowerUtf8 = normalizedRoot.toUtf8();
    const QByteArray upperUtf8 = upperBound.toUtf8();
    sqlite3_bind_text(stmt.get(), 1, lowerUtf8.constData(), lowerUtf8.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, upperUtf8.constData(), upperUtf8.size(), SQLITE_TRANSIENT);

    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return sqlite3_column_int64(stmt.get(), 0);
    }
    return 0;
}

QStringList DatabaseManager::queryImmediateChildPathsUnderDir(const QString& parentDirectory) const {
    QMutexLocker locker(&m_writeMutex);
    QStringList children;
    if (m_db == nullptr) {
        return children;
    }

    QString dirPrefix = indexing::IndexingPolicy::normalizeNativePath(parentDirectory);
    if (dirPrefix.isEmpty()) {
        return children;
    }
    if (!dirPrefix.endsWith(QLatin1Char('\\'))) {
        dirPrefix.append(QLatin1Char('\\'));
    }
    QString upperBound = dirPrefix;
    upperBound[upperBound.size() - 1] = QLatin1Char(']');

    const char* sql =
        "SELECT path FROM files WHERE path >= ?1 AND path < ?2 ORDER BY path ASC LIMIT 1;";
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return children;
    }

    const QByteArray upperUtf8 = upperBound.toUtf8();
    const int prefixLen = dirPrefix.size();
    QString cursor = dirPrefix;

    // B-tree index-skip scan: jumps past each child's subtree in O(log N) per direct child
    while (children.size() < 50000) {
        const QByteArray cursorUtf8 = cursor.toUtf8();
        sqlite3_bind_text(stmt.get(), 1, cursorUtf8.constData(), cursorUtf8.size(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt.get(), 2, upperUtf8.constData(), upperUtf8.size(), SQLITE_STATIC);

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_ROW) {
            break;
        }
        const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        const QString found = rawPath ? QString::fromUtf8(rawPath) : QString();
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());

        if (found.isEmpty() || found.size() <= prefixLen) {
            break;
        }

        const int nextSep = found.indexOf(QLatin1Char('\\'), prefixLen);
        const QString childPath = (nextSep > prefixLen) ? found.left(nextSep) : found;
        children.append(childPath);

        // Advance cursor past both childPath (file) and childPath\* (subtree: '\' + 1 = ']')
        cursor = childPath + QLatin1Char(']');
    }
    return children;
}

QHash<QString, ExistingFileMeta> DatabaseManager::queryExistingFilesUnderRoot(const QString& rootDirectory) const {
    QMutexLocker locker(&m_writeMutex);
    QHash<QString, ExistingFileMeta> result;
    if (m_db == nullptr) {
        return result;
    }

    QString normalizedRoot = indexing::IndexingPolicy::normalizeNativePath(rootDirectory);
    if (normalizedRoot.isEmpty()) {
        return result;
    }
    if (!normalizedRoot.endsWith(QLatin1Char('\\'))) {
        normalizedRoot.append(QLatin1Char('\\'));
    }

    QString upperBound = normalizedRoot;
    upperBound[upperBound.size() - 1] = QLatin1Char(']');

    const char* sql =
        "SELECT id, path, size, mtime, is_dir FROM files WHERE path >= ?1 AND path < ?2;";
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return result;
    }

    const QByteArray lowerUtf8 = normalizedRoot.toUtf8();
    const QByteArray upperUtf8 = upperBound.toUtf8();
    sqlite3_bind_text(stmt.get(), 1, lowerUtf8.constData(), lowerUtf8.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, upperUtf8.constData(), upperUtf8.size(), SQLITE_TRANSIENT);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        ExistingFileMeta meta;
        meta.id = sqlite3_column_int64(stmt.get(), 0);
        const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        meta.size = sqlite3_column_int64(stmt.get(), 2);
        meta.mtime = sqlite3_column_int64(stmt.get(), 3);
        meta.isDir = (sqlite3_column_int(stmt.get(), 4) != 0);
        if (rawPath != nullptr) {
            result.insert(QString::fromUtf8(rawPath), meta);
        }
    }
    return result;
}

QHash<QString, ExistingFileMeta> DatabaseManager::queryFileMetaByExactPaths(const QStringList& exactPaths) const {
    QMutexLocker locker(&m_writeMutex);
    QHash<QString, ExistingFileMeta> result;
    if (m_db == nullptr || exactPaths.isEmpty()) {
        return result;
    }

    ScopedStmt stmt;
    const char* sql = "SELECT id, size, mtime, is_dir FROM files WHERE path = ?1 LIMIT 1;";
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return result;
    }

    result.reserve(exactPaths.size());
    for (const QString& rawPath : exactPaths) {
        const QString clean = indexing::IndexingPolicy::normalizeNativePath(rawPath);
        if (clean.isEmpty()) {
            continue;
        }
        const QByteArray utf8 = clean.toUtf8();
        sqlite3_bind_text(stmt.get(), 1, utf8.constData(), utf8.size(), SQLITE_STATIC);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            ExistingFileMeta meta;
            meta.id = sqlite3_column_int64(stmt.get(), 0);
            meta.size = sqlite3_column_int64(stmt.get(), 1);
            meta.mtime = sqlite3_column_int64(stmt.get(), 2);
            meta.isDir = (sqlite3_column_int(stmt.get(), 3) != 0);
            result.insert(clean, meta);
        }
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
    }
    return result;
}

std::optional<ExistingFileMeta> DatabaseManager::queryFileMetaByPath(const QString& path) const {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return std::nullopt;
    }

    const QString cleanNative = QDir::toNativeSeparators(QDir::cleanPath(path));
    const QByteArray pathUtf8 = cleanNative.toUtf8();

    {
        ScopedStmt stmt;
        const char* sql = "SELECT id, size, mtime, is_dir FROM files WHERE path = ?1 LIMIT 1;";
        if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt.get(), 1, pathUtf8.constData(), pathUtf8.size(), SQLITE_STATIC);
            if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
                ExistingFileMeta meta;
                meta.id = sqlite3_column_int64(stmt.get(), 0);
                meta.size = sqlite3_column_int64(stmt.get(), 1);
                meta.mtime = sqlite3_column_int64(stmt.get(), 2);
                meta.isDir = (sqlite3_column_int(stmt.get(), 3) != 0);
                return meta;
            }
        }
    }

    return std::nullopt;
}

bool DatabaseManager::upsertFileBatch(const QVector<FileRecord>& records,
                                      const std::atomic_bool* cancelFlag) {
    if (records.isEmpty()) {
        return true;
    }

    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }

    if (!executeSqlUnlocked("BEGIN IMMEDIATE TRANSACTION;")) {
        return false;
    }

    const char* sql =
        "INSERT INTO files(path, name, extension, size, mtime, usn, is_dir) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
        "ON CONFLICT(path) DO UPDATE SET "
        "    name = excluded.name,"
        "    extension = excluded.extension,"
        "    size = excluded.size,"
        "    mtime = excluded.mtime,"
        "    usn = excluded.usn,"
        "    is_dir = excluded.is_dir "
        "WHERE files.mtime != excluded.mtime "
        "   OR files.size != excluded.size "
        "   OR files.name != excluded.name "
        "   OR files.is_dir != excluded.is_dir;";

    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        executeSqlUnlocked("ROLLBACK;");
        return false;
    }

    for (const FileRecord& rec : records) {
        if (cancelFlag != nullptr && cancelFlag->load(std::memory_order_relaxed)) {
            stmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        const QByteArray pathUtf8 = rec.path.toUtf8();
        const QByteArray nameUtf8 = rec.name.toUtf8();
        const QByteArray extUtf8 = rec.extension.toUtf8();

        sqlite3_bind_text(stmt.get(), 1, pathUtf8.constData(), pathUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_text(stmt.get(), 2, nameUtf8.constData(), nameUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_text(stmt.get(), 3, extUtf8.constData(), extUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_int64(stmt.get(), 4, rec.size);
        sqlite3_bind_int64(stmt.get(), 5, rec.mtime);
        sqlite3_bind_int64(stmt.get(), 6, rec.usn);
        sqlite3_bind_int(stmt.get(), 7, rec.isDir ? 1 : 0);

        const int stepRc = sqlite3_step(stmt.get());
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());

        if (stepRc != SQLITE_DONE) {
            stmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
    }

    stmt.reset();
    if (!executeSqlUnlocked("COMMIT;")) {
        executeSqlUnlocked("ROLLBACK;");
        return false;
    }
    return true;
}

bool DatabaseManager::deleteFileBatch(const QStringList& paths) {
    if (paths.isEmpty()) {
        return true;
    }

    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }

    if (!executeSqlUnlocked("BEGIN IMMEDIATE TRANSACTION;")) {
        return false;
    }

    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM files WHERE path = ?1;", -1, stmt.receive(), nullptr) != SQLITE_OK) {
        executeSqlUnlocked("ROLLBACK;");
        return false;
    }

    for (const QString& path : paths) {
        const QByteArray pathUtf8 = path.toUtf8();
        sqlite3_bind_text(stmt.get(), 1, pathUtf8.constData(), pathUtf8.size(), SQLITE_STATIC);
        const int rc = sqlite3_step(stmt.get());
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
        if (rc != SQLITE_DONE) {
            stmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
    }

    stmt.reset();
    if (!executeSqlUnlocked("COMMIT;")) {
        executeSqlUnlocked("ROLLBACK;");
        return false;
    }
    return true;
}

bool DatabaseManager::upsertFile(const FileRecord& record) {
    QVector<FileRecord> singleBatch;
    singleBatch.append(record);
    return upsertFileBatch(singleBatch, nullptr);
}

bool DatabaseManager::removeFileByPath(const QString& path) {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }

    const QString cleanNative = QDir::toNativeSeparators(QDir::cleanPath(path));
    const QByteArray exactUtf8 = cleanNative.toUtf8();

    QString dirPrefix = cleanNative;
    if (!dirPrefix.endsWith(QLatin1Char('\\'))) {
        dirPrefix.append(QLatin1Char('\\'));
    }
    QString dirUpper = dirPrefix;
    dirUpper[dirUpper.size() - 1] = QLatin1Char(']');

    const QByteArray lowerUtf8 = dirPrefix.toUtf8();
    const QByteArray upperUtf8 = dirUpper.toUtf8();

    const char* sql =
        "DELETE FROM files WHERE path = ?1 OR (path >= ?2 AND path < ?3);";
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt.get(), 1, exactUtf8.constData(), exactUtf8.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 2, lowerUtf8.constData(), lowerUtf8.size(), SQLITE_STATIC);
    sqlite3_bind_text(stmt.get(), 3, upperUtf8.constData(), upperUtf8.size(), SQLITE_STATIC);
    return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool DatabaseManager::renamePath(const QString& oldPath,
                                 const QString& newPath,
                                 const QString& newName,
                                 const QString& newExt,
                                 int* outAffectedRows,
                                 bool* outSourceWasDir) {
    if (outAffectedRows != nullptr) {
        *outAffectedRows = 0;
    }
    if (outSourceWasDir != nullptr) {
        *outSourceWasDir = false;
    }

    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }

    const QString oldNative = QDir::toNativeSeparators(QDir::cleanPath(oldPath));
    const QString newNative = QDir::toNativeSeparators(QDir::cleanPath(newPath));

    if (!executeSqlUnlocked("BEGIN IMMEDIATE TRANSACTION;")) {
        return false;
    }

    qint64 sourceId = 0;
    bool sourceIsDir = false;
    QString matchedOldNative = oldNative;
    {
        ScopedStmt findStmt;
        const char* findSql = "SELECT id, is_dir, path FROM files WHERE path = ?1 LIMIT 1;";
        if (sqlite3_prepare_v2(m_db, findSql, -1, findStmt.receive(), nullptr) == SQLITE_OK) {
            const QByteArray oldUtf8 = oldNative.toUtf8();
            sqlite3_bind_text(findStmt.get(), 1, oldUtf8.constData(), oldUtf8.size(), SQLITE_STATIC);
            if (sqlite3_step(findStmt.get()) == SQLITE_ROW) {
                sourceId = sqlite3_column_int64(findStmt.get(), 0);
                sourceIsDir = (sqlite3_column_int(findStmt.get(), 1) != 0);
            }
        }
        // Fallback lookup for NTFS case-insensitive path casing differences
        if (sourceId == 0) {
            ScopedStmt nocaseStmt;
            const char* nocaseSql = "SELECT id, is_dir, path FROM files WHERE path = ?1 COLLATE NOCASE LIMIT 1;";
            if (sqlite3_prepare_v2(m_db, nocaseSql, -1, nocaseStmt.receive(), nullptr) == SQLITE_OK) {
                const QByteArray oldUtf8 = oldNative.toUtf8();
                sqlite3_bind_text(nocaseStmt.get(), 1, oldUtf8.constData(), oldUtf8.size(), SQLITE_STATIC);
                if (sqlite3_step(nocaseStmt.get()) == SQLITE_ROW) {
                    sourceId = sqlite3_column_int64(nocaseStmt.get(), 0);
                    sourceIsDir = (sqlite3_column_int(nocaseStmt.get(), 1) != 0);
                    if (const char* stored = reinterpret_cast<const char*>(sqlite3_column_text(nocaseStmt.get(), 2))) {
                        matchedOldNative = QString::fromUtf8(stored);
                    }
                }
            }
        }
    }

    if (outSourceWasDir != nullptr) {
        *outSourceWasDir = sourceIsDir;
    }

    if (matchedOldNative == newNative) {
        executeSqlUnlocked("COMMIT;");
        return true;
    }

    QString oldDirPrefix = matchedOldNative;
    if (!oldDirPrefix.endsWith(QLatin1Char('\\'))) {
        oldDirPrefix.append(QLatin1Char('\\'));
    }
    QString newDirPrefix = newNative;
    if (!newDirPrefix.endsWith(QLatin1Char('\\'))) {
        newDirPrefix.append(QLatin1Char('\\'));
    }
    QString oldDirUpper = oldDirPrefix;
    oldDirUpper[oldDirUpper.size() - 1] = QLatin1Char(']');

    const QByteArray newPathUtf8 = newNative.toUtf8();
    const QByteArray newNameUtf8 = newName.toUtf8();
    const QString effectiveExt = sourceIsDir ? QString() : newExt;
    const QByteArray newExtUtf8 = effectiveExt.toUtf8();
    const QByteArray oldPathUtf8 = matchedOldNative.toUtf8();
    const QByteArray newPrefixUtf8 = newDirPrefix.toUtf8();
    const QByteArray lowerUtf8 = oldDirPrefix.toUtf8();
    const QByteArray upperUtf8 = oldDirUpper.toUtf8();
    const int substrStart = static_cast<int>(oldDirPrefix.toUcs4().size()) + 1;

    // Remove any stale colliding destination row before moving source rows so UNIQUE(path) never fails
    // while preserving the renamed item's rowid (and cached content/chunks FK).
    if (sourceId != 0) {
        ScopedStmt delCollisionStmt;
        const char* delCollisionSql = "DELETE FROM files WHERE path = ?1 AND id != ?2;";
        if (sqlite3_prepare_v2(m_db, delCollisionSql, -1, delCollisionStmt.receive(), nullptr) == SQLITE_OK) {
            sqlite3_bind_text(delCollisionStmt.get(), 1, newPathUtf8.constData(), newPathUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_int64(delCollisionStmt.get(), 2, sourceId);
            sqlite3_step(delCollisionStmt.get());
        }
    }

    {
        ScopedStmt delChildCollisionStmt;
        const char* delChildCollisionSql =
            "DELETE FROM files "
            "WHERE path IN ("
            "    SELECT ?1 || SUBSTR(src.path, ?2) FROM files src WHERE src.path >= ?3 AND src.path < ?4"
            ") AND NOT (path >= ?3 AND path < ?4);";
        if (sqlite3_prepare_v2(m_db, delChildCollisionSql, -1, delChildCollisionStmt.receive(), nullptr) == SQLITE_OK) {
            sqlite3_bind_text(delChildCollisionStmt.get(), 1, newPrefixUtf8.constData(), newPrefixUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_int(delChildCollisionStmt.get(), 2, substrStart);
            sqlite3_bind_text(delChildCollisionStmt.get(), 3, lowerUtf8.constData(), lowerUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_text(delChildCollisionStmt.get(), 4, upperUtf8.constData(), upperUtf8.size(), SQLITE_STATIC);
            sqlite3_step(delChildCollisionStmt.get());
        }
    }

    int affectedRows = 0;

    {
        ScopedStmt stmt;
        const char* sql =
            "UPDATE files SET path = ?1, name = ?2, extension = ?3 WHERE path = ?4;";
        if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        sqlite3_bind_text(stmt.get(), 1, newPathUtf8.constData(), newPathUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_text(stmt.get(), 2, newNameUtf8.constData(), newNameUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_text(stmt.get(), 3, newExtUtf8.constData(), newExtUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_text(stmt.get(), 4, oldPathUtf8.constData(), oldPathUtf8.size(), SQLITE_STATIC);

        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            stmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        affectedRows += sqlite3_changes(m_db);
    }

    {
        ScopedStmt dirStmt;
        const char* dirSql =
            "UPDATE files SET path = ?1 || SUBSTR(path, ?2) WHERE path >= ?3 AND path < ?4;";
        if (sqlite3_prepare_v2(m_db, dirSql, -1, dirStmt.receive(), nullptr) != SQLITE_OK) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        sqlite3_bind_text(dirStmt.get(), 1, newPrefixUtf8.constData(), newPrefixUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_int(dirStmt.get(), 2, substrStart);
        sqlite3_bind_text(dirStmt.get(), 3, lowerUtf8.constData(), lowerUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_text(dirStmt.get(), 4, upperUtf8.constData(), upperUtf8.size(), SQLITE_STATIC);
        if (sqlite3_step(dirStmt.get()) != SQLITE_DONE) {
            dirStmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        affectedRows += sqlite3_changes(m_db);
    }

    if (!executeSqlUnlocked("COMMIT;")) {
        executeSqlUnlocked("ROLLBACK;");
        return false;
    }

    if (outAffectedRows != nullptr) {
        *outAffectedRows = affectedRows;
    }
    return true;
}

bool DatabaseManager::clearAllFiles() {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }
    const char* sql =
        "BEGIN IMMEDIATE TRANSACTION;"
        "DELETE FROM document_chunks;"
        "DELETE FROM document_content;"
        "DELETE FROM files;"
        "DELETE FROM indexing_checkpoints;"
        "INSERT INTO files_fts(files_fts) VALUES('rebuild');"
        "INSERT INTO folders_fts(folders_fts) VALUES('rebuild');"
        "INSERT INTO document_fts(document_fts) VALUES('rebuild');"
        "COMMIT;";
    return executeSqlUnlocked(sql);
}

QVector<ContentExtractionCandidate> DatabaseManager::queryCandidatesForContentExtraction(
    int maxTotalContentFiles,
    qint64 maxFileSizeBytes,
    const QStringList& allowedRoots,
    bool restrictToAllowedRoots) const {
    QMutexLocker locker(&m_writeMutex);
    QVector<ContentExtractionCandidate> candidates;
    if (m_db == nullptr || maxTotalContentFiles <= 0) {
        return candidates;
    }
    if (restrictToAllowedRoots && allowedRoots.isEmpty()) {
        return candidates;
    }

    QVector<QByteArray> countBindings;
    const QString countScopeSql = restrictToAllowedRoots
                                      ? (QStringLiteral(" AND ") + buildPathScopeClause(allowedRoots, QStringLiteral("f.path"), 1, &countBindings))
                                      : QString();

    // Count how many valid up-to-date documents inside scope are already content-indexed
    int existingValidCount = 0;
    {
        const QByteArray countSqlUtf8 = QStringLiteral(
            "SELECT COUNT(*) FROM document_content dc "
            "JOIN files f ON f.id = dc.file_id "
            "WHERE dc.mtime = f.mtime%1;")
                                            .arg(countScopeSql)
                                            .toUtf8();
        ScopedStmt countStmt;
        if (sqlite3_prepare_v2(m_db, countSqlUtf8.constData(), -1, countStmt.receive(), nullptr) == SQLITE_OK) {
            for (int i = 0; i < countBindings.size(); ++i) {
                sqlite3_bind_text(countStmt.get(), i + 1, countBindings[i].constData(), countBindings[i].size(), SQLITE_STATIC);
            }
            if (sqlite3_step(countStmt.get()) == SQLITE_ROW) {
                existingValidCount = sqlite3_column_int(countStmt.get(), 0);
            }
        }
    }

    const int remainingBudget = std::max(0, maxTotalContentFiles - existingValidCount);
    if (remainingBudget == 0) {
        return candidates;
    }

    QStringList docAndCodeExts;
    QStringList mediaExts;
    for (const QString& ext : extraction::DocumentExtractor::allContentSupportedExtensions()) {
        const QString quoted = QStringLiteral("'%1'").arg(ext);
        if (extraction::DocumentExtractor::isMediaMetadataExtension(ext)) {
            mediaExts.append(quoted);
        } else {
            docAndCodeExts.append(quoted);
        }
    }

    QVector<QByteArray> scopeBindings;
    const QString scopeSql = restrictToAllowedRoots
                                 ? (QStringLiteral("  AND ") + buildPathScopeClause(allowedRoots, QStringLiteral("f.path"), 3, &scopeBindings) + QLatin1Char('\n'))
                                 : QString();

    const QString sqlQuery = QStringLiteral(
        "SELECT f.id, f.path, f.extension, f.size, f.mtime "
        "FROM files f "
        "LEFT JOIN document_content dc ON dc.file_id = f.id "
        "WHERE f.is_dir = 0 "
        "  AND ((f.extension IN (%1) COLLATE NOCASE AND f.size > 0 AND f.size <= ?1) "
        "   OR (f.extension IN (%2) COLLATE NOCASE AND f.size > 0)) "
        "  AND (dc.file_id IS NULL OR dc.mtime != f.mtime) "
        "%3"
        "ORDER BY f.mtime DESC "
        "LIMIT ?2;")
                                 .arg(docAndCodeExts.join(QStringLiteral(", ")),
                                      mediaExts.join(QStringLiteral(", ")),
                                      scopeSql);
    const QByteArray sqlUtf8 = sqlQuery.toUtf8();
    const char* sql = sqlUtf8.constData();

    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return candidates;
    }

    sqlite3_bind_int64(stmt.get(), 1, maxFileSizeBytes);
    sqlite3_bind_int(stmt.get(), 2, remainingBudget);
    for (int i = 0; i < scopeBindings.size(); ++i) {
        sqlite3_bind_text(stmt.get(), 3 + i, scopeBindings[i].constData(), scopeBindings[i].size(), SQLITE_STATIC);
    }

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        ContentExtractionCandidate cand;
        cand.fileId = sqlite3_column_int64(stmt.get(), 0);
        const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        const char* rawExt = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        cand.size = sqlite3_column_int64(stmt.get(), 3);
        cand.mtime = sqlite3_column_int64(stmt.get(), 4);
        cand.path = rawPath ? QString::fromUtf8(rawPath) : QString();
        cand.extension = rawExt ? QString::fromUtf8(rawExt).toLower() : QString();
        if (restrictToAllowedRoots && !indexing::IndexingPolicy::isPathUnderAnyRoot(cand.path, allowedRoots)) {
            continue;
        }
        candidates.append(std::move(cand));
    }
    return candidates;
}

bool DatabaseManager::upsertDocumentContentBatch(const QVector<DocumentContentRecord>& records,
                                                 const std::atomic_bool* cancelFlag) {
    if (records.isEmpty()) {
        return true;
    }

    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }

    if (!executeSqlUnlocked("BEGIN IMMEDIATE TRANSACTION;")) {
        return false;
    }

    const char* sql =
        "INSERT INTO document_content(file_id, mtime, page_count, char_count, body_text, page_offsets_json) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(file_id) DO UPDATE SET "
        "    mtime = excluded.mtime,"
        "    page_count = excluded.page_count,"
        "    char_count = excluded.char_count,"
        "    body_text = excluded.body_text,"
        "    page_offsets_json = excluded.page_offsets_json "
        "WHERE document_content.mtime != excluded.mtime "
        "   OR document_content.char_count != excluded.char_count;";

    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        executeSqlUnlocked("ROLLBACK;");
        return false;
    }

    for (const DocumentContentRecord& rec : records) {
        if (cancelFlag != nullptr && cancelFlag->load(std::memory_order_relaxed)) {
            stmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        const QByteArray bodyUtf8 = rec.bodyText.toUtf8();
        const QByteArray offsetsUtf8 = rec.pageOffsetsJson.toUtf8();

        sqlite3_bind_int64(stmt.get(), 1, rec.fileId);
        sqlite3_bind_int64(stmt.get(), 2, rec.mtime);
        sqlite3_bind_int(stmt.get(), 3, rec.pageCount);
        sqlite3_bind_int(stmt.get(), 4, rec.charCount);
        sqlite3_bind_text(stmt.get(), 5, bodyUtf8.constData(), bodyUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_text(stmt.get(), 6, offsetsUtf8.constData(), offsetsUtf8.size(), SQLITE_STATIC);

        const int rc = sqlite3_step(stmt.get());
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());

        if (rc != SQLITE_DONE) {
            stmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
    }

    stmt.reset();
    if (!executeSqlUnlocked("COMMIT;")) {
        executeSqlUnlocked("ROLLBACK;");
        return false;
    }
    return true;
}

bool DatabaseManager::upsertDocumentContent(const DocumentContentRecord& record) {
    QVector<DocumentContentRecord> batch;
    batch.append(record);
    return upsertDocumentContentBatch(batch, nullptr);
}

bool DatabaseManager::optimizeDocumentFtsIndex() {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }
    return executeSqlUnlocked("INSERT INTO document_fts(document_fts) VALUES('optimize');");
}

QVector<SemanticExtractionCandidate> DatabaseManager::queryCandidatesForSemanticIndexing(
    int maxTotalSemanticFiles,
    const QStringList& allowedRoots,
    bool restrictToAllowedRoots) const {
    QMutexLocker locker(&m_writeMutex);
    QVector<SemanticExtractionCandidate> candidates;
    if (m_db == nullptr || maxTotalSemanticFiles <= 0) {
        return candidates;
    }
    if (restrictToAllowedRoots && allowedRoots.isEmpty()) {
        return candidates;
    }

    QVector<QByteArray> countBindings;
    const QString countScopeSql = restrictToAllowedRoots
                                      ? (QStringLiteral(" AND ") + buildPathScopeClause(allowedRoots, QStringLiteral("f.path"), 1, &countBindings))
                                      : QString();

    int existingValidCount = 0;
    {
        const QByteArray countSqlUtf8 = QStringLiteral(
            "SELECT COUNT(DISTINCT ch.file_id) FROM document_chunks ch "
            "JOIN document_content dc ON dc.file_id = ch.file_id "
            "JOIN files f ON f.id = ch.file_id "
            "WHERE ch.mtime = dc.mtime%1;")
                                            .arg(countScopeSql)
                                            .toUtf8();
        ScopedStmt countStmt;
        if (sqlite3_prepare_v2(m_db, countSqlUtf8.constData(), -1, countStmt.receive(), nullptr) == SQLITE_OK) {
            for (int i = 0; i < countBindings.size(); ++i) {
                sqlite3_bind_text(countStmt.get(), i + 1, countBindings[i].constData(), countBindings[i].size(), SQLITE_STATIC);
            }
            if (sqlite3_step(countStmt.get()) == SQLITE_ROW) {
                existingValidCount = sqlite3_column_int(countStmt.get(), 0);
            }
        }
    }

    const int remainingBudget = std::max(0, maxTotalSemanticFiles - existingValidCount);
    if (remainingBudget == 0) {
        return candidates;
    }

    QVector<QByteArray> scopeBindings;
    const QString scopeSql = restrictToAllowedRoots
                                 ? (QStringLiteral("  AND ") + buildPathScopeClause(allowedRoots, QStringLiteral("f.path"), 2, &scopeBindings) + QLatin1Char('\n'))
                                 : QString();

    const QByteArray sqlUtf8 = QStringLiteral(
        "SELECT dc.file_id, dc.mtime, dc.body_text, dc.page_offsets_json, f.path "
        "FROM document_content dc "
        "JOIN files f ON f.id = dc.file_id "
        "LEFT JOIN document_chunks ch ON ch.file_id = dc.file_id AND ch.chunk_index = 0 "
        "WHERE dc.char_count > 0 "
        "  AND (ch.id IS NULL OR ch.mtime != dc.mtime) "
        "%1"
        "ORDER BY dc.mtime DESC "
        "LIMIT ?1;")
                                   .arg(scopeSql)
                                   .toUtf8();

    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, sqlUtf8.constData(), -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return candidates;
    }
    sqlite3_bind_int(stmt.get(), 1, remainingBudget);
    for (int i = 0; i < scopeBindings.size(); ++i) {
        sqlite3_bind_text(stmt.get(), 2 + i, scopeBindings[i].constData(), scopeBindings[i].size(), SQLITE_STATIC);
    }

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
        const QString filePath = rawPath ? QString::fromUtf8(rawPath) : QString();
        if (restrictToAllowedRoots && !indexing::IndexingPolicy::isPathUnderAnyRoot(filePath, allowedRoots)) {
            continue;
        }
        SemanticExtractionCandidate cand;
        cand.fileId = sqlite3_column_int64(stmt.get(), 0);
        cand.mtime = sqlite3_column_int64(stmt.get(), 1);
        const char* rawBody = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
        const char* rawPages = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
        cand.bodyText = rawBody ? QString::fromUtf8(rawBody) : QString();
        cand.pageOffsetsJson = rawPages ? QString::fromUtf8(rawPages) : QString();
        candidates.append(std::move(cand));
    }
    return candidates;
}

int DatabaseManager::pruneContentAndSemanticOutsideRoots(const QStringList& allowedRoots) {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return 0;
    }

    QVector<qint64> toDeleteIds;
    {
        ScopedStmt stmt;
        const char* sql =
            "SELECT DISTINCT dc.file_id, f.path "
            "FROM document_content dc "
            "JOIN files f ON f.id = dc.file_id;";
        if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
            return 0;
        }
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const qint64 fid = sqlite3_column_int64(stmt.get(), 0);
            const char* rawPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
            const QString filePath = rawPath ? QString::fromUtf8(rawPath) : QString();
            if (allowedRoots.isEmpty() || !indexing::IndexingPolicy::isPathUnderAnyRoot(filePath, allowedRoots)) {
                toDeleteIds.append(fid);
            }
        }
    }

    if (toDeleteIds.isEmpty()) {
        return 0;
    }

    if (!executeSqlUnlocked("BEGIN IMMEDIATE TRANSACTION;")) {
        return 0;
    }

    ScopedStmt delChunks;
    ScopedStmt delContent;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM document_chunks WHERE file_id = ?1;", -1, delChunks.receive(), nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, "DELETE FROM document_content WHERE file_id = ?1;", -1, delContent.receive(), nullptr) != SQLITE_OK) {
        executeSqlUnlocked("ROLLBACK;");
        return 0;
    }

    for (qint64 fid : toDeleteIds) {
        sqlite3_bind_int64(delChunks.get(), 1, fid);
        sqlite3_step(delChunks.get());
        sqlite3_reset(delChunks.get());

        sqlite3_bind_int64(delContent.get(), 1, fid);
        sqlite3_step(delContent.get());
        sqlite3_reset(delContent.get());
    }

    if (!executeSqlUnlocked("COMMIT;")) {
        executeSqlUnlocked("ROLLBACK;");
        return 0;
    }
    return static_cast<int>(toDeleteIds.size());
}

bool DatabaseManager::replaceDocumentChunksBatch(const QVector<qint64>& fileIdsToClear,
                                                 const QVector<DocumentChunkRecord>& records,
                                                 const std::atomic_bool* cancelFlag) {
    if (fileIdsToClear.isEmpty() && records.isEmpty()) {
        return true;
    }

    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }

    if (!executeSqlUnlocked("BEGIN IMMEDIATE TRANSACTION;")) {
        return false;
    }

    if (!fileIdsToClear.isEmpty()) {
        ScopedStmt delStmt;
        if (sqlite3_prepare_v2(m_db, "DELETE FROM document_chunks WHERE file_id = ?1;", -1, delStmt.receive(), nullptr) != SQLITE_OK) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        for (qint64 fid : fileIdsToClear) {
            sqlite3_bind_int64(delStmt.get(), 1, fid);
            const int rc = sqlite3_step(delStmt.get());
            sqlite3_reset(delStmt.get());
            if (rc != SQLITE_DONE) {
                delStmt.reset();
                executeSqlUnlocked("ROLLBACK;");
                return false;
            }
        }
    }

    if (!records.isEmpty()) {
        const char* insSql =
            "INSERT INTO document_chunks(file_id, chunk_index, page_number, mtime, snippet_preview, embedding) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6);";
        ScopedStmt insStmt;
        if (sqlite3_prepare_v2(m_db, insSql, -1, insStmt.receive(), nullptr) != SQLITE_OK) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

        for (const DocumentChunkRecord& rec : records) {
            if (cancelFlag != nullptr && cancelFlag->load(std::memory_order_relaxed)) {
                insStmt.reset();
                executeSqlUnlocked("ROLLBACK;");
                return false;
            }

            const QByteArray previewUtf8 = rec.snippetPreview.toUtf8();
            sqlite3_bind_int64(insStmt.get(), 1, rec.fileId);
            sqlite3_bind_int(insStmt.get(), 2, rec.chunkIndex);
            sqlite3_bind_int(insStmt.get(), 3, rec.pageNumber);
            sqlite3_bind_int64(insStmt.get(), 4, rec.mtime);
            sqlite3_bind_text(insStmt.get(), 5, previewUtf8.constData(), previewUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_blob(insStmt.get(), 6, rec.embeddingBlob.constData(), rec.embeddingBlob.size(), SQLITE_STATIC);

            const int rc = sqlite3_step(insStmt.get());
            sqlite3_reset(insStmt.get());
            sqlite3_clear_bindings(insStmt.get());

            if (rc != SQLITE_DONE) {
                insStmt.reset();
                executeSqlUnlocked("ROLLBACK;");
                return false;
            }
        }
    }

    if (!executeSqlUnlocked("COMMIT;")) {
        executeSqlUnlocked("ROLLBACK;");
        return false;
    }
    return true;
}

bool DatabaseManager::updateCheckpoint(const QString& rootPath,
                                       qint64 lastUsn,
                                       qint64 lastScanTime,
                                       const QString& status) {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }

    const char* sql =
        "INSERT INTO indexing_checkpoints(root_path, last_usn, last_scan_time, status) "
        "VALUES (?1, ?2, ?3, ?4) "
        "ON CONFLICT(root_path) DO UPDATE SET "
        "    last_usn = excluded.last_usn,"
        "    last_scan_time = excluded.last_scan_time,"
        "    status = excluded.status;";
    ScopedStmt stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return false;
    }

    const QByteArray rootUtf8 = QDir::toNativeSeparators(QDir::cleanPath(rootPath)).toUtf8();
    const QByteArray statusUtf8 = status.toUtf8();

    sqlite3_bind_text(stmt.get(), 1, rootUtf8.constData(), rootUtf8.size(), SQLITE_STATIC);
    sqlite3_bind_int64(stmt.get(), 2, lastUsn);
    sqlite3_bind_int64(stmt.get(), 3, lastScanTime);
    sqlite3_bind_text(stmt.get(), 4, statusUtf8.constData(), statusUtf8.size(), SQLITE_STATIC);
    return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

qint64 DatabaseManager::queryLastScanTime(const QString& rootPath) const {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return 0;
    }

    ScopedStmt stmt;
    const char* sql = "SELECT last_scan_time FROM indexing_checkpoints WHERE root_path = ?1 LIMIT 1;";
    if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
        return 0;
    }

    const QByteArray rootUtf8 = QDir::toNativeSeparators(QDir::cleanPath(rootPath)).toUtf8();
    sqlite3_bind_text(stmt.get(), 1, rootUtf8.constData(), rootUtf8.size(), SQLITE_STATIC);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        return sqlite3_column_int64(stmt.get(), 0);
    }
    return 0;
}

bool DatabaseManager::optimizeFtsIndex() {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }
    return executeSqlUnlocked(
        "INSERT INTO files_fts(files_fts) VALUES('optimize');"
        "INSERT INTO folders_fts(folders_fts) VALUES('optimize');");
}

bool DatabaseManager::walCheckpointTruncate() {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }
    int logFrames = 0;
    int checkpointedFrames = 0;
    const int rc = sqlite3_wal_checkpoint_v2(
        m_db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, &logFrames, &checkpointedFrames);
    return rc == SQLITE_OK;
}

} // namespace lumen::database
