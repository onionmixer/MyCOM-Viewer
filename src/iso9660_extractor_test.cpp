#include "iso9660_extractor.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

constexpr int kBlockSize = 2048;

bool require(bool condition, const QString &message)
{
    if (condition)
        return true;
    QTextStream(stderr) << "FAIL: " << message << '\n';
    return false;
}

void putLe16(QByteArray *bytes, int offset, quint16 value)
{
    bytes->operator[](offset) = static_cast<char>(value & 0xff);
    bytes->operator[](offset + 1) = static_cast<char>((value >> 8) & 0xff);
}

void putLe32(QByteArray *bytes, int offset, quint32 value)
{
    for (int index = 0; index < 4; ++index)
        bytes->operator[](offset + index) = static_cast<char>((value >> (index * 8)) & 0xff);
}

int appendRecord(QByteArray *directory, int offset, const QByteArray &name, quint32 extent,
                 quint32 bytes, bool isDirectory)
{
    const int length = 33 + name.size() + (name.size() % 2 == 0 ? 1 : 0);
    (*directory)[offset] = static_cast<char>(length);
    putLe32(directory, offset + 2, extent);
    putLe32(directory, offset + 10, bytes);
    (*directory)[offset + 25] = isDirectory ? 0x02 : 0x00;
    putLe16(directory, offset + 28, 1);
    (*directory)[offset + 32] = static_cast<char>(name.size());
    for (int index = 0; index < name.size(); ++index)
        (*directory)[offset + 33 + index] = name.at(index);
    return offset + length;
}

void writeBlock(QByteArray *image, int sector, const QByteArray &content)
{
    for (int index = 0; index < content.size(); ++index)
        (*image)[sector * kBlockSize + index] = content.at(index);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    if (!require(temporary.isValid(), QStringLiteral("temporary directory is available")))
        return 1;

    QByteArray image(32 * kBlockSize, '\0');
    QByteArray root(kBlockSize, '\0');
    int rootOffset = 0;
    rootOffset = appendRecord(&root, rootOffset, QByteArray(1, '\0'), 20, kBlockSize, true);
    rootOffset = appendRecord(&root, rootOffset, QByteArray(1, '\1'), 20, kBlockSize, true);
    rootOffset = appendRecord(&root, rootOffset, QByteArrayLiteral("BOOK.MVB;1"), 21, 3, false);
    rootOffset = appendRecord(&root, rootOffset, QByteArrayLiteral("DBF"), 22, kBlockSize, true);
    appendRecord(&root, rootOffset, QByteArrayLiteral("BMP"), 23, kBlockSize, true);
    writeBlock(&image, 20, root);

    QByteArray dbf(kBlockSize, '\0');
    int dbfOffset = 0;
    dbfOffset = appendRecord(&dbf, dbfOffset, QByteArray(1, '\0'), 22, kBlockSize, true);
    dbfOffset = appendRecord(&dbf, dbfOffset, QByteArray(1, '\1'), 20, kBlockSize, true);
    appendRecord(&dbf, dbfOffset, QByteArrayLiteral("MYDBF01.DBF;1"), 24, 3, false);
    writeBlock(&image, 22, dbf);

    QByteArray bmp(kBlockSize, '\0');
    int bmpOffset = 0;
    bmpOffset = appendRecord(&bmp, bmpOffset, QByteArray(1, '\0'), 23, kBlockSize, true);
    bmpOffset = appendRecord(&bmp, bmpOffset, QByteArray(1, '\1'), 20, kBlockSize, true);
    appendRecord(&bmp, bmpOffset, QByteArrayLiteral("IMAGE.DIB;1"), 25, 3, false);
    writeBlock(&image, 23, bmp);
    writeBlock(&image, 21, QByteArrayLiteral("mvb"));
    writeBlock(&image, 24, QByteArrayLiteral("dbf"));
    writeBlock(&image, 25, QByteArrayLiteral("dib"));

    QByteArray descriptor(kBlockSize, ' ');
    descriptor[0] = 1;
    descriptor.replace(1, 5, QByteArrayLiteral("CD001"));
    descriptor[6] = 1;
    putLe16(&descriptor, 128, kBlockSize);
    QByteArray rootRecord(34, '\0');
    appendRecord(&rootRecord, 0, QByteArray(1, '\0'), 20, kBlockSize, true);
    descriptor.replace(156, rootRecord.size(), rootRecord);
    writeBlock(&image, 16, descriptor);
    QByteArray terminator(kBlockSize, '\0');
    terminator[0] = static_cast<char>(255);
    terminator.replace(1, 5, QByteArrayLiteral("CD001"));
    terminator[6] = 1;
    writeBlock(&image, 17, terminator);

    const QString isoPath = temporary.filePath(QStringLiteral("fixture.iso"));
    QFile iso(isoPath);
    if (!require(iso.open(QIODevice::WriteOnly), QStringLiteral("fixture ISO can be created")))
        return 2;
    iso.write(image);
    iso.close();
    const QString output = temporary.filePath(QStringLiteral("out"));
    Iso9660ExtractionSummary summary;
    QString error;
    if (!require(extractMycomIso9660(isoPath, output, &summary, &error), error)
        || !require(summary.files == 3 && summary.bytes == 9, QStringLiteral("selected files are extracted"))) {
        return 3;
    }
    const auto readFile = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    if (!require(readFile(output + QStringLiteral("/BOOK.MVB")) == QByteArrayLiteral("mvb"),
                 QStringLiteral("MVB extracted"))
        || !require(readFile(output + QStringLiteral("/DBF/MYDBF01.DBF")) == QByteArrayLiteral("dbf"),
                    QStringLiteral("DBF extracted"))
        || !require(readFile(output + QStringLiteral("/BMP/IMAGE.DIB")) == QByteArrayLiteral("dib"),
                    QStringLiteral("BMP extracted"))) {
        return 4;
    }
    return 0;
}
