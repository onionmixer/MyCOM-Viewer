#include "iso9660_extractor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace {

constexpr qint64 kDescriptorSector = 16;
constexpr qint64 kMaxDescriptorSectors = 64;
constexpr qint64 kMaxDirectoryBytes = 64LL * 1024 * 1024;
constexpr int kMaxDirectoryDepth = 32;

quint16 littleEndian16(const char *bytes)
{
    const auto *data = reinterpret_cast<const unsigned char *>(bytes);
    return static_cast<quint16>(data[0]) | (static_cast<quint16>(data[1]) << 8);
}

quint32 littleEndian32(const char *bytes)
{
    const auto *data = reinterpret_cast<const unsigned char *>(bytes);
    return static_cast<quint32>(data[0]) | (static_cast<quint32>(data[1]) << 8)
           | (static_cast<quint32>(data[2]) << 16) | (static_cast<quint32>(data[3]) << 24);
}

struct DirectoryRecord {
    quint32 extent = 0;
    quint32 bytes = 0;
    bool directory = false;
    bool multiExtent = false;
    QString name;
};

class Iso9660Reader final {
public:
    explicit Iso9660Reader(QString isoPath)
        : file_(std::move(isoPath))
    {
    }

    bool extract(const QString &outputDirectory, Iso9660ExtractionSummary *summary, QString *errorMessage)
    {
        if (!file_.open(QIODevice::ReadOnly)) {
            *errorMessage = QStringLiteral("Cannot open ISO %1: %2")
                                .arg(file_.fileName(), file_.errorString());
            return false;
        }
        DirectoryRecord root;
        if (!readPrimaryVolumeDescriptor(&root, errorMessage))
            return false;
        outputDirectory_ = QDir(outputDirectory).absolutePath();
        summary_ = summary;
        if (summary_) {
            summary_->files = 0;
            summary_->bytes = 0;
        }
        return visitDirectory(root, QString(), 0, errorMessage);
    }

private:
    bool readPrimaryVolumeDescriptor(DirectoryRecord *root, QString *errorMessage)
    {
        QByteArray descriptor;
        for (qint64 sector = kDescriptorSector; sector < kDescriptorSector + kMaxDescriptorSectors; ++sector) {
            if (!readAt(sector * 2048, 2048, &descriptor, errorMessage))
                return false;
            const unsigned char type = static_cast<unsigned char>(descriptor.at(0));
            if (descriptor.mid(1, 5) != QByteArrayLiteral("CD001") || descriptor.at(6) != 1) {
                *errorMessage = QStringLiteral("Unsupported ISO descriptor at sector %1.").arg(sector);
                return false;
            }
            if (type == 1) {
                blockSize_ = littleEndian16(descriptor.constData() + 128);
                if (blockSize_ < 512 || blockSize_ > 32768 || (blockSize_ & (blockSize_ - 1)) != 0) {
                    *errorMessage = QStringLiteral("Unsupported ISO logical block size: %1.").arg(blockSize_);
                    return false;
                }
                if (!parseRecord(descriptor.mid(156), root, errorMessage))
                    return false;
                if (!root->directory) {
                    *errorMessage = QStringLiteral("ISO primary volume descriptor has no root directory.");
                    return false;
                }
                return true;
            }
            if (type == 255)
                break;
        }
        *errorMessage = QStringLiteral("ISO9660 primary volume descriptor was not found.");
        return false;
    }

    bool readAt(qint64 offset, qint64 bytes, QByteArray *result, QString *errorMessage)
    {
        if (offset < 0 || bytes < 0 || offset > file_.size() || bytes > file_.size() - offset) {
            *errorMessage = QStringLiteral("ISO record points outside the ISO image.");
            return false;
        }
        if (!file_.seek(offset)) {
            *errorMessage = QStringLiteral("Cannot seek ISO: %1").arg(file_.errorString());
            return false;
        }
        *result = file_.read(bytes);
        if (result->size() != bytes) {
            *errorMessage = QStringLiteral("Cannot read ISO record: %1").arg(file_.errorString());
            return false;
        }
        return true;
    }

