#pragma once

#include <QString>
#include <QStringList>

class BookmarkStore final {
public:
    BookmarkStore(QString iniPath, QString archiveSignature);

    QStringList load(QString *errorMessage = nullptr) const;
    bool save(const QStringList &targets, QString *errorMessage = nullptr) const;
    bool hasStoredValue() const;

private:
    QString iniPath_;
    QString archiveSignature_;

    QString settingsKey() const;
};
