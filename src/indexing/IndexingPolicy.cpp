#include "indexing/IndexingPolicy.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <array>

namespace lumen::indexing {

namespace {

void appendUniqueNormalizedRoot(QStringList* dest, const QString& rawPath) {
    const QString norm = IndexingPolicy::normalizeNativePath(rawPath);
    if (norm.isEmpty()) {
        return;
    }
    for (const QString& existing : *dest) {
        if (existing.compare(norm, Qt::CaseInsensitive) == 0) {
            return;
        }
    }
    dest->append(norm);
}

bool containsInsensitive(const QString& target, std::initializer_list<const wchar_t*> candidates) {
    for (const wchar_t* c : candidates) {
        if (target.compare(QStringView(c), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool isDriveRootSystemDirName(const QString& lowerName) {
    return containsInsensitive(lowerName, {
        L"windows",
        L"windows.old",
        L"programdata",
        L"recovery",
        L"perflogs",
        L"$recycle.bin",
        L"system volume information",
        L"config.msi",
        L"msocache",
        L"$winreagent",
        L"$windows.~bt",
        L"$windows.~ws",
        L"$sysreset",
        L"onedrivetemp",
        L"boot",
        L"efi"
    });
}

bool isDriveRootSystemFileName(const QString& lowerName) {
    return containsInsensitive(lowerName, {
        L"hiberfil.sys",
        L"pagefile.sys",
        L"swapfile.sys",
        L"dumpstack.log",
        L"dumpstack.log.tmp",
        L"bootmgr",
        L"bootnxt",
        L"bootsect.bak"
    });
}

bool isProgramFilesProtectedSubdir(const QString& parentLower, const QString& dirNameLower) {
    if (!parentLower.endsWith(QStringLiteral("\\program files")) &&
        !parentLower.endsWith(QStringLiteral("\\program files (x86)"))) {
        return false;
    }
    return containsInsensitive(dirNameLower, {
        L"windowsapps",
        L"modifiablewindowsapps",
        L"windows defender",
        L"windows defender advanced threat protection"
    });
}

bool isSensitiveCredentialFile(const QString& fileNameLower) {
    return fileNameLower == QStringLiteral(".env") ||
           fileNameLower.startsWith(QStringLiteral(".env.")) ||
           fileNameLower.startsWith(QStringLiteral("id_rsa")) ||
           fileNameLower.startsWith(QStringLiteral("id_ed25519")) ||
           fileNameLower.endsWith(QStringLiteral(".pem")) ||
           fileNameLower.endsWith(QStringLiteral(".key")) ||
           fileNameLower.endsWith(QStringLiteral(".pfx"));
}

bool isUserProfileDirectoryPath(const QString& cleanNativeDir) {
    // Matches X:\Users\<username>
    const QString norm = IndexingPolicy::normalizeNativePath(cleanNativeDir);
    if (norm.size() < 10 || norm.at(1) != QLatin1Char(':')) {
        return false;
    }
    const QString afterDrive = norm.mid(3);
    const QStringList parts = afterDrive.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
    return parts.size() == 2 && parts.at(0).compare(QStringLiteral("Users"), Qt::CaseInsensitive) == 0;
}

bool isUsersRootDirectoryPath(const QString& cleanNativeDir) {
    const QString norm = IndexingPolicy::normalizeNativePath(cleanNativeDir);
    return norm.size() == 8 &&
           norm.at(1) == QLatin1Char(':') &&
           norm.mid(2).compare(QStringLiteral("\\Users"), Qt::CaseInsensitive) == 0;
}

QVector<QString> listImmediateNormalSubdirs(const QString& parentDir) {
    QVector<QString> out;
    QString pattern = IndexingPolicy::normalizeNativePath(parentDir);
    if (!pattern.endsWith(QLatin1Char('\\'))) {
        pattern.append(QLatin1Char('\\'));
    }
    pattern.append(QLatin1Char('*'));

    WIN32_FIND_DATAW fd{};
    const std::wstring widePattern = pattern.toStdWString();
    HANDLE hFind = FindFirstFileExW(widePattern.c_str(),
                                    FindExInfoBasic,
                                    &fd,
                                    FindExSearchNameMatch,
                                    nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE || hFind == nullptr) {
        return out;
    }

    const QString cleanParent = IndexingPolicy::normalizeNativePath(parentDir);
    const QString basePrefix = cleanParent.endsWith(QLatin1Char('\\'))
                                   ? cleanParent
                                   : (cleanParent + QLatin1Char('\\'));
    do {
        const wchar_t* wName = fd.cFileName;
        if (wName[0] == L'.' && (wName[1] == L'\0' || (wName[1] == L'.' && wName[2] == L'\0'))) {
            continue;
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            continue;
        }
        out.append(basePrefix + QString::fromWCharArray(wName));
    } while (FindNextFileW(hFind, &fd) != FALSE);

    FindClose(hFind);
    return out;
}

} // namespace

QString IndexingPolicy::normalizeNativePath(const QString& path) {
    if (path.trimmed().isEmpty()) {
        return {};
    }
    QString clean = QDir::toNativeSeparators(QDir::cleanPath(path.trimmed()));
    if (clean.size() >= 2 && clean.at(1) == QLatin1Char(':')) {
        clean[0] = clean.at(0).toUpper();
        if (clean.size() == 2) {
            clean.append(QLatin1Char('\\'));
        }
    }
    while (clean.size() > 3 && clean.endsWith(QLatin1Char('\\'))) {
        clean.chop(1);
    }
    return clean;
}

bool IndexingPolicy::isDriveRootPath(const QString& cleanNativePath) {
    return cleanNativePath.size() == 3 &&
           cleanNativePath.at(0).isLetter() &&
           cleanNativePath.at(1) == QLatin1Char(':') &&
           cleanNativePath.at(2) == QLatin1Char('\\');
}

bool IndexingPolicy::isPathUnderRoot(const QString& candidatePath, const QString& rootPath) {
    const QString normCandidate = normalizeNativePath(candidatePath);
    const QString normRoot = normalizeNativePath(rootPath);
    if (normCandidate.isEmpty() || normRoot.isEmpty()) {
        return false;
    }
    if (normCandidate.compare(normRoot, Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QString prefix = normRoot.endsWith(QLatin1Char('\\')) ? normRoot : (normRoot + QLatin1Char('\\'));
    return normCandidate.startsWith(prefix, Qt::CaseInsensitive);
}

bool IndexingPolicy::isPathUnderAnyRoot(const QString& candidatePath, const QStringList& roots) {
    for (const QString& root : roots) {
        if (isPathUnderRoot(candidatePath, root)) {
            return true;
        }
    }
    return false;
}

QStringList IndexingPolicy::fixedDriveRoots() {
    QStringList drives;
    std::array<wchar_t, 512> buf{};
    const DWORD len = GetLogicalDriveStringsW(static_cast<DWORD>(buf.size() - 1), buf.data());
    if (len == 0 || len >= buf.size()) {
        return drives;
    }

    const wchar_t* cursor = buf.data();
    while (*cursor != L'\0') {
        if (GetDriveTypeW(cursor) == DRIVE_FIXED) {
            const QString driveStr = normalizeNativePath(QString::fromWCharArray(cursor));
            if (QDir(driveStr).exists()) {
                appendUniqueNormalizedRoot(&drives, driveStr);
            }
        }
        cursor += wcslen(cursor) + 1;
    }
    drives.sort(Qt::CaseInsensitive);
    return drives;
}

QStringList IndexingPolicy::startMenuRoots() {
    QStringList roots;
    const QString programData = qEnvironmentVariable("ProgramData");
    if (!programData.isEmpty()) {
        const QString sysMenu = normalizeNativePath(programData + QStringLiteral("\\Microsoft\\Windows\\Start Menu"));
        if (QDir(sysMenu).exists()) {
            appendUniqueNormalizedRoot(&roots, sysMenu);
        }
    }
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty()) {
        const QString userMenu = normalizeNativePath(appData + QStringLiteral("\\Microsoft\\Windows\\Start Menu"));
        if (QDir(userMenu).exists()) {
            appendUniqueNormalizedRoot(&roots, userMenu);
        }
    }
    return roots;
}

QStringList IndexingPolicy::buildWholePcScanRoots(const QStringList& contentSemanticRoots) {
    QStringList roots;
    for (const QString& drv : fixedDriveRoots()) {
        appendUniqueNormalizedRoot(&roots, drv);
    }
    for (const QString& menu : startMenuRoots()) {
        appendUniqueNormalizedRoot(&roots, menu);
    }
    for (const QString& userRoot : contentSemanticRoots) {
        const QString norm = normalizeNativePath(userRoot);
        if (norm.isEmpty() || !QDir(norm).exists()) {
            continue;
        }
        // Keep user root explicitly if it sits outside fixed drives or inside an excluded area (e.g. AppData/ProgramData)
        if (!isPathUnderAnyRoot(norm, roots) ||
            norm.contains(QStringLiteral("\\AppData\\"), Qt::CaseInsensitive) ||
            norm.contains(QStringLiteral("\\ProgramData\\"), Qt::CaseInsensitive)) {
            appendUniqueNormalizedRoot(&roots, norm);
        }
    }
    return roots;
}

bool IndexingPolicy::isBuiltInWholePcJunkDirName(const QString& dirNameLower) {
    if (dirNameLower.isEmpty()) {
        return false;
    }
    // Hidden dot-directories (.git, .gradle, .vscode, .cache, .m2, .codex, etc.) harbor huge tool caches
    if (dirNameLower.startsWith(QLatin1Char('.')) && dirNameLower.size() > 1) {
        return true;
    }
    return containsInsensitive(dirNameLower, {
        L"appdata",
        L"node_modules",
        L"venv",
        L"__pycache__",
        L"build",
        L"cmakefiles",
        L"temp",
        L"tmp",
        L"cache",
        L"code cache",
        L"gpucache",
        L"shadercache",
        L"dxcache",
        L"grshadercache",
        L"dawnwebgpucache",
        L"dawngraphitecache",
        L"appcache",
        L"depotcache",
        L"crashdumps",
        L"webcache"
    });
}

bool IndexingPolicy::shouldSkipDirectory(const QString& cleanParentDir,
                                         const QString& dirNameLower,
                                         DWORD fileAttributes,
                                         const QSet<QString>& ignoredDirNamesLower,
                                         bool wholePcMode) {
    if ((fileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;
    }
    if (ignoredDirNamesLower.contains(dirNameLower)) {
        return true;
    }
    if (!wholePcMode) {
        return false;
    }
    if (isBuiltInWholePcJunkDirName(dirNameLower)) {
        return true;
    }

    const QString normParent = normalizeNativePath(cleanParentDir);
    if (isDriveRootPath(normParent)) {
        if (isDriveRootSystemDirName(dirNameLower)) {
            return true;
        }
        // Skip hidden+system root folders ($...) not explicitly covered above
        if ((fileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 &&
            (fileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0) {
            return true;
        }
    }

    const QString parentLower = normParent.toLower();
    if (isProgramFilesProtectedSubdir(parentLower, dirNameLower)) {
        return true;
    }

    return false;
}

bool IndexingPolicy::shouldSkipFile(const QString& cleanParentDir,
                                    const QString& fileNameLower,
                                    DWORD fileAttributes,
                                    bool wholePcMode) {
    if (isSensitiveCredentialFile(fileNameLower)) {
        return true;
    }
    if (!wholePcMode) {
        return false;
    }

    const QString normParent = normalizeNativePath(cleanParentDir);
    if (isDriveRootPath(normParent)) {
        if (isDriveRootSystemFileName(fileNameLower)) {
            return true;
        }
        if ((fileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 &&
            (fileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0) {
            return true;
        }
    }

    if (fileNameLower.startsWith(QStringLiteral("ntuser.dat")) ||
        fileNameLower.startsWith(QStringLiteral("usrclass.dat"))) {
        return true;
    }

    return false;
}

bool IndexingPolicy::isRelativePathIgnored(const QString& normalizedRootPath,
                                           const QString& relativePath,
                                           const QSet<QString>& ignoredDirNamesLower,
                                           bool wholePcMode) {
    const QStringList parts = relativePath.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return false;
    }

    QString curParent = normalizedRootPath;
    const int lastIdx = parts.size() - 1;
    for (int i = 0; i < parts.size(); ++i) {
        const QString& seg = parts.at(i);
        const QString segLower = seg.toLower();
        const bool isLast = (i == lastIdx);

        if (isSensitiveCredentialFile(segLower)) {
            return true;
        }

        if (!isLast || wholePcMode) {
            // Check directory rules on interior segments (and basename junk names on leaf too)
            if (ignoredDirNamesLower.contains(segLower)) {
                return true;
            }
            if (wholePcMode) {
                if (isBuiltInWholePcJunkDirName(segLower) && (!isLast || QDir(curParent + QLatin1Char('\\') + seg).exists())) {
                    return true;
                }
                if (isDriveRootPath(curParent) &&
                    (isDriveRootSystemDirName(segLower) || isDriveRootSystemFileName(segLower))) {
                    return true;
                }
                if (isProgramFilesProtectedSubdir(curParent.toLower(), segLower)) {
                    return true;
                }
                if (isLast && (segLower.startsWith(QStringLiteral("ntuser.dat")) ||
                               segLower.startsWith(QStringLiteral("usrclass.dat")))) {
                    return true;
                }
            }
        }

        const QString sep = curParent.endsWith(QLatin1Char('\\')) ? QString() : QStringLiteral("\\");
        curParent = curParent + sep + seg;
    }
    return false;
}

QVector<WatchTarget> IndexingPolicy::buildWholePcWatchTargets(const QStringList& scanRoots,
                                                              const QSet<QString>& ignoredDirNamesLower) {
    QVector<WatchTarget> targets;
    QSet<QString> seenRootsLower;

    auto addTarget = [&](const QString& rawPath, bool subtree) {
        const QString norm = normalizeNativePath(rawPath);
        if (norm.isEmpty() || !QDir(norm).exists()) {
            return;
        }
        const QString key = norm.toLower() + (subtree ? QStringLiteral("|1") : QStringLiteral("|0"));
        if (seenRootsLower.contains(key)) {
            return;
        }
        seenRootsLower.insert(key);
        targets.append({norm, subtree});
    };

    for (const QString& rawRoot : scanRoots) {
        const QString normRoot = normalizeNativePath(rawRoot);
        if (normRoot.isEmpty() || !QDir(normRoot).exists()) {
            continue;
        }

        const bool isOsDrive = isDriveRootPath(normRoot) &&
                               (QDir(normRoot + QStringLiteral("Windows")).exists() ||
                                QDir(normRoot + QStringLiteral("Users")).exists());
        if (!isOsDrive) {
            addTarget(normRoot, true);
            continue;
        }

        // Watch OS drive root non-recursively so kernel events from C:\Windows & AppData never flood the buffer
        addTarget(normRoot, false);

        for (const QString& topDir : listImmediateNormalSubdirs(normRoot)) {
            const QString topNameLower = QFileInfo(topDir).fileName().toLower();
            const DWORD topAttrs = GetFileAttributesW(topDir.toStdWString().c_str());
            if (shouldSkipDirectory(normRoot, topNameLower, topAttrs, ignoredDirNamesLower, true)) {
                continue;
            }

            if (isUsersRootDirectoryPath(topDir)) {
                addTarget(topDir, false);
                for (const QString& userHome : listImmediateNormalSubdirs(topDir)) {
                    const QString userNameLower = QFileInfo(userHome).fileName().toLower();
                    const DWORD userAttrs = GetFileAttributesW(userHome.toStdWString().c_str());
                    if (shouldSkipDirectory(topDir, userNameLower, userAttrs, ignoredDirNamesLower, true)) {
                        continue;
                    }
                    addTarget(userHome, false);
                    for (const QString& userSub : listImmediateNormalSubdirs(userHome)) {
                        const QString subNameLower = QFileInfo(userSub).fileName().toLower();
                        const DWORD subAttrs = GetFileAttributesW(userSub.toStdWString().c_str());
                        if (!shouldSkipDirectory(userHome, subNameLower, subAttrs, ignoredDirNamesLower, true)) {
                            addTarget(userSub, true);
                        }
                    }
                }
            } else {
                addTarget(topDir, true);
            }
        }
    }

    return targets;
}

} // namespace lumen::indexing