    bool parseRecord(const QByteArray &bytes, DirectoryRecord *record, QString *errorMessage) const
    {
        if (bytes.size() < 34) {
            *errorMessage = QStringLiteral("Truncated ISO directory record.");
            return false;
        }
        const int recordLength = static_cast<unsigned char>(bytes.at(0));
        const int nameLength = static_cast<unsigned char>(bytes.at(32));
        if (recordLength < 34 || recordLength > bytes.size() || 33 + nameLength > recordLength) {
            *errorMessage = QStringLiteral("Invalid ISO directory record length.");
            return false;
        }
        record->extent = littleEndian32(bytes.constData() + 2);
        record->bytes = littleEndian32(bytes.constData() + 10);
        const unsigned char flags = static_cast<unsigned char>(bytes.at(25));
        record->directory = (flags & 0x02U) != 0;
        record->multiExtent = (flags & 0x80U) != 0;
        const QByteArray rawName = bytes.mid(33, nameLength);
        if (rawName.size() == 1 && static_cast<unsigned char>(rawName.at(0)) <= 1) {
            record->name = static_cast<unsigned char>(rawName.at(0)) == 0 ? QStringLiteral(".")
                                                                          : QStringLiteral("..");
            return true;
        }
        QString name = QString::fromLatin1(rawName);
        const int version = name.indexOf(QLatin1Char(';'));
        if (version >= 0)
            name.truncate(version);
        if (name.endsWith(QLatin1Char('.')))
            name.chop(1);
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")
            || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
            *errorMessage = QStringLiteral("Unsafe ISO directory entry name.");
            return false;
        }
        record->name = name;
        return true;
    }

    bool visitDirectory(const DirectoryRecord &directory, const QString &relativePath, int depth,
                        QString *errorMessage)
    {
        if (depth > kMaxDirectoryDepth) {
            *errorMessage = QStringLiteral("ISO directory nesting is too deep.");
            return false;
        }
        if (directory.multiExtent) {
            *errorMessage = QStringLiteral("Multi-extent ISO directories are unsupported.");
            return false;
        }
        const qint64 offset = static_cast<qint64>(directory.extent) * blockSize_;
        if (directory.bytes > kMaxDirectoryBytes) {
            *errorMessage = QStringLiteral("ISO directory is too large.");
            return false;
        }
        const QString directoryKey = QString::number(directory.extent) + QLatin1Char(':')
                                     + QString::number(directory.bytes);
        if (visitedDirectories_.contains(directoryKey)) {
            *errorMessage = QStringLiteral("ISO directory loop detected.");
            return false;
        }
        visitedDirectories_.insert(directoryKey);
        QByteArray data;
        if (!readAt(offset, directory.bytes, &data, errorMessage))
            return false;
        qint64 position = 0;
        while (position < data.size()) {
            const int recordLength = static_cast<unsigned char>(data.at(position));
            if (recordLength == 0) {
                position = ((position / blockSize_) + 1) * blockSize_;
                continue;
            }
            if (position + recordLength > data.size()) {
                *errorMessage = QStringLiteral("ISO directory record crosses its declared boundary.");
                return false;
            }
            DirectoryRecord entry;
            if (!parseRecord(data.mid(position, recordLength), &entry, errorMessage))
                return false;
            position += recordLength;
            if (entry.name == QStringLiteral(".") || entry.name == QStringLiteral(".."))
                continue;
            const QString entryPath = relativePath.isEmpty() ? entry.name
                                                              : relativePath + QLatin1Char('/') + entry.name;
            if (entry.directory) {
                if (!visitDirectory(entry, entryPath, depth + 1, errorMessage))
                    return false;
            } else if (isMycomInput(entryPath)) {
                if (!extractFile(entry, entryPath, errorMessage))
                    return false;
            }
        }
        return true;
    }

    static bool isMycomInput(const QString &path)
    {
        const QString normalized = path.toUpper();
        if (normalized.endsWith(QStringLiteral(".MVB")))
            return true;
        if (normalized == QStringLiteral("DBF/MYDBF01.DBF"))
            return true;
        const QString topLevel = normalized.section(QLatin1Char('/'), 0, 0);
        return topLevel == QStringLiteral("BMP") || topLevel == QStringLiteral("WAV")
               || topLevel == QStringLiteral("MYAVI");
    }

    bool extractFile(const DirectoryRecord &entry, const QString &relativePath, QString *errorMessage)
    {
        if (entry.multiExtent) {
            *errorMessage = QStringLiteral("Multi-extent ISO files are unsupported: %1").arg(relativePath);
            return false;
        }
        const QString normalizedPath = QDir::cleanPath(relativePath);
        if (normalizedPath.startsWith(QStringLiteral("../")) || normalizedPath == QStringLiteral("..")) {
            *errorMessage = QStringLiteral("Unsafe ISO output path.");
            return false;
        }
        const QString destination = QDir(outputDirectory_).filePath(normalizedPath);
        const QString identity = normalizedPath.toLower();
        if (extractedPaths_.contains(identity)) {
            *errorMessage = QStringLiteral("Duplicate ISO output path: %1").arg(relativePath);
            return false;
        }
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
            *errorMessage = QStringLiteral("Cannot create extraction directory for %1").arg(relativePath);
            return false;
        }
        QFile output(destination);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            *errorMessage = QStringLiteral("Cannot write extracted file %1: %2")
                                .arg(destination, output.errorString());
            return false;
        }
        const qint64 offset = static_cast<qint64>(entry.extent) * blockSize_;
        if (offset < 0 || offset > file_.size() || entry.bytes > file_.size() - offset) {
            *errorMessage = QStringLiteral("ISO file points outside the ISO image: %1").arg(relativePath);
            return false;
        }
        if (!file_.seek(offset)) {
            *errorMessage = QStringLiteral("Cannot seek ISO file %1: %2").arg(relativePath, file_.errorString());
            return false;
        }
        qint64 remaining = entry.bytes;
        while (remaining > 0) {
            const QByteArray chunk = file_.read(qMin<qint64>(remaining, 1024 * 1024));
            if (chunk.isEmpty()) {
                *errorMessage = QStringLiteral("Cannot read ISO file %1: %2").arg(relativePath, file_.errorString());
                return false;
            }
            if (output.write(chunk) != chunk.size()) {
                *errorMessage = QStringLiteral("Cannot write extracted file %1: %2")
                                    .arg(destination, output.errorString());
                return false;
            }
            remaining -= chunk.size();
        }
        extractedPaths_.insert(identity);
        if (summary_) {
            ++summary_->files;
            summary_->bytes += entry.bytes;
        }
        return true;
    }

    QFile file_;
    quint16 blockSize_ = 2048;
    QString outputDirectory_;
    QSet<QString> visitedDirectories_;
    QSet<QString> extractedPaths_;
    Iso9660ExtractionSummary *summary_ = nullptr;
};

} // namespace

bool extractMycomIso9660(const QString &isoPath, const QString &outputDirectory,
                         Iso9660ExtractionSummary *summary, QString *errorMessage)
{
    Iso9660Reader reader(isoPath);
    return reader.extract(outputDirectory, summary, errorMessage);
}
