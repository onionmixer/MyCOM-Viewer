#include "archive_tool_locator.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
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

    const QString builderPath = directory.filePath(ArchiveToolLocator::archiveBuilderFileName());
    QFile builder(builderPath);
    if (!require(builder.open(QIODevice::WriteOnly), QStringLiteral("temporary builder can be created")))
        return 2;
    builder.write("test");
    builder.close();
    builder.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    const QStringList candidates = ArchiveToolLocator::archiveBuilderCandidates(directory.path());
    if (!require(candidates.contains(builderPath), QStringLiteral("local builder candidate is included"))
        || !require(ArchiveToolLocator::findExecutable({builderPath}) == QFileInfo(builderPath).absoluteFilePath(),
                    QStringLiteral("executable builder is found"))) {
        return 3;
    }

    return 0;
}
