#pragma once

#include <QFile>
#include <QMutex>
#include <QString>

namespace lumen::core {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3
};

class Logger {
public:
    static Logger& instance();

    bool initialize(const QString& logFilePath,
                    qint64 maxFileSizeBytes = 1024 * 1024,
                    int maxBackupFiles = 2);
    void shutdown();

    void log(LogLevel level, const QString& category, const QString& message);

    static void info(const QString& category, const QString& message);
    static void warning(const QString& category, const QString& message);
    static void error(const QString& category, const QString& message);
    static void debug(const QString& category, const QString& message);

    QString currentLogFilePath() const;
    qint64 maxFileSizeBytes() const;

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void rotateIfNeededUnlocked();

    mutable QMutex m_mutex;
    QFile m_file;
    QString m_logFilePath;
    qint64 m_maxFileSizeBytes = 1024 * 1024;
    int m_maxBackupFiles = 2;
    bool m_initialized = false;
};

} // namespace lumen::core
