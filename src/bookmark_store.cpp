#include "bookmark_store.h"

#include <QSettings>

#include <utility>

namespace {

QString errorFor(const QSettings &settings)
{
    switch (settings.status()) {
    case QSettings::NoError:
        return {};
    case QSettings::AccessError:
        return QStringLiteral("The bookmark INI file cannot be written.");
    case QSettings::FormatError:
        return QStringLiteral("The bookmark INI file has an invalid format.");
    }
    return QStringLiteral("The bookmark INI file returned an unknown error.");
}

} // namespace

BookmarkStore::BookmarkStore(QString iniPath, QString archiveSignature)
    : iniPath_(std::move(iniPath)), archiveSignature_(std::move(archiveSignature))
{
}

QString BookmarkStore::settingsKey() const
{
    return QStringLiteral("bookmarks/v1/") + archiveSignature_ + QStringLiteral("/items");
}

QStringList BookmarkStore::load(QString *errorMessage) const
{
    QSettings settings(iniPath_, QSettings::IniFormat);
    const QStringList values = settings.value(settingsKey()).toStringList();
    if (errorMessage)
        *errorMessage = errorFor(settings);
    return values;
}

bool BookmarkStore::save(const QStringList &targets, QString *errorMessage) const
{
    QSettings settings(iniPath_, QSettings::IniFormat);
    settings.setValue(settingsKey(), targets);
    settings.sync();
    const QString error = errorFor(settings);
    if (errorMessage)
        *errorMessage = error;
    return error.isEmpty();
}

bool BookmarkStore::hasStoredValue() const
{
    QSettings settings(iniPath_, QSettings::IniFormat);
    return settings.contains(settingsKey());
}
