#include "bookmark_store.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

bool require(bool condition, const QString &message)
{
    if (condition)
        return true;
    QTextStream(stderr) << "FAIL: " << message << '\n';
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    if (!require(directory.isValid(), QStringLiteral("temporary directory is available")))
        return 1;

    const QString iniPath = directory.filePath(QStringLiteral("mycom-viewer.ini"));
    BookmarkStore firstArchive(iniPath, QStringLiteral("archive-a"));
    BookmarkStore secondArchive(iniPath, QStringLiteral("archive-b"));
    const QStringList initial{QStringLiteral("article:HEADA/90040480"),
                              QStringLiteral("booktext:VIEW")};
    QString error;
    if (!require(firstArchive.save(initial, &error), error)
        || !require(firstArchive.hasStoredValue(), QStringLiteral("saved value exists"))
        || !require(firstArchive.load(&error) == initial, error.isEmpty() ? QStringLiteral("values persist") : error)
        || !require(secondArchive.load(&error).isEmpty(), error.isEmpty() ? QStringLiteral("archives are isolated") : error)) {
        return 1;
    }

    const QStringList afterRemoval{QStringLiteral("booktext:VIEW")};
    if (!require(firstArchive.save(afterRemoval, &error), error)
        || !require(firstArchive.load(&error) == afterRemoval,
                    error.isEmpty() ? QStringLiteral("removal persists") : error)) {
        return 1;
    }
    if (!require(firstArchive.save({}, &error), error)
        || !require(firstArchive.hasStoredValue(), QStringLiteral("empty bookmark list is stored"))
        || !require(firstArchive.load(&error).isEmpty(),
                    error.isEmpty() ? QStringLiteral("clear persists") : error)) {
        return 1;
    }
    return 0;
}
