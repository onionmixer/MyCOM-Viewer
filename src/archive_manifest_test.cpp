#include "archive_manifest.h"

#include <QCoreApplication>
#include <QJsonObject>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QJsonObject normalized;
    normalized.insert(QStringLiteral("assetDirectory"), QStringLiteral("normalized/assets"));
    QJsonObject valid;
    valid.insert(QStringLiteral("format"), QStringLiteral("mycom-archive/v1"));
    valid.insert(QStringLiteral("schemaVersion"), 1);
    valid.insert(QStringLiteral("contentDirectory"), QStringLiteral("content"));
    valid.insert(QStringLiteral("normalized"), normalized);
    const ArchiveManifestPaths paths = readArchiveManifestPaths(valid);
    if (!paths.error.isEmpty() || paths.contentDirectory != QStringLiteral("content")
        || paths.assetDirectory != QStringLiteral("normalized/assets"))
        return 1;

    valid.insert(QStringLiteral("contentDirectory"), QStringLiteral("../outside"));
    if (readArchiveManifestPaths(valid).error.isEmpty())
        return 2;
    valid.insert(QStringLiteral("contentDirectory"), QStringLiteral("content"));
    valid.insert(QStringLiteral("schemaVersion"), 2);
    if (readArchiveManifestPaths(valid).error.isEmpty())
        return 3;
    return 0;
}
