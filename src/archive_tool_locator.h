#pragma once

#include <QString>
#include <QStringList>

namespace ArchiveToolLocator {

QString archiveBuilderFileName();
QStringList archiveBuilderCandidates(const QString &applicationDirectory);
QString findExecutable(const QStringList &candidates);
QString findArchiveBuilder(const QString &applicationDirectory);

} // namespace ArchiveToolLocator
