#include "archive_tool_locator.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace ArchiveToolLocator {

QString archiveBuilderFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("mycom-archive-build.exe");
#else
    return QStringLiteral("mycom-archive-build");
#endif
}

QStringList archiveBuilderCandidates(const QString &applicationDirectory)
{
    QStringList candidates;
    const QString fileName = archiveBuilderFileName();
    if (!applicationDirectory.isEmpty()) {
        const QDir applicationDir(applicationDirectory);
        candidates.append(applicationDir.filePath(fileName));
#ifdef Q_OS_MACOS
        // Development builds place the command-line target next to the .app
        // bundle; the installed PKG uses the conventional /usr/local/bin path.
        candidates.append(applicationDir.filePath(QStringLiteral("../../../") + fileName));
        candidates.append(QStringLiteral("/usr/local/bin/") + fileName);
#endif
    }
    candidates.append(QStandardPaths::findExecutable(fileName));
    candidates.removeAll(QString());
    candidates.removeDuplicates();
    return candidates;
}

QString findExecutable(const QStringList &candidates)
{
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable())
            return info.absoluteFilePath();
    }
    return {};
}

QString findArchiveBuilder(const QString &applicationDirectory)
{
    return findExecutable(archiveBuilderCandidates(applicationDirectory));
}

} // namespace ArchiveToolLocator
