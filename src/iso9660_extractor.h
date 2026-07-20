#pragma once

#include <QString>

struct Iso9660ExtractionSummary {
    int files = 0;
    qint64 bytes = 0;
};

// Extract only the MYCOM files consumed by the normalizer.  The ISO reader is
// deliberately limited to plain ISO9660; MYCOM.ISO has no Joliet or Rock Ridge
// extensions and needs no external extraction program.
bool extractMycomIso9660(const QString &isoPath, const QString &outputDirectory,
                         Iso9660ExtractionSummary *summary, QString *errorMessage);
