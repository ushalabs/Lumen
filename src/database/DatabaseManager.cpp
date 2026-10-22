#include "database/DatabaseManager.h"

#include "core/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

#include <sqlite3.h>

namespace lumen::database {

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
        "PRAGMA cache_size = -4096;"; // Capped 4 MB page cache for idle efficiency
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

    const int currentVersion = querySchemaVersionUnlocked();
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
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }

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

QHash<QString, ExistingFileMeta> DatabaseManager::queryExistingFilesUnderRoot(const QString& rootDirectory) const {
    QMutexLocker locker(&m_writeMutex);
    QHash<QString, ExistingFileMeta> result;
    if (m_db == nullptr) {
        return result;
    }

    QString normalizedRoot = QDir::toNativeSeparators(QDir::cleanPath(rootDirectory));
    if (!normalizedRoot.endsWith(QLatin1Char('\\'))) {
        normalizedRoot.append(QLatin1Char('\\'));
    }

    // Use range scan on the UNIQUE path index: path >= 'C:\root\' AND path < 'C:\root]'
    // ('\\' is ASCII 92, ']' is ASCII 93)
    QString upperBound = normalizedRoot;
    upperBound[upperBound.size() - 1] = QLatin1Char(']');

    const char* sql =
        "SELECT id, path, size, mtime FROM files WHERE path >= ?1 AND path < ?2;";
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
        if (rawPath != nullptr) {
            result.insert(QString::fromUtf8(rawPath), meta);
        }
    }
    return result;
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
        "INSERT INTO files(path, name, extension, size, mtime, usn) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6) "
        "ON CONFLICT(path) DO UPDATE SET "
        "    name = excluded.name,"
        "    extension = excluded.extension,"
        "    size = excluded.size,"
        "    mtime = excluded.mtime,"
        "    usn = excluded.usn "
        "WHERE files.mtime != excluded.mtime "
        "   OR files.size != excluded.size "
        "   OR files.name != excluded.name;";

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

    // Delete either exact file match or directory subtree if path was a directory
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
                                 const QString& newExt) {
    QMutexLocker locker(&m_writeMutex);
    if (m_db == nullptr) {
        return false;
    }

    const QString oldNative = QDir::toNativeSeparators(QDir::cleanPath(oldPath));
    const QString newNative = QDir::toNativeSeparators(QDir::cleanPath(newPath));

    if (!executeSqlUnlocked("BEGIN IMMEDIATE TRANSACTION;")) {
        return false;
    }

    // 1. Update exact file match
    {
        ScopedStmt stmt;
        const char* sql =
            "UPDATE files SET path = ?1, name = ?2, extension = ?3 WHERE path = ?4;";
        if (sqlite3_prepare_v2(m_db, sql, -1, stmt.receive(), nullptr) != SQLITE_OK) {
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
        const QByteArray newPathUtf8 = newNative.toUtf8();
        const QByteArray newNameUtf8 = newName.toUtf8();
        const QByteArray newExtUtf8 = newExt.toUtf8();
        const QByteArray oldPathUtf8 = oldNative.toUtf8();

        sqlite3_bind_text(stmt.get(), 1, newPathUtf8.constData(), newPathUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_text(stmt.get(), 2, newNameUtf8.constData(), newNameUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_text(stmt.get(), 3, newExtUtf8.constData(), newExtUtf8.size(), SQLITE_STATIC);
        sqlite3_bind_text(stmt.get(), 4, oldPathUtf8.constData(), oldPathUtf8.size(), SQLITE_STATIC);

        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
            stmt.reset();
            executeSqlUnlocked("ROLLBACK;");
            return false;
        }
    }

    // 2. If a directory was renamed, also update child paths under oldNative + '\'
    QString oldDirPrefix = oldNative;
    if (!oldDirPrefix.endsWith(QLatin1Char('\\'))) {
        oldDirPrefix.append(QLatin1Char('\\'));
    }
    QString newDirPrefix = newNative;
    if (!newDirPrefix.endsWith(QLatin1Char('\\'))) {
        newDirPrefix.append(QLatin1Char('\\'));
    }
    QString oldDirUpper = oldDirPrefix;
    oldDirUpper[oldDirUpper.size() - 1] = QLatin1Char(']');

    {
        ScopedStmt dirStmt;
        const char* dirSql =
            "UPDATE files SET path = ?1 || SUBSTR(path, ?2) WHERE path >= ?3 AND path < ?4;";
        if (sqlite3_prepare_v2(m_db, dirSql, -1, dirStmt.receive(), nullptr) == SQLITE_OK) {
            const QByteArray newPrefixUtf8 = newDirPrefix.toUtf8();
            const QByteArray lowerUtf8 = oldDirPrefix.toUtf8();
            const QByteArray upperUtf8 = oldDirUpper.toUtf8();
            const int substrStart = oldDirPrefix.size() + 1; // SQLite SUBSTR is 1-indexed in characters

            sqlite3_bind_text(dirStmt.get(), 1, newPrefixUtf8.constData(), newPrefixUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_int(dirStmt.get(), 2, substrStart);
            sqlite3_bind_text(dirStmt.get(), 3, lowerUtf8.constData(), lowerUtf8.size(), SQLITE_STATIC);
            sqlite3_bind_text(dirStmt.get(), 4, upperUtf8.constData(), upperUtf8.size(), SQLITE_STATIC);
            sqlite3_step(dirStmt.get());
        }
    }

    if (!executeSqlUnlocked("COMMIT;")) {
        executeSqlUnlocked("ROLLBACK;");
        return false;
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
        "DELETE FROM files;"
        "DELETE FROM indexing_checkpoints;"
        "INSERT INTO files_fts(files_fts) VALUES('rebuild');"
        "COMMIT;";
    return executeSqlUnlocked(sql);
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
    return executeSqlUnlocked("INSERT INTO files_fts(files_fts) VALUES('optimize');");
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
