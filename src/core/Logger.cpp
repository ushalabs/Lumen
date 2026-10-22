#include "core/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>

namespace lumen::core {

Logger& Logger::instance() {
    static Logger s_instance;
    return s_instance;
}

Logger::~Logger() {
    shutdown();
}

bool Logger::initialize(const QString& logFilePath, qint64 maxFileSizeBytes, int maxBackupFiles) {
    QMutexLocker locker(&m_mutex);

    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }

    m_logFilePath = logFilePath;
    m_maxFileSizeBytes = (maxFileSizeBytes > 1024) ? maxFileSizeBytes : 1024;
    m_maxBackupFiles = (maxBackupFiles >= 1) ? maxBackupFiles : 1;

    const QFileInfo fi(m_logFilePath);
    QDir dir = fi.dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        m_initialized = false;
        return false;
    }

    m_file.setFileName(m_logFilePath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_initialized = false;
        return false;
    }

    m_initialized = true;
    return true;
}

void Logger::shutdown() {
    QMutexLocker locker(&m_mutex);
    if (m_file.isOpen()) {
        m_file.flush();
        m_file.close();
    }
    m_initialized = false;
}

void Logger::rotateIfNeededUnlocked() {
    if (!m_file.isOpen() || m_file.size() < m_maxFileSizeBytes) {
        return;
    }

    m_file.flush();
    m_file.close();

    // Remove oldest rotated log if it exists
    const QString oldestPath = QStringLiteral("%1.%2").arg(m_logFilePath).arg(m_maxBackupFiles);
    if (QFile::exists(oldestPath)) {
        QFile::remove(oldestPath);
    }

    // Shift existing backup files: .(N-1) -> .N
    for (int i = m_maxBackupFiles - 1; i >= 1; --i) {
        const QString src = QStringLiteral("%1.%2").arg(m_logFilePath).arg(i);
        const QString dst = QStringLiteral("%1.%2").arg(m_logFilePath).arg(i + 1);
        if (QFile::exists(src)) {
            QFile::rename(src, dst);
        }
    }

    // Rename active log to .1
    const QString firstBackup = QStringLiteral("%1.1").arg(m_logFilePath);
    QFile::rename(m_logFilePath, firstBackup);

    // Reopen fresh primary log file
    m_file.setFileName(m_logFilePath);
    m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
}

void Logger::log(LogLevel level, const QString& category, const QString& message) {
    QMutexLocker locker(&m_mutex);
    if (!m_initialized || !m_file.isOpen()) {
        return;
    }

    rotateIfNeededUnlocked();
    if (!m_file.isOpen()) {
        return;
    }

    const char* levelStr = "INFO";
    switch (level) {
        case LogLevel::Debug:   levelStr = "DEBUG"; break;
        case LogLevel::Info:    levelStr = "INFO"; break;
        case LogLevel::Warning: levelStr = "WARN"; break;
        case LogLevel::Error:   levelStr = "ERROR"; break;
    }

    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QString line = QStringLiteral("[%1] [%2] [%3] %4\n")
                             .arg(timestamp, QLatin1String(levelStr), category, message);

    const QByteArray utf8 = line.toUtf8();
    m_file.write(utf8);
    m_file.flush();
}

void Logger::info(const QString& category, const QString& message) {
    instance().log(LogLevel::Info, category, message);
}

void Logger::warning(const QString& category, const QString& message) {
    instance().log(LogLevel::Warning, category, message);
}

void Logger::error(const QString& category, const QString& message) {
    instance().log(LogLevel::Error, category, message);
}

void Logger::debug(const QString& category, const QString& message) {
    instance().log(LogLevel::Debug, category, message);
}

QString Logger::currentLogFilePath() const {
    QMutexLocker locker(&m_mutex);
    return m_logFilePath;
}

qint64 Logger::maxFileSizeBytes() const {
    QMutexLocker locker(&m_mutex);
    return m_maxFileSizeBytes;
}

} // namespace lumen::core
