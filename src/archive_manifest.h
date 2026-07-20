#pragma once

#include <QJsonObject>
#include <QString>

struct ArchiveManifestPaths {
    QString contentDirectory;
    QString assetDirectory;
    QString error;
};

ArchiveManifestPaths readArchiveManifestPaths(const QJsonObject &manifest);
