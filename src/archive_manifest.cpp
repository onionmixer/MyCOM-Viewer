#include "archive_manifest.h"

#include <QDir>

namespace {

QString safeRelativeDirectory(const QString &value)
{
    const QString cleaned = QDir::cleanPath(value);
    if (cleaned.isEmpty() || cleaned == QStringLiteral(".") || cleaned == QStringLiteral("..")
        || QDir::isAbsolutePath(cleaned) || cleaned.startsWith(QStringLiteral("../")))
        return {};
    return cleaned;
}

} // namespace

ArchiveManifestPaths readArchiveManifestPaths(const QJsonObject &manifest)
{
    ArchiveManifestPaths result;
    if (manifest.value(QStringLiteral("format")).toString() != QStringLiteral("mycom-archive/v1")
        || manifest.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        result.error = QStringLiteral("Unsupported MYCOM archive manifest format.");
        return result;
    }
    result.contentDirectory = safeRelativeDirectory(manifest.value(QStringLiteral("contentDirectory")).toString());
    result.assetDirectory = safeRelativeDirectory(
        manifest.value(QStringLiteral("normalized")).toObject().value(QStringLiteral("assetDirectory")).toString());
    if (result.contentDirectory.isEmpty() || result.assetDirectory.isEmpty()) {
        result.error = QStringLiteral("Archive manifest contains an invalid relative content or asset directory.");
        return result;
    }
    return result;
}
