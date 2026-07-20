#include <algorithm>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QTextCodec>
#include <QTextStream>
#include <QTemporaryDir>
#include <QUrl>

namespace {

struct TextFragment {
    qint64 offset = 0;
    QString text;
    int occurrences = 1;
};

struct ArticleMetadata {
    QString id;
    QString date;
    QString section;
    QString category;
    QString keywords;
    QString title;
};

struct ArticleTopic {
    QString id;
    int firstFragment = 0;
    int afterLastFragment = 0;
    QString boundarySource;
};

struct AssetCatalog {
    QHash<QString, QString> sourceByLowercasePath;
    QString outputDirectory;
    bool copyAssets = false;
};

struct ConversionSummary {
    int textFragmentCount = 0;
    int articleTopicCount = 0;
    QStringList articleIds;
    bool succeeded = false;
};

struct NavigationReference {
    QString function;
    qint64 offset = 0;
    QString sourceBook;
    QString sourcePane;
    QString targetId;
    QString targetPane;
    QString resolution;
    QString contextArticleId;
};

struct RenderToken {
    qint64 offset = 0;
    QString text;
    QString kind;
    QString style;
};

bool isAsciiTextByte(unsigned char value)
{
    return value == '\t' || value == '\n' || value == '\r'
        || (value >= 0x20 && value <= 0x7e);
}

bool isCp949Trail(unsigned char value)
{
    return (value >= 0x41 && value <= 0x5a) || (value >= 0x61 && value <= 0x7a)
        || (value >= 0x81 && value <= 0xfe);
}

const QSet<quint16> &validCp949Pairs()
{
    static const QSet<quint16> pairs = [] {
        QSet<quint16> result;
        QTextCodec *codec = QTextCodec::codecForName("CP949");
        if (!codec)
            codec = QTextCodec::codecForLocale();
        for (int lead = 0x81; lead <= 0xfe; ++lead) {
            for (int trail = 0x00; trail <= 0xff; ++trail) {
                if (!isCp949Trail(static_cast<unsigned char>(trail)))
                    continue;
                QByteArray bytes;
                bytes.append(static_cast<char>(lead));
                bytes.append(static_cast<char>(trail));
                const QString decoded = codec->toUnicode(bytes);
                if (decoded.size() == 1 && decoded.at(0) != QChar::ReplacementCharacter
                    && !decoded.at(0).isNull()) {
                    result.insert(static_cast<quint16>((lead << 8) | trail));
                }
            }
        }
        return result;
    }();
    return pairs;
}

QByteArray trimTextBytes(QByteArray value)
{
    while (!value.isEmpty() && static_cast<unsigned char>(value.front()) <= 0x20)
        value.remove(0, 1);
    while (!value.isEmpty() && static_cast<unsigned char>(value.back()) <= 0x20)
        value.chop(1);
    return value;
}

bool isUsefulText(const QString &text, int minimumBytes)
{
    if (text.size() < minimumBytes)
        return false;

    int asciiLetters = 0;
    int KoreanCharacters = 0;
    int replacements = 0;
    for (const QChar character : text) {
        if (character == QChar::ReplacementCharacter)
            ++replacements;
        if (character.isLetterOrNumber() && character.unicode() < 0x80)
            ++asciiLetters;
        if (character.unicode() >= 0xac00 && character.unicode() <= 0xd7a3)
            ++KoreanCharacters;
    }
    return (asciiLetters >= 6 || KoreanCharacters >= 2) && replacements == 0;
}

QList<TextFragment> extractText(const QByteArray &contents, int minimumBytes)
{
    QTextCodec *codec = QTextCodec::codecForName("CP949");
    if (!codec)
        codec = QTextCodec::codecForLocale();

    QList<TextFragment> fragments;
    QHash<QString, int> fragmentIndex;
    qsizetype start = -1;
    const QSet<quint16> &cp949Pairs = validCp949Pairs();

    const auto addRun = [&](qsizetype offset, qsizetype length) {
        if (length < minimumBytes)
            return;

        const QByteArray bytes = trimTextBytes(contents.mid(offset, length));
        if (bytes.size() < minimumBytes)
            return;

        const QString text = codec->toUnicode(bytes);
        if (!isUsefulText(text, minimumBytes))
            return;

        const auto existing = fragmentIndex.constFind(text);
        if (existing != fragmentIndex.constEnd()) {
            ++fragments[*existing].occurrences;
            return;
        }

        fragmentIndex.insert(text, fragments.size());
        fragments.append({offset, text, 1});
    };

    for (qsizetype index = 0; index < contents.size();) {
        const unsigned char byte = static_cast<unsigned char>(contents.at(index));
        const bool isCp949Pair = byte >= 0x81 && byte <= 0xfe && index + 1 < contents.size()
            && cp949Pairs.contains(static_cast<quint16>((byte << 8)
                                                         | static_cast<unsigned char>(contents.at(index + 1))));
        if (isAsciiTextByte(byte) || isCp949Pair) {
            if (start < 0)
                start = index;
            index += isCp949Pair ? 2 : 1;
            continue;
        }

        if (start >= 0)
            addRun(start, index - start);
        start = -1;
        ++index;
    }
    if (start >= 0)
        addRun(start, contents.size() - start);

    return fragments;
}

QString normalizedPath(QString value)
{
    value.replace('\\', '/');
    return value;
}

QJsonArray sortedArray(const QSet<QString> &values)
{
    QStringList sorted = values.values();
    sorted.sort(Qt::CaseInsensitive);
    QJsonArray output;
    for (const QString &value : sorted)
        output.append(value);
    return output;
}

QList<NavigationReference> extractNavigationReferences(const QString &byteText)
{
    const QRegularExpression expression(
        QStringLiteral("\\b(PopupID|JumpID|PaneID)\\(\\s*`([A-Za-z0-9_]+)(?:>([A-Za-z0-9_]+))?[`']\\s*,\\s*`?([A-Za-z0-9_]+)(?:>([A-Za-z0-9_]+))?[`']?"),
        QRegularExpression::CaseInsensitiveOption);
    QList<NavigationReference> references;
    for (auto match = expression.globalMatch(byteText); match.hasNext();) {
        const QRegularExpressionMatch item = match.next();
        NavigationReference reference;
        reference.function = item.captured(1);
        reference.offset = item.capturedStart();
        reference.sourceBook = item.captured(2).toUpper();
        reference.sourcePane = item.captured(3);
        reference.targetId = item.captured(4);
        reference.targetPane = item.captured(5);
        if (reference.function.compare(QStringLiteral("PaneID"), Qt::CaseInsensitive) == 0) {
            reference.resolution = QStringLiteral("paneLayout");
        } else if (QRegularExpression(QStringLiteral("^h[0-9]{8}$"), QRegularExpression::CaseInsensitiveOption)
                       .match(reference.targetId).hasMatch()) {
            reference.resolution = QStringLiteral("article");
        } else if (QRegularExpression(QStringLiteral("^Y[0-9]{4}$"), QRegularExpression::CaseInsensitiveOption)
                       .match(reference.targetId).hasMatch()) {
            reference.resolution = QStringLiteral("monthIndex");
        } else {
            reference.resolution = QStringLiteral("unresolvedLegacyTopic");
        }
        references.append(reference);
    }
    return references;
}

QString articleContextForOffset(qint64 offset, const QList<TextFragment> &fragments,
                                const QList<ArticleTopic> &topics)
{
    int fragmentIndex = -1;
    for (int index = 0; index < fragments.size(); ++index) {
        if (fragments.at(index).offset > offset)
            break;
        fragmentIndex = index;
    }
    if (fragmentIndex < 0)
        return {};
    for (const ArticleTopic &topic : topics) {
        if (fragmentIndex >= topic.firstFragment && fragmentIndex < topic.afterLastFragment)
            return topic.id;
    }
    return {};
}

QJsonArray navigationJson(const QList<NavigationReference> &references)
{
    QJsonArray output;
    for (const NavigationReference &reference : references) {
        QJsonObject item;
        item.insert(QStringLiteral("function"), reference.function);
        item.insert(QStringLiteral("offset"), static_cast<double>(reference.offset));
        item.insert(QStringLiteral("sourceBook"), reference.sourceBook);
        item.insert(QStringLiteral("sourcePane"), reference.sourcePane);
        item.insert(QStringLiteral("targetId"), reference.targetId);
        item.insert(QStringLiteral("targetPane"), reference.targetPane);
        item.insert(QStringLiteral("resolution"), reference.resolution);
        if (!reference.contextArticleId.isEmpty())
            item.insert(QStringLiteral("contextArticleId"), reference.contextArticleId);
        output.append(item);
    }
    return output;
}

void writeNavigationReport(const QString &outputDirectory, const QString &baseName,
                           const QList<NavigationReference> &references)
{
    QFile report(QDir(outputDirectory).filePath(baseName + QStringLiteral(".navigation.html")));
    if (!report.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    QTextStream html(&report);
    html.setCodec("UTF-8");
    QHash<QString, int> counts;
    for (const NavigationReference &reference : references)
        ++counts[reference.resolution];
    html << "<!doctype html><meta charset=\"utf-8\"><title>" << baseName
         << " navigation report</title><style>body{font:16px/1.45 system-ui,sans-serif;margin:2rem}"
            "table{border-collapse:collapse;width:100%}th,td{border:1px solid #ccc;padding:.35rem;text-align:left}"
            "code{white-space:nowrap}</style>\n";
    html << "<h1>" << baseName << " legacy navigation report</h1><ul>\n";
    for (const QString &key : QStringList{QStringLiteral("article"), QStringLiteral("monthIndex"),
                                          QStringLiteral("paneLayout"), QStringLiteral("unresolvedLegacyTopic")})
        html << "<li>" << key << ": " << counts.value(key) << "</li>\n";
    html << "</ul><p>Unresolved legacy topics are retained as source metadata and are not guessed as article links.</p>\n";
    html << "<table><tr><th>offset</th><th>function</th><th>source</th><th>target</th><th>resolution</th></tr>\n";
    for (const NavigationReference &reference : references) {
        html << "<tr><td>0x" << QString::number(reference.offset, 16) << "</td><td>"
             << reference.function.toHtmlEscaped() << "</td><td><code>"
             << reference.sourceBook.toHtmlEscaped();
        if (!reference.sourcePane.isEmpty()) html << ">" << reference.sourcePane.toHtmlEscaped();
        html << "</code></td><td><code>" << reference.targetId.toHtmlEscaped();
        if (!reference.targetPane.isEmpty()) html << ">" << reference.targetPane.toHtmlEscaped();
        html << "</code></td><td>" << reference.resolution.toHtmlEscaped();
        if (!reference.contextArticleId.isEmpty())
            html << " <small>in " << reference.contextArticleId.toHtmlEscaped() << "</small>";
        html << "</td></tr>\n";
    }
    html << "</table>\n";
}

quint16 littleEndian16(const QByteArray &value, int offset)
{
    return static_cast<quint16>(static_cast<unsigned char>(value.at(offset)))
        | (static_cast<quint16>(static_cast<unsigned char>(value.at(offset + 1))) << 8);
}

quint32 littleEndian32(const QByteArray &value, int offset)
{
    return static_cast<quint32>(static_cast<unsigned char>(value.at(offset)))
        | (static_cast<quint32>(static_cast<unsigned char>(value.at(offset + 1))) << 8)
        | (static_cast<quint32>(static_cast<unsigned char>(value.at(offset + 2))) << 16)
        | (static_cast<quint32>(static_cast<unsigned char>(value.at(offset + 3))) << 24);
}

QHash<QString, ArticleMetadata> readMycomDbf(const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *errorMessage = QStringLiteral("Cannot read DBF %1: %2").arg(path, file.errorString());
        return {};
    }
    const QByteArray data = file.readAll();
    if (data.size() < 33) {
        *errorMessage = QStringLiteral("DBF %1 is too short.").arg(path);
        return {};
    }

    const quint32 recordCount = littleEndian32(data, 4);
    const quint16 headerLength = littleEndian16(data, 8);
    const quint16 recordLength = littleEndian16(data, 10);
    if (headerLength > data.size() || recordLength < 2) {
        *errorMessage = QStringLiteral("DBF %1 has an invalid header.").arg(path);
        return {};
    }

    struct Field { QString name; int offset; int length; };
    QList<Field> fields;
    int recordOffset = 1; // First byte is the deletion flag.
    for (int offset = 32; offset + 32 <= headerLength && data.at(offset) != '\r'; offset += 32) {
        const QByteArray nameBytes = data.mid(offset, 11);
        const int nul = nameBytes.indexOf('\0');
        const QString name = QString::fromLatin1(nameBytes.constData(), nul < 0 ? nameBytes.size() : nul);
        const int length = static_cast<unsigned char>(data.at(offset + 16));
        fields.append({name, recordOffset, length});
        recordOffset += length;
    }

    QTextCodec *codec = QTextCodec::codecForName("CP949");
    if (!codec)
        codec = QTextCodec::codecForLocale();
    const auto decodeField = [codec](QByteArray value) {
        value = value.trimmed();
        return codec->toUnicode(value);
    };
    const auto valueFor = [&fields, &decodeField](const QByteArray &record, const QString &name) {
        for (const Field &field : fields) {
            if (field.name.compare(name, Qt::CaseInsensitive) == 0)
                return decodeField(record.mid(field.offset, field.length));
        }
        return QString();
    };

    QHash<QString, ArticleMetadata> records;
    for (quint32 index = 0; index < recordCount; ++index) {
        const qint64 offset = headerLength + static_cast<qint64>(index) * recordLength;
        if (offset + recordLength > data.size())
            break;
        const QByteArray record = data.mid(offset, recordLength);
        if (record.at(0) == '*')
            continue;
        ArticleMetadata article;
        article.id = valueFor(record, QStringLiteral("IDNO"));
        if (article.id.isEmpty())
            continue;
        article.date = valueFor(record, QStringLiteral("DATE"));
        article.section = valueFor(record, QStringLiteral("SEL1"));
        article.category = valueFor(record, QStringLiteral("SEL2"));
        article.keywords = valueFor(record, QStringLiteral("BU"));
        article.title = valueFor(record, QStringLiteral("JMOK"));
        records.insert(article.id, article);
    }
    return records;
}

QJsonObject metadataJson(const ArticleMetadata &metadata)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), metadata.id);
    object.insert(QStringLiteral("date"), metadata.date);
    object.insert(QStringLiteral("section"), metadata.section);
    object.insert(QStringLiteral("category"), metadata.category);
    object.insert(QStringLiteral("keywords"), metadata.keywords);
    object.insert(QStringLiteral("title"), metadata.title);
    return object;
}

QJsonArray catalogJson(const QHash<QString, ArticleMetadata> &metadata)
{
    QList<ArticleMetadata> entries = metadata.values();
    std::sort(entries.begin(), entries.end(), [](const ArticleMetadata &left, const ArticleMetadata &right) {
        if (left.date != right.date)
            return left.date < right.date;
        return left.id < right.id;
    });
    QJsonArray output;
    for (const ArticleMetadata &entry : entries)
        output.append(metadataJson(entry));
    return output;
}

QList<ArticleTopic> findNavigationArticleTopics(const QList<TextFragment> &fragments)
{
    const QRegularExpression marker(QStringLiteral("PopupID\\([^\\r\\n]{0,120}h([0-9]{8})>mokcha"),
                                    QRegularExpression::CaseInsensitiveOption);
    QList<ArticleTopic> topics;
    for (int index = 0; index < fragments.size(); ++index) {
        for (auto match = marker.globalMatch(fragments.at(index).text); match.hasNext();) {
            const QString id = match.next().captured(1);
            if (!topics.isEmpty() && topics.last().id == id)
                continue;
            topics.append({id, index, fragments.size(), QStringLiteral("legacyNavigationReference")});
        }
    }
    for (int index = 0; index + 1 < topics.size(); ++index)
        topics[index].afterLastFragment = topics[index + 1].firstFragment;
    return topics;
}

QString dateMarkerForMetadata(const ArticleMetadata &metadata)
{
    // The final DBF digit is sometimes damaged (for example '_' or '&'), while
    // Viewer stores only the first three digits in its stream marker.
    static const QRegularExpression dateExpression(
        QStringLiteral("^(\\d{4})\\.(\\d{2})\\.(\\d{3})[0-9_&]$"));
    const QRegularExpressionMatch match = dateExpression.match(metadata.date);
    if (!match.hasMatch())
        return {};
    return match.captured(1).right(2) + QLatin1Char('.') + match.captured(2)
           + QLatin1Char('.') + match.captured(3);
}

QString normalizedTitleText(const QString &text)
{
    QString normalized;
    normalized.reserve(text.size());
    for (const QChar character : text) {
        if (character.isLetterOrNumber())
            normalized.append(character.toLower());
    }
    return normalized;
}

QString semanticTitleText(QString text)
{
    // Series counters vary between the DBF and the rendered heading (for
    // example [3/6] versus [1/4]) and must not override the title words.
    static const QRegularExpression seriesCounter(QStringLiteral("\\[\\s*\\d+\\s*/\\s*\\d+\\s*\\]"));
    text.remove(seriesCounter);
    return normalizedTitleText(text);
}

QList<ArticleTopic> findArticleTopics(const QList<TextFragment> &fragments,
                                      const QHash<QString, ArticleMetadata> &metadata)
{
    static const QRegularExpression markerExpression(QStringLiteral("^(\\d{2}\\.\\d{2}\\.\\d{3})$"));
    QHash<QString, ArticleMetadata> metadataByMarker;
    QHash<QString, QList<ArticleMetadata>> metadataByMonth;
    for (auto item = metadata.constBegin(); item != metadata.constEnd(); ++item) {
        const ArticleMetadata &article = item.value();
        const QString marker = dateMarkerForMetadata(article);
        if (!marker.isEmpty())
            metadataByMarker.insert(marker, article);
        if (article.date.size() >= 7)
            metadataByMonth[article.date.mid(2, 5)].append(article);
    }

    struct StreamMarker { int fragment = 0; QString code; };
    QList<StreamMarker> streamMarkers;
    for (int index = 0; index < fragments.size(); ++index) {
        const QRegularExpressionMatch match = markerExpression.match(fragments.at(index).text.simplified());
        if (match.hasMatch())
            streamMarkers.append({index, match.captured(1)});
    }

    QList<ArticleTopic> topics;
    QSet<QString> usedArticleIds;
    QList<StreamMarker> unresolvedMarkers;
    for (const StreamMarker &streamMarker : streamMarkers) {
        const auto metadataItem = metadataByMarker.constFind(streamMarker.code);
        if (metadataItem == metadataByMarker.constEnd()) {
            unresolvedMarkers.append(streamMarker);
            continue;
        }
        topics.append({metadataItem->id, streamMarker.fragment, fragments.size(),
                       QStringLiteral("streamDateMarkerExact")});
        usedArticleIds.insert(metadataItem->id);
    }

    // Some DBF dates contain a single corrupted digit while the MVB stream marker is intact.
    // Resolve only a one-to-one leftover within the same publication month; otherwise retain
    // the catalog record without guessing a content boundary.
    QHash<QString, QList<StreamMarker>> unresolvedByMonth;
    for (const StreamMarker &streamMarker : unresolvedMarkers)
        unresolvedByMonth[streamMarker.code.left(5)].append(streamMarker);
    for (auto item = unresolvedByMonth.constBegin(); item != unresolvedByMonth.constEnd(); ++item) {
        QList<ArticleMetadata> candidates;
        for (const ArticleMetadata &article : metadataByMonth.value(item.key())) {
            if (!usedArticleIds.contains(article.id))
                candidates.append(article);
        }
        if (item.value().size() != 1 || candidates.size() != 1)
            continue;
        topics.append({candidates.first().id, item.value().first().fragment, fragments.size(),
                       QStringLiteral("streamDateMarkerMonthOrderInference")});
        usedArticleIds.insert(candidates.first().id);
    }

    // Some MVB date markers carry a stale or reused numeric code.  When the
    // nearby rendered heading disproves the exact DBF assignment but uniquely
    // identifies an unused record from the same month, repair that assignment.
    // The displaced record is intentionally left for the later title matcher.
    for (ArticleTopic &topic : topics) {
        if (topic.boundarySource != QStringLiteral("streamDateMarkerExact") || !metadata.contains(topic.id))
            continue;
        const ArticleMetadata assigned = metadata.value(topic.id);
        const QString assignedTitle = semanticTitleText(assigned.title);
        if (assignedTitle.size() < 8)
            continue;

        QString nearbyText;
        const int first = std::max(0, topic.firstFragment - 3);
        const int last = std::min(fragments.size(), topic.firstFragment + 7);
        for (int index = first; index < last; ++index)
            nearbyText += semanticTitleText(fragments.at(index).text);
        if (nearbyText.contains(assignedTitle))
            continue;

        QList<ArticleMetadata> headingCandidates;
        for (const ArticleMetadata &candidate : metadataByMonth.value(assigned.date.mid(2, 5))) {
            if (usedArticleIds.contains(candidate.id))
                continue;
            const QString candidateTitle = semanticTitleText(candidate.title);
            if (candidateTitle.size() >= 8 && nearbyText.contains(candidateTitle))
                headingCandidates.append(candidate);
        }
        if (headingCandidates.size() != 1)
            continue;
        usedArticleIds.remove(topic.id);
        topic.id = headingCandidates.first().id;
        topic.boundarySource = QStringLiteral("streamDateMarkerHeadingValidated");
        usedArticleIds.insert(topic.id);
    }

    // A small number of records have no usable date marker in the MVB stream at all,
    // although their title and body are intact.  Recover only a title that appears in
    // exactly one extracted text fragment; generic or repeated titles stay catalog-only
    // so that an uncertain boundary is never fabricated.
    QList<QString> normalizedFragments;
    normalizedFragments.reserve(fragments.size());
    for (const TextFragment &fragment : fragments)
        normalizedFragments.append(normalizedTitleText(fragment.text));

    QHash<int, QList<ArticleMetadata>> uniqueTitleCandidates;
    for (auto item = metadata.constBegin(); item != metadata.constEnd(); ++item) {
        const ArticleMetadata &article = item.value();
        if (usedArticleIds.contains(article.id))
            continue;
        const QString title = normalizedTitleText(article.title);
        if (title.size() < 8)
            continue;

        int matchFragment = -1;
        for (int index = 0; index < normalizedFragments.size(); ++index) {
            if (!normalizedFragments.at(index).contains(title))
                continue;
            if (matchFragment >= 0) {
                matchFragment = -2;
                break;
            }
            matchFragment = index;
        }
        if (matchFragment >= 0)
            uniqueTitleCandidates[matchFragment].append(article);
    }
    for (auto item = uniqueTitleCandidates.constBegin(); item != uniqueTitleCandidates.constEnd(); ++item) {
        if (item.value().size() != 1)
            continue;
        const ArticleMetadata &article = item.value().first();
        topics.append({article.id, item.key(), fragments.size(),
                       QStringLiteral("metadataTitleUniqueMatch")});
        usedArticleIds.insert(article.id);
    }

    // Repeated titles can still be safely resolved when exactly one occurrence sits
    // between article boundaries from the record's publication month.  This excludes
    // the repeated table-of-contents material stored at the end of the MVB file.
    std::sort(topics.begin(), topics.end(), [](const ArticleTopic &left, const ArticleTopic &right) {
        return left.firstFragment < right.firstFragment;
    });
    const auto publicationMonth = [](const ArticleMetadata &article) {
        return article.date.size() >= 7 ? article.date.left(7) : QString();
    };
    const auto isMonthCompatible = [&](int fragment, const QString &month) {
        const auto next = std::lower_bound(topics.constBegin(), topics.constEnd(), fragment,
            [](const ArticleTopic &topic, int value) { return topic.firstFragment < value; });
        const QString before = next == topics.constBegin() ? QString()
            : publicationMonth(metadata.value((next - 1)->id));
        const QString after = next == topics.constEnd() ? QString()
            : publicationMonth(metadata.value(next->id));
        return (before.isEmpty() || before <= month) && (after.isEmpty() || month <= after);
    };

    QHash<int, QList<ArticleMetadata>> contextualTitleCandidates;
    for (auto item = metadata.constBegin(); item != metadata.constEnd(); ++item) {
        const ArticleMetadata &article = item.value();
        if (usedArticleIds.contains(article.id))
            continue;
        const QString title = normalizedTitleText(article.title);
        const QString month = publicationMonth(article);
        if (title.size() < 8 || month.isEmpty())
            continue;

        int compatibleFragment = -1;
        for (int index = 0; index < normalizedFragments.size(); ++index) {
            if (!normalizedFragments.at(index).contains(title) || !isMonthCompatible(index, month))
                continue;
            if (compatibleFragment >= 0) {
                compatibleFragment = -2;
                break;
            }
            compatibleFragment = index;
        }
        if (compatibleFragment >= 0)
            contextualTitleCandidates[compatibleFragment].append(article);
    }
    for (auto item = contextualTitleCandidates.constBegin(); item != contextualTitleCandidates.constEnd(); ++item) {
        if (item.value().size() != 1)
            continue;
        const ArticleMetadata &article = item.value().first();
        topics.append({article.id, item.key(), fragments.size(),
                       QStringLiteral("metadataTitleMonthContextMatch")});
        usedArticleIds.insert(article.id);
    }

    // "새책" is deliberately a generic catalog title, so it cannot be matched
    // by title alone.  A small number of its pages are identifiable from a
    // distinctive consecutive book-list structure (several page-count and
    // author/publisher lines).  Accept it only when one unused catalog record
    // and one such unassigned stream marker exist in the same month.
    const QRegularExpression pageCountExpression(QStringLiteral("\\d+\\s*쪽"));
    const auto isNewBookList = [&](int markerFragment) {
        int pageCountLines = 0;
        int creditLines = 0;
        const int last = std::min(markerFragment + 28, fragments.size());
        for (int index = markerFragment + 1; index < last; ++index) {
            const QString &text = fragments.at(index).text;
            if (pageCountExpression.match(text).hasMatch())
                ++pageCountLines;
            if (text.contains(QStringLiteral("지음")) || text.contains(QStringLiteral("출판")))
                ++creditLines;
        }
        return pageCountLines >= 2 && creditLines >= 2;
    };
    for (auto item = unresolvedByMonth.constBegin(); item != unresolvedByMonth.constEnd(); ++item) {
        QList<ArticleMetadata> newBookRecords;
        for (const ArticleMetadata &article : metadataByMonth.value(item.key())) {
            if (!usedArticleIds.contains(article.id)
                && normalizedTitleText(article.title) == normalizedTitleText(QStringLiteral("새책")))
                newBookRecords.append(article);
        }
        QList<StreamMarker> newBookMarkers;
        for (const StreamMarker &marker : item.value()) {
            if (isNewBookList(marker.fragment))
                newBookMarkers.append(marker);
        }
        if (newBookRecords.size() != 1 || newBookMarkers.size() != 1)
            continue;
        topics.append({newBookRecords.first().id, newBookMarkers.first().fragment, fragments.size(),
                       QStringLiteral("streamNewBookContentSignature")});
        usedArticleIds.insert(newBookRecords.first().id);
    }

    if (topics.isEmpty())
        return findNavigationArticleTopics(fragments);
    std::sort(topics.begin(), topics.end(), [](const ArticleTopic &left, const ArticleTopic &right) {
        return left.firstFragment < right.firstFragment;
    });
    for (int index = 0; index + 1 < topics.size(); ++index)
        topics[index].afterLastFragment = topics[index + 1].firstFragment;
    return topics;
}

QList<ArticleTopic> findCrossBookArticleTopics(const QList<TextFragment> &fragments,
                                               const QHash<QString, ArticleMetadata> &metadata,
                                               const QSet<QString> &eligibleArticleIds)
{
    QList<ArticleTopic> topics = findNavigationArticleTopics(fragments);
    if (eligibleArticleIds.isEmpty())
        return topics;

    // A few articles were stored in a different MVB book than their HEADA
    // catalog entry.  Do not attempt broad title recovery here: accept only an
    // as-yet-unreconstructed DBF record whose complete normalized title occurs
    // in exactly one fragment of this book.  This preserves an auditable,
    // unambiguous boundary for a cross-book source.
    QList<QString> normalizedFragments;
    normalizedFragments.reserve(fragments.size());
    for (const TextFragment &fragment : fragments)
        normalizedFragments.append(normalizedTitleText(fragment.text));

    struct CatalogTitle { QString id; QString date; QString title; };
    QList<CatalogTitle> catalogTitles;
    for (auto item = metadata.constBegin(); item != metadata.constEnd(); ++item) {
        const QString title = normalizedTitleText(item.value().title);
        if (title.size() >= 8)
            catalogTitles.append({item.key(), item.value().date, title});
    }

    QSet<QString> usedArticleIds;
    for (const ArticleTopic &topic : topics)
        usedArticleIds.insert(topic.id);
    for (const QString &id : eligibleArticleIds) {
        if (usedArticleIds.contains(id) || !metadata.contains(id))
            continue;
        const QString title = normalizedTitleText(metadata.value(id).title);
        if (title.size() < 12)
            continue;

        int matchFragment = -1;
        for (int index = 0; index < normalizedFragments.size(); ++index) {
            if (!normalizedFragments.at(index).contains(title))
                continue;
            if (matchFragment >= 0) {
                matchFragment = -2;
                break;
            }
            matchFragment = index;
        }
        if (matchFragment < 0)
            continue;
        // A cross-book title does not have the HEADA date marker that normally
        // supplies the end boundary.  Stop at the first later, uniquely named
        // catalog heading in the same chronological stream.  If no such next
        // heading exists, retain the record as unresolved rather than exposing
        // the remainder of the book as one article.
        int afterLastFragment = -1;
        for (int index = matchFragment + 1; index < normalizedFragments.size(); ++index) {
            QList<QString> nextIds;
            for (const CatalogTitle &candidate : catalogTitles) {
                if (candidate.id == id || candidate.date <= metadata.value(id).date
                    || !normalizedFragments.at(index).contains(candidate.title))
                    continue;
                nextIds.append(candidate.id);
            }
            if (nextIds.size() == 1) {
                afterLastFragment = index;
                break;
            }
        }
        if (afterLastFragment < 0)
            continue;
        topics.append({id, matchFragment, afterLastFragment,
                       QStringLiteral("crossBookMetadataTitleUniqueMatch")});
        usedArticleIds.insert(id);
    }

    std::sort(topics.begin(), topics.end(), [](const ArticleTopic &left, const ArticleTopic &right) {
        return left.firstFragment < right.firstFragment;
    });
    for (int index = 0; index + 1 < topics.size(); ++index) {
        const int nextStart = topics[index + 1].firstFragment;
        if (topics[index].afterLastFragment <= topics[index].firstFragment
            || topics[index].afterLastFragment > nextStart)
            topics[index].afterLastFragment = nextStart;
    }
    return topics;
}

struct MvbTopicCatalog {
    QList<quint32> offsets;
    QString error;
};

struct MvbInternalStream {
    qint64 dataOffset = 0;
    qint64 usedSize = 0;
};

struct MvbInternalDirectory {
    QHash<QString, MvbInternalStream> streams;
    QString error;
};

struct MvbTopicHeader {
    quint32 topicOffset = 0;
    quint32 nextTopicOffset = 0;
    quint32 streamPosition = 0;
    quint32 topicNumber = 0;
    QString title;
    QString bodyPreview;
    quint32 bodyTextBytes = 0;
    int phraseCompressedRecords = 0;
    int bodyReadErrors = 0;
};

struct MvbDecodedTopics {
    QList<MvbTopicHeader> headers;
    QList<quint32> bodyReadErrorPositions;
    QString error;
};

bool rangeIsValid(const QByteArray &data, qint64 offset, qint64 length)
{
    return offset >= 0 && length >= 0 && offset <= data.size() && length <= data.size() - offset;
}

quint16 littleEndianWord(const QByteArray &data, qint64 offset)
{
    const auto *bytes = reinterpret_cast<const unsigned char *>(data.constData() + offset);
    return static_cast<quint16>(bytes[0]) | (static_cast<quint16>(bytes[1]) << 8);
}

quint32 littleEndianDword(const QByteArray &data, qint64 offset)
{
    const auto *bytes = reinterpret_cast<const unsigned char *>(data.constData() + offset);
    return static_cast<quint32>(bytes[0]) | (static_cast<quint32>(bytes[1]) << 8)
           | (static_cast<quint32>(bytes[2]) << 16) | (static_cast<quint32>(bytes[3]) << 24);
}

MvbInternalDirectory readMvbInternalDirectory(const QByteArray &data)
{
    MvbInternalDirectory result;
    constexpr qint64 helpHeaderSize = 16;
    constexpr qint64 fileHeaderSize = 9;
    constexpr qint64 btreeHeaderSize = 38;
    constexpr qint64 nodeHeaderSize = 8;
    constexpr quint32 helpMagic = 0x00035f3f;
    constexpr quint16 btreeMagic = 0x293b;
    if (!rangeIsValid(data, 0, helpHeaderSize) || littleEndianDword(data, 0) != helpMagic) {
        result.error = QStringLiteral("not a WinHelp/MVB container");
        return result;
    }

    const qint64 directoryOffset = littleEndianDword(data, 4);
    if (!rangeIsValid(data, directoryOffset, fileHeaderSize + btreeHeaderSize)) {
        result.error = QStringLiteral("invalid internal directory offset");
        return result;
    }
    const qint64 directoryDataOffset = directoryOffset + fileHeaderSize;
    if (littleEndianWord(data, directoryDataOffset) != btreeMagic) {
        result.error = QStringLiteral("invalid internal directory signature");
        return result;
    }
    const qint64 pageSize = littleEndianWord(data, directoryDataOffset + 4);
    const int rootPage = static_cast<qint16>(littleEndianWord(data, directoryDataOffset + 26));
    const int levels = static_cast<qint16>(littleEndianWord(data, directoryDataOffset + 32));
    const int totalPages = static_cast<qint16>(littleEndianWord(data, directoryDataOffset + 30));
    const qint64 pagesOffset = directoryDataOffset + btreeHeaderSize;
    if (pageSize <= 0 || levels <= 0 || rootPage < 0 || rootPage >= totalPages
        || !rangeIsValid(data, pagesOffset, pageSize * static_cast<qint64>(totalPages))) {
        result.error = QStringLiteral("invalid internal directory layout");
        return result;
    }

    int page = rootPage;
    for (int level = 1; level < levels; ++level) {
        const qint64 pageOffset = pagesOffset + pageSize * page;
        if (!rangeIsValid(data, pageOffset, 6)) {
            result.error = QStringLiteral("invalid directory index page");
            return result;
        }
        page = static_cast<qint16>(littleEndianWord(data, pageOffset + 4));
        if (page < 0 || page >= totalPages) {
            result.error = QStringLiteral("invalid directory child page");
            return result;
        }
    }

    QSet<int> visitedPages;
    while (page >= 0 && page < totalPages && !visitedPages.contains(page)) {
        visitedPages.insert(page);
        const qint64 pageOffset = pagesOffset + pageSize * page;
        if (!rangeIsValid(data, pageOffset, nodeHeaderSize)) {
            result.error = QStringLiteral("invalid directory leaf page");
            return result;
        }
        const int entries = static_cast<qint16>(littleEndianWord(data, pageOffset + 2));
        const int nextPage = static_cast<qint16>(littleEndianWord(data, pageOffset + 6));
        if (entries < 0 || nextPage < -1 || nextPage >= totalPages) {
            result.error = QStringLiteral("invalid directory leaf metadata");
            return result;
        }
        qint64 cursor = pageOffset + nodeHeaderSize;
        const qint64 pageEnd = pageOffset + pageSize;
        for (int entry = 0; entry < entries; ++entry) {
            const qint64 zero = data.indexOf('\0', cursor);
            if (zero < cursor || zero + 5 > pageEnd) {
                result.error = QStringLiteral("invalid directory leaf entry");
                return result;
            }
            const qint64 fileOffset = littleEndianDword(data, zero + 1);
            if (!rangeIsValid(data, fileOffset, fileHeaderSize)) {
                result.error = QStringLiteral("invalid internal stream offset");
                return result;
            }
            const qint64 usedSize = littleEndianDword(data, fileOffset + 4);
            if (!rangeIsValid(data, fileOffset + fileHeaderSize, usedSize)) {
                result.error = QStringLiteral("invalid internal stream size");
                return result;
            }
            const QString name = QString::fromLatin1(data.constData() + cursor, zero - cursor);
            result.streams.insert(name, {fileOffset + fileHeaderSize, usedSize});
            cursor = zero + 5;
        }
        page = nextPage;
    }
    if (page != -1)
        result.error = QStringLiteral("cyclic directory leaf chain");
    return result;
}

MvbTopicCatalog extractMvbTopicCatalog(const QByteArray &data)
{
    // The MVB container uses the same internal B+ tree directory as WinHelp.
    // This intentionally reads only the directory and |CATALOG| stream: topic
    // text remains with the existing conservative extractor until its rich-text
    // record format is decoded as well.
    MvbTopicCatalog result;
    constexpr qint64 catalogHeaderSize = 40;
    const MvbInternalDirectory directory = readMvbInternalDirectory(data);
    if (!directory.error.isEmpty()) {
        result.error = directory.error;
        return result;
    }
    const auto catalogFile = directory.streams.constFind(QStringLiteral("|CATALOG"));
    if (catalogFile == directory.streams.constEnd()) {
        result.error = QStringLiteral("no |CATALOG stream");
        return result;
    }
    const MvbInternalStream catalog = catalogFile.value();
    if (!rangeIsValid(data, catalog.dataOffset, catalogHeaderSize)) {
        result.error = QStringLiteral("invalid |CATALOG stream data");
        return result;
    }
    const qint64 entries = littleEndianDword(data, catalog.dataOffset + 6);
    if (littleEndianWord(data, catalog.dataOffset) != 0x1111
        || catalog.usedSize < catalogHeaderSize + entries * 4) {
        result.error = QStringLiteral("invalid |CATALOG stream layout");
        return result;
    }
    result.offsets.reserve(entries);
    for (qint64 index = 0; index < entries; ++index)
        result.offsets.append(littleEndianDword(data, catalog.dataOffset + catalogHeaderSize + index * 4));
    return result;
}

QByteArray decompressMvbLz77(const QByteArray &compressed, int outputLimit, bool *ok)
{
    QByteArray output;
    QByteArray window(4096, '\0');
    int windowPosition = 0;
    int inputPosition = 0;
    unsigned int mask = 0;
    unsigned char flags = 0;
    *ok = false;
    while (inputPosition < compressed.size()) {
        if (mask == 0) {
            flags = static_cast<unsigned char>(compressed.at(inputPosition++));
            mask = 1;
            continue;
        }
        if (output.size() >= outputLimit)
            return {};
        unsigned char value = 0;
        if (flags & mask) {
            if (inputPosition + 1 >= compressed.size())
                return {};
            const quint16 reference = static_cast<unsigned char>(compressed.at(inputPosition))
                | (static_cast<quint16>(static_cast<unsigned char>(compressed.at(inputPosition + 1))) << 8);
            inputPosition += 2;
            const int length = (reference >> 12) + 3;
            int source = windowPosition - (reference & 0x0fff) - 1;
            if (source < 0)
                source += 4096;
            if (output.size() + length > outputLimit)
                return {};
            for (int index = 0; index < length; ++index) {
                value = static_cast<unsigned char>(window.at(source));
                source = (source + 1) & 0x0fff;
                output.append(static_cast<char>(value));
                window[windowPosition] = static_cast<char>(value);
                windowPosition = (windowPosition + 1) & 0x0fff;
            }
        } else {
            if (inputPosition >= compressed.size())
                return {};
            value = static_cast<unsigned char>(compressed.at(inputPosition++));
            output.append(static_cast<char>(value));
            window[windowPosition] = static_cast<char>(value);
            windowPosition = (windowPosition + 1) & 0x0fff;
        }
        mask <<= 1;
        if (mask == 0x100)
            mask = 0;
    }
    *ok = true;
    return output;
}

quint32 readCompressedWord(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return 0;
    const unsigned char first = static_cast<unsigned char>(bytes.at(0));
    if (!(first & 1))
        return first >> 1;
    if (bytes.size() < 2)
        return 0;
    return (static_cast<quint32>(static_cast<unsigned char>(bytes.at(1))) << 7) | (first >> 1);
}

QString decodeMvbInlineText(const QByteArray &bytes, QTextCodec *codec)
{
    QString output;
    const QSet<quint16> &cp949Pairs = validCp949Pairs();
    for (int index = 0; index < bytes.size();) {
        const unsigned char byte = static_cast<unsigned char>(bytes.at(index));
        if (byte == '\r' || byte == '\n' || byte == '\t') {
            output.append(QLatin1Char(' '));
            ++index;
        } else if (byte >= 0x20 && byte <= 0x7e) {
            output.append(QChar(byte));
            ++index;
        } else if (byte >= 0x81 && byte <= 0xfe && index + 1 < bytes.size()
                   && cp949Pairs.contains(static_cast<quint16>((byte << 8)
                                                                 | static_cast<unsigned char>(bytes.at(index + 1))))) {
            output.append(codec->toUnicode(bytes.mid(index, 2)));
            index += 2;
        } else {
            if (!output.isEmpty() && !output.endsWith(QLatin1Char(' ')))
                output.append(QLatin1Char(' '));
            ++index;
        }
    }
    return output.simplified();
}

MvbDecodedTopics decodeMvbTopicHeaders(const QByteArray &data)
{
    MvbDecodedTopics result;
    constexpr qint64 systemHeaderSize = 12;
    constexpr qint64 topicBlockHeaderSize = 12;
    constexpr qint64 topicLinkSize = 21;
    constexpr qint64 topicHeaderDataSize = 28;
    constexpr int decompressedBlockSize = 0x4000;
    const MvbInternalDirectory directory = readMvbInternalDirectory(data);
    if (!directory.error.isEmpty()) {
        result.error = directory.error;
        return result;
    }
    const auto systemIt = directory.streams.constFind(QStringLiteral("|SYSTEM"));
    const auto topicIt = directory.streams.constFind(QStringLiteral("|TOPIC"));
    if (systemIt == directory.streams.constEnd() || topicIt == directory.streams.constEnd()) {
        result.error = QStringLiteral("missing |SYSTEM or |TOPIC stream");
        return result;
    }
    const MvbInternalStream system = systemIt.value();
    const MvbInternalStream topic = topicIt.value();
    if (system.usedSize < systemHeaderSize || !rangeIsValid(data, system.dataOffset, systemHeaderSize)) {
        result.error = QStringLiteral("invalid |SYSTEM stream");
        return result;
    }
    const quint16 systemMinor = littleEndianWord(data, system.dataOffset + 2);
    const quint16 systemFlags = littleEndianWord(data, system.dataOffset + 10);
    if (systemMinor < 16) {
        result.error = QStringLiteral("unsupported pre-3.1 topic stream");
        return result;
    }
    const bool compressed = systemFlags == 4 || systemFlags == 8;
    const int physicalBlockSize = systemFlags == 8 ? 2048 : 4096;
    // TOPICPOS always advances in 16 KiB logical block slots in 3.1+ files.
    // For an uncompressed physical block only the first 4,084 bytes are present;
    // the unused part of the logical slot is skipped when a record crosses blocks.
    const int topicPositionBlockSize = decompressedBlockSize;
    if (topic.usedSize < topicBlockHeaderSize) {
        result.error = QStringLiteral("invalid |TOPIC block layout");
        return result;
    }

    QList<QByteArray> blocks;
    const int physicalBlockCount = (topic.usedSize + physicalBlockSize - 1) / physicalBlockSize;
    blocks.reserve(physicalBlockCount);
    for (int index = 0; index < physicalBlockCount; ++index) {
        const qint64 physicalOffset = topic.dataOffset + static_cast<qint64>(index) * physicalBlockSize;
        const int physicalBytes = std::min<qint64>(physicalBlockSize,
                                                   topic.usedSize - static_cast<qint64>(index) * physicalBlockSize);
        if (physicalBytes <= topicBlockHeaderSize) {
            result.error = QStringLiteral("truncated |TOPIC block %1").arg(index);
            return result;
        }
        const QByteArray payload = data.mid(physicalOffset + topicBlockHeaderSize,
                                            physicalBytes - topicBlockHeaderSize);
        bool decompressed = false;
        const QByteArray logical = compressed ? decompressMvbLz77(payload, decompressedBlockSize, &decompressed)
                                               : payload;
        if (!decompressed && compressed) {
            result.error = QStringLiteral("invalid |TOPIC LZ77 block %1").arg(index);
            return result;
        }
        blocks.append(logical);
    }
    const auto advanceTopicPosition = [&blocks, topicPositionBlockSize](quint32 position, quint32 length, bool *ok) {
        *ok = false;
        if (position < topicBlockHeaderSize)
            return quint32(0);
        quint32 cursor = position;
        quint32 remaining = length;
        while (remaining > 0) {
            const quint32 block = (cursor - topicBlockHeaderSize) / topicPositionBlockSize;
            if (block >= static_cast<quint32>(blocks.size()))
                return quint32(0);
            const quint32 offset = (cursor - topicBlockHeaderSize) % topicPositionBlockSize;
            const quint32 blockDataSize = blocks.at(block).size();
            if (offset >= blockDataSize)
                return quint32(0);
            const quint32 count = std::min(remaining, blockDataSize - offset);
            remaining -= count;
            cursor += count;
            // LinkData2 is read as a continuation after LinkData1.  A LinkData1
            // ending exactly on a physical payload boundary must continue at
            // the next 16 KiB logical slot rather than at the unused tail.
            if (remaining > 0 || offset + count == blockDataSize)
                cursor = (block + 1) * topicPositionBlockSize + topicBlockHeaderSize;
        }
        *ok = true;
        return cursor;
    };
    const auto readTopic = [&blocks, topicPositionBlockSize](quint32 position, quint32 length, bool *ok) {
        QByteArray output;
        *ok = false;
        if (position < topicBlockHeaderSize)
            return output;
        output.reserve(length);
        quint32 cursor = position;
        quint32 remaining = length;
        while (remaining > 0) {
            const quint32 block = (cursor - topicBlockHeaderSize) / topicPositionBlockSize;
            if (block >= static_cast<quint32>(blocks.size()))
                return QByteArray();
            const quint32 blockDataSize = blocks.at(block).size();
            const quint32 offset = (cursor - topicBlockHeaderSize) % topicPositionBlockSize;
            if (offset >= blockDataSize)
                return QByteArray();
            const quint32 count = std::min(remaining,
                                           blockDataSize - offset);
            output.append(blocks.at(block).constData() + offset, count);
            remaining -= count;
            if (remaining > 0 && count == 0)
                return QByteArray();
            if (remaining > 0)
                cursor = (block + 1) * topicPositionBlockSize + topicBlockHeaderSize;
        }
        *ok = true;
        return output;
    };

    QTextCodec *codec = QTextCodec::codecForName("CP949");
    if (!codec)
        codec = QTextCodec::codecForLocale();
    quint32 position = topicBlockHeaderSize;
    quint32 logicalOffset = 0;
    QSet<quint32> visitedPositions;
    bool terminated = false;
    int currentHeader = -1;
    while (position >= topicBlockHeaderSize && !visitedPositions.contains(position)) {
        visitedPositions.insert(position);
        bool readOk = false;
        const QByteArray link = readTopic(position, topicLinkSize, &readOk);
        if (!readOk || link.size() != topicLinkSize) {
            result.error = QStringLiteral("invalid |TOPIC link at 0x%1").arg(position, 0, 16);
            return result;
        }
        const quint32 blockSize = littleEndianDword(link, 0);
        const quint32 dataLength2 = littleEndianDword(link, 4);
        const quint32 nextPosition = littleEndianDword(link, 12);
        const quint32 dataLength1 = littleEndianDword(link, 16);
        const unsigned char recordType = static_cast<unsigned char>(link.at(20));
        if (dataLength1 < topicLinkSize || blockSize < dataLength1 || blockSize > 0x100000
            || nextPosition == position) {
            result.error = QStringLiteral("invalid |TOPIC record at 0x%1").arg(position, 0, 16);
            return result;
        }
        const QByteArray data1 = readTopic(position + topicLinkSize, dataLength1 - topicLinkSize, &readOk);
        if (!readOk) {
            result.error = QStringLiteral("truncated |TOPIC record data at 0x%1").arg(position, 0, 16);
            return result;
        }
        if (recordType == 0x02 && data1.size() >= topicHeaderDataSize) {
            MvbTopicHeader header;
            header.topicOffset = logicalOffset;
            header.streamPosition = position;
            header.topicNumber = littleEndianDword(data1, 12);
            header.nextTopicOffset = littleEndianDword(data1, 24);
            const quint32 storedLength2 = blockSize - dataLength1;
            const quint32 data2Position = advanceTopicPosition(position + topicLinkSize,
                                                                dataLength1 - topicLinkSize, &readOk);
            const QByteArray data2 = readOk ? readTopic(data2Position, storedLength2, &readOk) : QByteArray();
            if (!readOk) {
                result.error = QStringLiteral("truncated |TOPIC title at 0x%1").arg(position, 0, 16);
                return result;
            }
            // Old phrase compression advertises a larger logical DataLen2 than the stored bytes.
            // Preserve its boundary now; phrase expansion is handled by the body parser stage.
            if (dataLength2 <= static_cast<quint32>(data2.size())) {
                const int nul = data2.indexOf('\0');
                header.title = codec->toUnicode(data2.left(nul < 0 ? dataLength2 : nul));
            }
            result.headers.append(header);
            currentHeader = result.headers.size() - 1;
        } else if (recordType == 0x20 || recordType == 0x23) {
            logicalOffset += readCompressedWord(data1);
            if (currentHeader >= 0) {
                const quint32 storedLength2 = blockSize - dataLength1;
                const quint32 data2Position = advanceTopicPosition(position + topicLinkSize,
                                                                    dataLength1 - topicLinkSize, &readOk);
                const QByteArray data2 = readOk ? readTopic(data2Position, storedLength2, &readOk) : QByteArray();
                MvbTopicHeader &header = result.headers[currentHeader];
                if (!readOk) {
                    ++header.bodyReadErrors;
                    result.bodyReadErrorPositions.append(position);
                } else {
                    header.bodyTextBytes += dataLength2;
                    if (dataLength2 > static_cast<quint32>(data2.size())) {
                        ++header.phraseCompressedRecords;
                    } else if (header.bodyPreview.size() < 1200) {
                        const QString text = decodeMvbInlineText(data2.left(dataLength2), codec);
                        if (!text.isEmpty()) {
                            if (!header.bodyPreview.isEmpty())
                                header.bodyPreview.append(QLatin1Char(' '));
                            header.bodyPreview.append(text.left(1200 - header.bodyPreview.size()));
                        }
                    }
                }
            }
        }
        if (nextPosition == 0 || nextPosition == 0xffffffff) {
            terminated = true;
            break;
        }
        const quint32 currentBlock = (position - topicBlockHeaderSize) / topicPositionBlockSize;
        const quint32 nextBlock = (nextPosition - topicBlockHeaderSize) / topicPositionBlockSize;
        if (nextBlock != currentBlock)
            logicalOffset = nextBlock * 0x8000;
        position = nextPosition;
    }
    if (!terminated && visitedPositions.contains(position))
        result.error = QStringLiteral("cyclic |TOPIC link chain");
    return result;
}

QJsonObject validateArticleTitlesAgainstMvbTopics(const QList<MvbTopicHeader> &headers,
                                                  const QHash<QString, ArticleMetadata> &metadata,
                                                  const QList<ArticleTopic> &articles)
{
    QHash<QString, QList<const MvbTopicHeader *>> headersByTitle;
    for (const MvbTopicHeader &header : headers) {
        const QString title = normalizedTitleText(header.title);
        if (title.size() >= 4)
            headersByTitle[title].append(&header);
    }
    QHash<QString, QStringList> articleIdsByTitle;
    for (auto item = metadata.constBegin(); item != metadata.constEnd(); ++item) {
        const QString title = normalizedTitleText(item.value().title);
        if (title.size() >= 4)
            articleIdsByTitle[title].append(item.key());
    }
    QSet<QString> reconstructedIds;
    for (const ArticleTopic &article : articles)
        reconstructedIds.insert(article.id);

    QJsonArray matches;
    int validatedReconstructed = 0;
    for (auto item = articleIdsByTitle.constBegin(); item != articleIdsByTitle.constEnd(); ++item) {
        const QList<const MvbTopicHeader *> topicMatches = headersByTitle.value(item.key());
        if (item.value().size() != 1 || topicMatches.size() != 1)
            continue;
        const QString id = item.value().first();
        const MvbTopicHeader *header = topicMatches.first();
        QJsonObject match;
        match.insert(QStringLiteral("articleId"), id);
        match.insert(QStringLiteral("topicOffset"), static_cast<double>(header->topicOffset));
        match.insert(QStringLiteral("topicNumber"), static_cast<double>(header->topicNumber));
        match.insert(QStringLiteral("title"), header->title);
        match.insert(QStringLiteral("reconstructedArticle"), reconstructedIds.contains(id));
        if (reconstructedIds.contains(id))
            ++validatedReconstructed;
        matches.append(match);
    }
    QJsonObject result;
    result.insert(QStringLiteral("uniqueTitleMatchCount"), matches.size());
    result.insert(QStringLiteral("validatedReconstructedArticleCount"), validatedReconstructed);
    result.insert(QStringLiteral("matches"), matches);
    result.insert(QStringLiteral("note"),
                  QStringLiteral("Validation only: original topic offsets are retained separately and do not replace marker-based article boundaries."));
    return result;
}

AssetCatalog createAssetCatalog(const QString &assetRoot, const QString &outputDirectory, bool copyAssets)
{
    AssetCatalog catalog;
    catalog.outputDirectory = outputDirectory;
    catalog.copyAssets = copyAssets;
    if (assetRoot.isEmpty())
        return catalog;

    QDir root(QFileInfo(assetRoot).absoluteFilePath());
    QDirIterator iterator(root.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString source = iterator.next();
        const QString relative = normalizedPath(root.relativeFilePath(source));
        catalog.sourceByLowercasePath.insert(relative.toLower(), source);
    }
    return catalog;
}

QString imageHref(const AssetCatalog &catalog, const QString &reference, const QString &topicDirectory)
{
    QString assetKey = normalizedPath(reference).toLower();
    QString source = catalog.sourceByLowercasePath.value(assetKey);
    if (source.isEmpty() && !assetKey.contains(QLatin1Char('/'))) {
        for (auto item = catalog.sourceByLowercasePath.constBegin(); item != catalog.sourceByLowercasePath.constEnd(); ++item) {
            if (item.key().endsWith(QLatin1Char('/') + assetKey)) {
                assetKey = item.key();
                source = item.value();
                break;
            }
        }
    }
    if (source.isEmpty())
        return {};
    if (!catalog.copyAssets)
        return QDir(topicDirectory).relativeFilePath(source);

    const QString destination = QDir(catalog.outputDirectory).filePath(
        QStringLiteral("assets/") + assetKey);
    QDir().mkpath(QFileInfo(destination).absolutePath());
    if (!QFile::exists(destination) && !QFile::copy(source, destination))
        return QUrl::fromLocalFile(source).toString();
    return QDir(topicDirectory).relativeFilePath(destination);
}

QStringList findMonthTopicIds(const QList<TextFragment> &fragments)
{
    const QRegularExpression target(QStringLiteral("JumpID\\([^\\r\\n]{0,120}[`'](Y[0-9]{4})[`']"),
                                    QRegularExpression::CaseInsensitiveOption);
    QSet<QString> ids;
    for (const TextFragment &fragment : fragments) {
        for (auto match = target.globalMatch(fragment.text); match.hasNext();)
            ids.insert(match.next().captured(1).toUpper());
    }
    QStringList result = ids.values();
    result.sort();
    return result;
}

QString topicHref(const QString &text, const QString &currentBook, const QString &currentTopic)
{
    const QRegularExpression articleTarget(
        QStringLiteral("(?:PopupID|JumpID)\\(\\s*[`']?([A-Za-z0-9_]+)[^,]*,\\s*[`']?h([0-9]{8})>mokcha"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch articleMatch = articleTarget.match(text);
    if (articleMatch.hasMatch()) {
        const QString targetBook = articleMatch.captured(1).toUpper();
        const QString fileName = articleMatch.captured(2) + QStringLiteral(".html");
        if (targetBook == currentBook.toUpper() && articleMatch.captured(2) == currentTopic)
            return {};
        return targetBook == currentBook.toUpper() ? fileName
                                                     : QStringLiteral("../") + targetBook + QStringLiteral("/") + fileName;
    }

    const QRegularExpression monthTarget(
        QStringLiteral("JumpID\\(\\s*[`']?([A-Za-z0-9_]+)[^,]*,\\s*[`']?(Y[0-9]{4})[`']"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch monthMatch = monthTarget.match(text);
    if (!monthMatch.hasMatch())
        return {};
    const QString targetBook = monthMatch.captured(1).toUpper();
    const QString fileName = monthMatch.captured(2).toUpper() + QStringLiteral(".html");
    return targetBook == currentBook.toUpper() ? fileName
                                                 : QStringLiteral("../") + targetBook + QStringLiteral("/") + fileName;
}

QString monthLabel(const QString &topicId)
{
    return QStringLiteral("19") + topicId.mid(1, 2) + QStringLiteral(".") + topicId.mid(3, 2);
}

bool isHeadingLike(const QString &text)
{
    const QString value = text.simplified();
    if (value.size() < 2 || value.size() > 100 || value.contains(QStringLiteral("MVBMP2"))
        || value.contains(QLatin1Char('`')))
        return false;
    if (value.contains(QRegularExpression(QStringLiteral("[.!?]"))))
        return false;
    int KoreanCharacters = 0;
    for (const QChar character : value) {
        if (character.unicode() >= 0xac00 && character.unicode() <= 0xd7a3)
            ++KoreanCharacters;
    }
    return KoreanCharacters >= 2 || value.count(QRegularExpression(QStringLiteral("[A-Za-z]"))) >= 6;
}

QJsonArray renderTokensJson(const QList<TextFragment> &fragments)
{
    const QRegularExpression imageExpression(
        QStringLiteral("\\bbmp[\\\\/][A-Za-z0-9_-]+\\.(?:dib|bmp|shg)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression mediaExpression(
        QStringLiteral("\\b[A-Za-z0-9_.-]+\\.(?:avi|wav)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression macroExpression(
        QStringLiteral("\\b(?:PopupID|JumpID|JumpContext|PaneID|RegisterRoutine)\\("),
        QRegularExpression::CaseInsensitiveOption);
    QJsonArray output;
    for (const TextFragment &fragment : fragments) {
        RenderToken token;
        token.offset = fragment.offset;
        token.text = fragment.text;
        if (imageExpression.match(fragment.text).hasMatch()) {
            token.kind = QStringLiteral("imageReference");
            token.style = QStringLiteral("figure");
        } else if (mediaExpression.match(fragment.text).hasMatch()) {
            token.kind = QStringLiteral("mediaReference");
            token.style = QStringLiteral("media");
        } else if (macroExpression.match(fragment.text).hasMatch()) {
            token.kind = QStringLiteral("viewerMacro");
            token.style = QStringLiteral("macro");
        } else {
            token.kind = QStringLiteral("text");
            token.style = isHeadingLike(fragment.text) ? QStringLiteral("headingCandidate")
                                                       : QStringLiteral("paragraph");
        }
        QJsonObject item;
        item.insert(QStringLiteral("offset"), static_cast<double>(token.offset));
        item.insert(QStringLiteral("text"), token.text);
        item.insert(QStringLiteral("kind"), token.kind);
        item.insert(QStringLiteral("style"), token.style);
        output.append(item);
    }
    return output;
}

void writeCatalogRecordPage(const QString &topicDirectory, const ArticleMetadata &article)
{
    const QString recordDirectory = QDir(topicDirectory).filePath(QStringLiteral("records"));
    QDir().mkpath(recordDirectory);
    QFile pageFile(QDir(recordDirectory).filePath(article.id + QStringLiteral(".html")));
    if (!pageFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    QTextStream page(&pageFile);
    page.setCodec("UTF-8");
    const QString title = article.title.isEmpty() ? article.id : article.title;
    page << "<!doctype html><meta charset=\"utf-8\"><title>" << title.toHtmlEscaped()
         << "</title><style>body{font:16px/1.6 system-ui,sans-serif;margin:2rem;max-width:48rem}"
            ".meta{color:#56616d}.notice{padding:1rem;background:#fff8e6;border-left:4px solid #c78400}</style>\n";
    page << "<p><a href=\"../index.html\">&larr; article index</a></p><h1>" << title.toHtmlEscaped() << "</h1>\n";
    page << "<p class=\"meta\">ID " << article.id.toHtmlEscaped();
    if (!article.date.isEmpty()) page << " &middot; " << article.date.toHtmlEscaped();
    if (!article.section.isEmpty()) page << " &middot; " << article.section.toHtmlEscaped();
    if (!article.category.isEmpty()) page << " / " << article.category.toHtmlEscaped();
    page << "</p><p class=\"notice\">This article is indexed by the original DBF catalog, but its exact "
            "MVB content boundary has not yet been reconstructed. The displayed metadata is original catalog data."
            "</p>\n";
    if (!article.keywords.isEmpty())
        page << "<p><strong>Keywords:</strong> " << article.keywords.toHtmlEscaped() << "</p>\n";
}

void writeMonthIndexes(const QString &topicDirectory, const QStringList &monthTopics,
                       const QList<ArticleTopic> &topics, const QHash<QString, ArticleMetadata> &metadata)
{
    QSet<QString> articleIds;
    for (const ArticleTopic &topic : topics)
        articleIds.insert(topic.id);
    for (const QString &month : monthTopics) {
        struct Entry { QString id; ArticleMetadata metadata; };
        QList<Entry> entries;
        const QString datePrefix = monthLabel(month);
        for (auto item = metadata.constBegin(); item != metadata.constEnd(); ++item) {
            const ArticleMetadata article = item.value();
            if (!article.id.isEmpty() && article.date.startsWith(datePrefix))
                entries.append({article.id, article});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry &left, const Entry &right) {
            return left.id < right.id;
        });

        QFile pageFile(QDir(topicDirectory).filePath(month + QStringLiteral(".html")));
        if (!pageFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
            continue;
        QTextStream page(&pageFile);
        page.setCodec("UTF-8");
        page << "<!doctype html><meta charset=\"utf-8\"><title>" << monthLabel(month)
             << " article index</title><style>body{font:16px/1.5 system-ui,sans-serif;margin:2rem;max-width:72rem}</style>\n";
        page << "<p><a href=\"index.html\">&larr; article index</a></p><h1>" << monthLabel(month)
             << " article index</h1><ol>\n";
        for (const Entry &entry : entries) {
            const QString title = entry.metadata.title.isEmpty() ? entry.id : entry.metadata.title;
            const bool reconstructed = articleIds.contains(entry.id);
            if (!reconstructed)
                writeCatalogRecordPage(topicDirectory, entry.metadata);
            page << "<li><a href=\"" << (reconstructed ? entry.id + QStringLiteral(".html")
                                                              : QStringLiteral("records/") + entry.id + QStringLiteral(".html"))
                 << "\">" << title.toHtmlEscaped() << "</a> <small>"
                 << entry.metadata.section.toHtmlEscaped() << " / " << entry.metadata.category.toHtmlEscaped();
            if (!reconstructed)
                page << " &middot; DBF catalog record";
            page << "</small></li>\n";
        }
        page << "</ol>\n";
    }
}

bool isDecorativeImageReference(const QString &reference)
{
    const QString fileName = QFileInfo(reference).completeBaseName().toLower();
    return fileName.startsWith(QStringLiteral("ico")) || fileName.startsWith(QStringLiteral("jicon"));
}

bool isArticleMarker(const QString &text)
{
    static const QRegularExpression marker(QStringLiteral("^\\s*\\d{2}\\.\\d{2}\\.\\d{3,4}\\s*$"));
    return marker.match(text).hasMatch();
}

bool isViewerMacroFragment(const QString &text)
{
    static const QRegularExpression macro(
        QStringLiteral("\\b(?:PopupID|JumpID|JumpContext|PaneID|RegisterRoutine)\\("),
        QRegularExpression::CaseInsensitiveOption);
    return macro.match(text).hasMatch();
}

void writeReadableTopicBody(QTextStream &page, const QString &baseName, const QString &topicId,
                            const QList<TextFragment> &fragments, const ArticleTopic &topic,
                            const AssetCatalog &assets, const QString &topicDirectory)
{
    const QRegularExpression imageReference(QStringLiteral("\\bbmp[\\\\/][A-Za-z0-9_-]+\\.(?:dib|bmp|shg)"),
                                             QRegularExpression::CaseInsensitiveOption);
    QSet<QString> displayedImages;
    QStringList sourceNotes;
    bool previousWasFigure = false;

    page << "<main class=\"article-body\">\n";
    for (int fragment = topic.firstFragment; fragment < topic.afterLastFragment; ++fragment) {
        const TextFragment &item = fragments.at(fragment);
        const QRegularExpressionMatch imageMatch = imageReference.match(item.text);
        const QString target = topicHref(item.text, baseName, topicId);
        if (imageMatch.hasMatch()) {
            const QString reference = normalizedPath(imageMatch.captured());
            const QString imageKey = reference.toLower();
            if (isDecorativeImageReference(reference)) {
                sourceNotes.append(QStringLiteral("Decorative asset omitted from reading flow: ") + reference);
                previousWasFigure = false;
                continue;
            }
            if (displayedImages.contains(imageKey)) {
                sourceNotes.append(QStringLiteral("Repeated image omitted: ") + reference);
                previousWasFigure = false;
                continue;
            }
            displayedImages.insert(imageKey);
            const QString source = imageHref(assets, reference, topicDirectory);
            page << "<figure class=\"article-figure\" data-offset=\"" << item.offset << "\">";
            if (!target.isEmpty()) page << "<a href=\"" << target.toHtmlEscaped() << "\">";
            if (!source.isEmpty()) {
                page << "<img loading=\"lazy\" src=\"" << source.toHtmlEscaped()
                     << "\" alt=\"" << reference.toHtmlEscaped() << "\">";
            } else {
                page << "<div class=\"missing-asset\">Missing image: <code>" << reference.toHtmlEscaped()
                     << "</code></div>";
            }
            if (!target.isEmpty()) page << "</a>";
            page << "<figcaption class=\"source-label\">" << reference.toHtmlEscaped()
                 << "</figcaption></figure>\n";
            previousWasFigure = true;
            continue;
        }
        if (isArticleMarker(item.text)) {
            page << "<div class=\"recovered-marker\" data-offset=\"" << item.offset
                 << "\">Recovered item marker " << item.text.toHtmlEscaped() << "</div>\n";
            previousWasFigure = false;
            continue;
        }
        if (!target.isEmpty()) {
            page << "<nav class=\"related-topic\" data-offset=\"" << item.offset << "\"><a href=\""
                 << target.toHtmlEscaped() << "\">Open related article</a></nav>\n";
            previousWasFigure = false;
            continue;
        }
        if (isViewerMacroFragment(item.text)) {
            sourceNotes.append(item.text.simplified());
            previousWasFigure = false;
            continue;
        }
        if (isHeadingLike(item.text)) {
            if (previousWasFigure && item.text.simplified().size() <= 100) {
                page << "<p class=\"figure-caption\" data-offset=\"" << item.offset << "\">"
                     << item.text.toHtmlEscaped() << "</p>\n";
            } else {
                page << "<h2 class=\"article-heading\" data-offset=\"" << item.offset << "\">"
                     << item.text.toHtmlEscaped() << "</h2>\n";
            }
        } else {
            page << "<p class=\"article-text\" data-offset=\"" << item.offset << "\">"
                 << item.text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>\n")) << "</p>\n";
        }
        previousWasFigure = false;
    }
    page << "</main>\n";
    if (!sourceNotes.isEmpty()) {
        page << "<details class=\"source-notes\"><summary>Source notes (" << sourceNotes.size()
             << " omitted technical item(s))</summary><ul>\n";
        for (const QString &note : sourceNotes)
            page << "<li><code>" << note.left(500).toHtmlEscaped() << "</code></li>\n";
        page << "</ul></details>\n";
    }
}

void writeTopicPages(const QString &outputDirectory, const QString &baseName,
                     const QList<ArticleTopic> &topics, const QList<TextFragment> &fragments,
                     const QHash<QString, ArticleMetadata> &metadata, const AssetCatalog &assets)
{
    QDir root(outputDirectory);
    root.mkpath(QStringLiteral("topics/") + baseName);
    const QString topicDirectory = root.filePath(QStringLiteral("topics/") + baseName);
    QFile indexFile(QDir(topicDirectory).filePath(QStringLiteral("index.html")));
    if (!indexFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    QTextStream index(&indexFile);
    index.setCodec("UTF-8");
    index << "<!doctype html><meta charset=\"utf-8\"><title>" << baseName.toHtmlEscaped()
          << " topics</title><style>body{font:16px/1.5 system-ui,sans-serif;margin:2rem;max-width:72rem}</style>\n";
    index << "<h1>" << baseName.toHtmlEscaped() << " - reconstructed article units</h1><ol>\n";

    const QStringList monthTopics = findMonthTopicIds(fragments);
    if (!monthTopics.isEmpty()) {
        index << "<h2>Monthly indexes</h2><ul>\n";
        for (const QString &month : monthTopics)
            index << "<li><a href=\"" << month << ".html\">" << monthLabel(month) << "</a></li>\n";
        index << "</ul>\n";
    }
    writeMonthIndexes(topicDirectory, monthTopics, topics, metadata);

    for (const ArticleTopic &topic : topics) {
        ArticleMetadata fallback;
        fallback.id = topic.id;
        const ArticleMetadata article = metadata.value(topic.id, fallback);
        const QString title = article.title.isEmpty() ? topic.id : article.title;
        const QString fileName = topic.id + QStringLiteral(".html");
        index << "<li><a href=\"" << fileName << "\">" << title.toHtmlEscaped() << "</a>"
              << " <small>" << topic.id.toHtmlEscaped() << "</small></li>\n";

        QFile pageFile(QDir(topicDirectory).filePath(fileName));
        if (!pageFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
            continue;
        QTextStream page(&pageFile);
        page.setCodec("UTF-8");
        page << "<!doctype html><meta charset=\"utf-8\"><title>" << title.toHtmlEscaped()
             << "</title><style>"
                ":root{color-scheme:light}body{font:17px/1.75 system-ui,sans-serif;margin:0;background:#f4f6f8;color:#20242a}"
                ".page{box-sizing:border-box;max-width:76rem;margin:0 auto;padding:2rem 2.25rem 4rem;background:#fff;box-shadow:0 0 2rem #dbe1e8}"
                ".back{display:inline-block;margin-bottom:1.5rem;color:#175ca8;text-decoration:none}.back:hover{text-decoration:underline}"
                "h1{line-height:1.3;margin:0 0 .5rem}.meta{color:#5b6470;border-bottom:1px solid #dce2e8;padding-bottom:1.25rem;margin:0 0 2rem}"
                ".article-body{max-width:66rem}.article-text{margin:0 0 1.2rem;letter-spacing:.01em}"
                ".article-heading{font-size:1.35rem;line-height:1.4;margin:2.4rem 0 .7rem;padding-top:.15rem;color:#142c47}"
                ".article-figure{margin:1.8rem auto;max-width:100%;padding:1rem;background:#f7f9fb;border:1px solid #e0e6ec;border-radius:.5rem;text-align:center}"
                ".article-figure img{display:block;max-width:100%;max-height:34rem;height:auto;margin:auto;object-fit:contain}"
                ".source-label{margin-top:.65rem;color:#76808c;font:12px ui-monospace,monospace;text-align:center;word-break:break-all}"
                ".figure-caption{margin:-1rem auto 1.7rem;max-width:90%;text-align:center;color:#4b5563;font-size:.95rem}"
                ".related-topic{margin:1.25rem 0;padding:.8rem 1rem;border-left:4px solid #2b78c5;background:#eef6ff}.related-topic a{color:#125ba4;font-weight:600}"
                ".recovered-marker{margin:2.5rem 0 1.25rem;padding:.5rem .75rem;border-top:2px solid #7b8794;color:#52606d;font-size:.85rem;font-weight:600;letter-spacing:.04em}"
                ".missing-asset{padding:2rem;color:#8a2632;background:#fff3f3}.source-notes{margin-top:3rem;padding:1rem;background:#f7f9fb;border:1px solid #e0e6ec;color:#58616d;font-size:.85rem}.source-notes summary{cursor:pointer;font-weight:600}.source-notes code{white-space:pre-wrap;word-break:break-word}"
                "@media(max-width:700px){.page{padding:1.25rem 1rem;box-shadow:none}.article-figure{padding:.55rem}body{background:#fff}}</style>\n";
        page << "<div class=\"page\"><a class=\"back\" href=\"index.html\">&larr; article index</a><h1>" << title.toHtmlEscaped() << "</h1>\n";
        page << "<p class=\"meta\">ID " << topic.id.toHtmlEscaped();
        if (!article.date.isEmpty()) page << " &middot; " << article.date.toHtmlEscaped();
        if (!article.section.isEmpty()) page << " &middot; " << article.section.toHtmlEscaped();
        if (!article.category.isEmpty()) page << " / " << article.category.toHtmlEscaped();
        page << "</p>\n";
        writeReadableTopicBody(page, baseName, topic.id, fragments, topic, assets, topicDirectory);
        page << "</div>\n";
    }
    index << "</ol>\n";
}

ConversionSummary convertFile(const QString &inputPath, const QString &outputDirectory, int minimumBytes,
                              const QHash<QString, ArticleMetadata> &metadata, bool writeTopics, bool writeReviewHtml,
                              const AssetCatalog &assets, const QSet<QString> &crossBookCandidates,
                              QString *errorMessage)
{
    QFile input(inputPath);
    if (!input.open(QIODevice::ReadOnly)) {
        *errorMessage = QStringLiteral("Cannot read %1: %2").arg(inputPath, input.errorString());
        return {};
    }
    const QByteArray contents = input.readAll();
    const MvbTopicCatalog mvbTopicCatalog = extractMvbTopicCatalog(contents);
    const MvbDecodedTopics decodedMvbTopics = decodeMvbTopicHeaders(contents);
    const QList<TextFragment> fragments = extractText(contents, minimumBytes);
    const bool isHeada = QFileInfo(inputPath).completeBaseName().compare(QStringLiteral("HEADA"),
                                                                           Qt::CaseInsensitive) == 0;
    const QList<ArticleTopic> topics = isHeada ? findArticleTopics(fragments, metadata)
                                                : findCrossBookArticleTopics(fragments, metadata,
                                                                             crossBookCandidates);
    const QStringList monthTopics = findMonthTopicIds(fragments);

    // Latin-1 preserves every byte one-to-one, which is useful when matching ASCII Viewer macros.
    const QString byteText = QString::fromLatin1(contents.constData(), contents.size());
    QList<NavigationReference> navigation = extractNavigationReferences(byteText);
    for (NavigationReference &reference : navigation) {
        if (reference.function.compare(QStringLiteral("PopupID"), Qt::CaseInsensitive) == 0)
            reference.contextArticleId = articleContextForOffset(reference.offset, fragments, topics);
    }
    QSet<QString> images;
    QSet<QString> media;
    QSet<QString> macros;

    const QRegularExpression imageExpression(
        QStringLiteral("\\bbmp[\\\\/][A-Za-z0-9_-]+\\.(?:dib|bmp|shg)"),
        QRegularExpression::CaseInsensitiveOption);
    for (auto match = imageExpression.globalMatch(byteText); match.hasNext();)
        images.insert(normalizedPath(match.next().captured()));

    const QRegularExpression mediaExpression(
        QStringLiteral("\\b[A-Za-z0-9_.-]+\\.(?:avi|wav)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    for (auto match = mediaExpression.globalMatch(byteText); match.hasNext();)
        media.insert(normalizedPath(match.next().captured()));

    const QRegularExpression macroExpression(
        QStringLiteral("\\b(?:PopupID|JumpID|JumpContext|PaneID|RegisterRoutine)\\([^\\r\\n\\x00]{1,512}\\)"),
        QRegularExpression::CaseInsensitiveOption);
    for (auto match = macroExpression.globalMatch(byteText); match.hasNext();)
        macros.insert(match.next().captured().trimmed());

    // Topic pages only render a subset of the book. Copy every referenced image so
    // the converted archive remains portable as additional MVB topic types are added.
    QSet<QString> missingImages;
    QSet<QString> missingMedia;
    QSet<QString> availableMediaAssets;
    if (!assets.sourceByLowercasePath.isEmpty()) {
        for (auto item = assets.sourceByLowercasePath.constBegin(); item != assets.sourceByLowercasePath.constEnd(); ++item) {
            if (item.key().startsWith(QStringLiteral("wav/")) || item.key().startsWith(QStringLiteral("myavi/")))
                availableMediaAssets.insert(item.key());
        }
        for (const QString &image : images) {
            if (!assets.sourceByLowercasePath.contains(normalizedPath(image).toLower()))
                missingImages.insert(image);
        }
        for (const QString &mediaReference : media) {
            const QString key = normalizedPath(mediaReference).toLower();
            bool present = assets.sourceByLowercasePath.contains(key);
            if (!present) {
                for (auto item = assets.sourceByLowercasePath.constBegin(); item != assets.sourceByLowercasePath.constEnd(); ++item) {
                    if (item.key().endsWith(QLatin1Char('/') + key)) {
                        present = true;
                        break;
                    }
                }
            }
            if (!present)
                missingMedia.insert(mediaReference);
        }
    }
    if (assets.copyAssets) {
        for (const QString &image : images)
            imageHref(assets, image, outputDirectory);
        for (const QString &mediaReference : media)
            imageHref(assets, mediaReference, outputDirectory);
        for (const QString &mediaAsset : availableMediaAssets)
            imageHref(assets, mediaAsset, outputDirectory);
    }

    QJsonArray textArray;
    for (const TextFragment &fragment : fragments) {
        QJsonObject item;
        item.insert(QStringLiteral("offset"), static_cast<double>(fragment.offset));
        item.insert(QStringLiteral("text"), fragment.text);
        if (fragment.occurrences > 1)
            item.insert(QStringLiteral("occurrences"), fragment.occurrences);
        textArray.append(item);
    }

    QJsonArray topicArray;
    for (const ArticleTopic &topic : topics) {
        QJsonObject item;
        item.insert(QStringLiteral("id"), topic.id);
        item.insert(QStringLiteral("firstFragment"), topic.firstFragment);
        item.insert(QStringLiteral("afterLastFragment"), topic.afterLastFragment);
        item.insert(QStringLiteral("boundarySource"), topic.boundarySource);
        if (metadata.contains(topic.id))
            item.insert(QStringLiteral("metadata"), metadataJson(metadata.value(topic.id)));
        topicArray.append(item);
    }
    QJsonArray monthArray;
    for (const QString &month : monthTopics)
        monthArray.append(month);
    QJsonArray popupArray;
    for (const NavigationReference &reference : navigation) {
        if (reference.function.compare(QStringLiteral("PopupID"), Qt::CaseInsensitive) != 0)
            continue;
        QJsonObject item;
        item.insert(QStringLiteral("offset"), static_cast<double>(reference.offset));
        item.insert(QStringLiteral("targetId"), reference.targetId);
        item.insert(QStringLiteral("targetPane"), reference.targetPane);
        item.insert(QStringLiteral("resolution"), reference.resolution);
        if (!reference.contextArticleId.isEmpty())
            item.insert(QStringLiteral("contextArticleId"), reference.contextArticleId);
        popupArray.append(item);
    }

    QJsonObject document;
    document.insert(QStringLiteral("format"), QStringLiteral("mycom-mvb-salvage/v1"));
    document.insert(QStringLiteral("sourceFile"), QFileInfo(inputPath).fileName());
    document.insert(QStringLiteral("sourceBytes"), static_cast<double>(contents.size()));
    document.insert(QStringLiteral("minimumRunBytes"), minimumBytes);
    QJsonObject topicCatalogJson;
    if (mvbTopicCatalog.error.isEmpty()) {
        QJsonArray offsets;
        for (const quint32 offset : mvbTopicCatalog.offsets)
            offsets.append(static_cast<double>(offset));
        topicCatalogJson.insert(QStringLiteral("topicCount"), mvbTopicCatalog.offsets.size());
        topicCatalogJson.insert(QStringLiteral("topicOffsets"), offsets);
    } else {
        topicCatalogJson.insert(QStringLiteral("error"), mvbTopicCatalog.error);
    }
    if (decodedMvbTopics.error.isEmpty()) {
        QJsonArray headers;
        for (const MvbTopicHeader &header : decodedMvbTopics.headers) {
            QJsonObject item;
            item.insert(QStringLiteral("topicOffset"), static_cast<double>(header.topicOffset));
            item.insert(QStringLiteral("nextTopicOffset"), static_cast<double>(header.nextTopicOffset));
            item.insert(QStringLiteral("streamPosition"), static_cast<double>(header.streamPosition));
            item.insert(QStringLiteral("topicNumber"), static_cast<double>(header.topicNumber));
            if (!header.title.isEmpty())
                item.insert(QStringLiteral("title"), header.title);
            item.insert(QStringLiteral("bodyTextBytes"), static_cast<double>(header.bodyTextBytes));
            if (!header.bodyPreview.isEmpty())
                item.insert(QStringLiteral("bodyPreview"), header.bodyPreview);
            if (header.phraseCompressedRecords > 0)
                item.insert(QStringLiteral("phraseCompressedRecords"), header.phraseCompressedRecords);
            if (header.bodyReadErrors > 0)
                item.insert(QStringLiteral("bodyReadErrors"), header.bodyReadErrors);
            headers.append(item);
        }
        topicCatalogJson.insert(QStringLiteral("decodedHeaderCount"), decodedMvbTopics.headers.size());
        topicCatalogJson.insert(QStringLiteral("decodedHeaders"), headers);
        if (!decodedMvbTopics.bodyReadErrorPositions.isEmpty()) {
            QJsonArray bodyReadErrorPositions;
            for (const quint32 position : decodedMvbTopics.bodyReadErrorPositions)
                bodyReadErrorPositions.append(static_cast<double>(position));
            topicCatalogJson.insert(QStringLiteral("bodyReadErrorPositions"), bodyReadErrorPositions);
        }
    } else {
        topicCatalogJson.insert(QStringLiteral("decodedHeaderError"), decodedMvbTopics.error);
    }
    document.insert(QStringLiteral("mvbTopicCatalog"), topicCatalogJson);
    if (decodedMvbTopics.error.isEmpty() && !metadata.isEmpty())
        document.insert(QStringLiteral("mvbTopicArticleValidation"),
                        validateArticleTitlesAgainstMvbTopics(decodedMvbTopics.headers, metadata, topics));
    document.insert(QStringLiteral("textFragments"), textArray);
    document.insert(QStringLiteral("renderTokens"), renderTokensJson(fragments));
    document.insert(QStringLiteral("renderTokenNote"),
                    QStringLiteral("Text styles are conservative recovery classifications; original MVB character formatting is not decoded."));
    document.insert(QStringLiteral("imageReferences"), sortedArray(images));
    document.insert(QStringLiteral("missingImageReferences"), sortedArray(missingImages));
    document.insert(QStringLiteral("mediaReferences"), sortedArray(media));
    document.insert(QStringLiteral("missingMediaReferences"), sortedArray(missingMedia));
    document.insert(QStringLiteral("availableMediaAssets"), sortedArray(availableMediaAssets));
    document.insert(QStringLiteral("viewerMacros"), sortedArray(macros));
    document.insert(QStringLiteral("articleTopics"), topicArray);
    if (!topics.isEmpty())
        document.insert(QStringLiteral("catalogArticles"), catalogJson(metadata));
    document.insert(QStringLiteral("monthIndexes"), monthArray);
    document.insert(QStringLiteral("navigationReferences"), navigationJson(navigation));
    document.insert(QStringLiteral("popupReferences"), popupArray);

    const QFileInfo sourceInfo(inputPath);
    const QString baseName = sourceInfo.completeBaseName();
    const QString jsonPath = QDir(outputDirectory).filePath(baseName + QStringLiteral(".json"));
    QFile jsonFile(jsonPath);
    if (!jsonFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *errorMessage = QStringLiteral("Cannot write %1: %2").arg(jsonPath, jsonFile.errorString());
        return {};
    }
    jsonFile.write(QJsonDocument(document).toJson(QJsonDocument::Indented));

    ConversionSummary summary;
    summary.textFragmentCount = fragments.size();
    summary.articleTopicCount = topics.size();
    for (const ArticleTopic &topic : topics)
        summary.articleIds.append(topic.id);
    summary.succeeded = true;

    if (!writeReviewHtml) {
        if (writeTopics)
            writeTopicPages(outputDirectory, QFileInfo(inputPath).completeBaseName(), topics, fragments, metadata, assets);
        return summary;
    }

    const QString htmlPath = QDir(outputDirectory).filePath(baseName + QStringLiteral(".html"));
    QFile htmlFile(htmlPath);
    if (!htmlFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *errorMessage = QStringLiteral("Cannot write %1: %2").arg(htmlPath, htmlFile.errorString());
        return {};
    }

    QTextStream html(&htmlFile);
    html.setCodec("UTF-8");
    html << "<!doctype html><meta charset=\"utf-8\">\n";
    html << "<title>" << sourceInfo.fileName().toHtmlEscaped() << " - MVB salvage</title>\n";
    html << "<style>body{font:16px/1.5 system-ui,sans-serif;margin:2rem;max-width:72rem}"
            "pre{white-space:pre-wrap;border-bottom:1px solid #ddd;padding:.7rem 0}"
            "code{color:#666}li{word-break:break-all}</style>\n";
    html << "<h1>" << sourceInfo.fileName().toHtmlEscaped() << "</h1>\n";
    html << "<p>Best-effort extraction from a legacy Microsoft Multimedia Viewer (.MVB) file. "
            "Offsets are byte offsets in the original file.</p>\n";
    html << "<p><a href=\"" << baseName << ".navigation.html\">Legacy navigation report</a></p>\n";
    html << "<h2>Text fragments (" << fragments.size() << ")</h2>\n";
    for (const TextFragment &fragment : fragments) {
        html << "<pre><code>0x" << QString::number(fragment.offset, 16) << "</code> "
             << fragment.text.toHtmlEscaped().replace('\n', "<br>\n") << "</pre>\n";
    }
    const auto writeList = [&html](const QString &title, const QSet<QString> &items) {
        html << "<h2>" << title << " (" << items.size() << ")</h2><ul>\n";
        QStringList sorted = items.values();
        sorted.sort(Qt::CaseInsensitive);
        for (const QString &item : sorted)
            html << "<li>" << item.toHtmlEscaped() << "</li>\n";
        html << "</ul>\n";
    };
    writeList(QStringLiteral("Image references"), images);
    writeList(QStringLiteral("Image references missing from the source ISO"), missingImages);
    writeList(QStringLiteral("Media references"), media);
    writeList(QStringLiteral("Media references missing from the source ISO"), missingMedia);
    writeList(QStringLiteral("Available media assets from the source ISO"), availableMediaAssets);
    writeList(QStringLiteral("Viewer macros"), macros);
    writeNavigationReport(outputDirectory, baseName, navigation);

    if (writeTopics)
        writeTopicPages(outputDirectory, baseName, topics, fragments, metadata, assets);

    return summary;
}

QStringList collectInputs(const QString &inputPath)
{
    const QFileInfo info(inputPath);
    if (info.isFile())
        return {info.absoluteFilePath()};

    QStringList result;
    QDirIterator iterator(inputPath, {QStringLiteral("*.mvb"), QStringLiteral("*.MVB")},
                         QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext())
        result.append(iterator.next());
    result.sort(Qt::CaseInsensitive);
    return result;
}

struct NormalizedArchiveSource {
    QString mvbDirectory;
    QString dbfPath;
    QString assetDirectory;
    QStringList mvbFiles;
    QStringList assetFiles;
    struct FileDigest {
        QString path;
        qint64 bytes = 0;
        QString sha256;
    };
    QList<FileDigest> files;
};

QString sha256File(const QString &path, QString *errorMessage);

bool appendNormalizedDigest(const QString &path, const QString &relativePath,
                            NormalizedArchiveSource *result, QString *errorMessage)
{
    const QFileInfo info(path);
    const QString sha256 = sha256File(path, errorMessage);
    if (sha256.isEmpty())
        return false;
    result->files.append({relativePath, info.size(), sha256});
    return true;
}

bool copyFileInto(const QString &source, const QString &destination, QString *errorMessage)
{
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        *errorMessage = QStringLiteral("Cannot create normalized directory for %1").arg(destination);
        return false;
    }
    if (!QFile::copy(source, destination)) {
        *errorMessage = QStringLiteral("Cannot normalize %1 into %2").arg(source, destination);
        return false;
    }
    return true;
}

bool normalizeIsoExtraction(const QString &extractionDirectory, const QString &archiveDirectory,
                           NormalizedArchiveSource *result, QString *errorMessage)
{
    const QDir archive(archiveDirectory);
    result->mvbDirectory = archive.filePath(QStringLiteral("normalized/mvb"));
    result->assetDirectory = archive.filePath(QStringLiteral("normalized/assets"));
    result->dbfPath = archive.filePath(QStringLiteral("normalized/dbf/MYDBF01.DBF"));
    QDirIterator iterator(extractionDirectory, QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString source = iterator.next();
        const QFileInfo sourceInfo(source);
        const QString relative = normalizedPath(QDir(extractionDirectory).relativeFilePath(source));
        const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (sourceInfo.suffix().compare(QStringLiteral("MVB"), Qt::CaseInsensitive) == 0) {
            const QString fileName = sourceInfo.completeBaseName().toUpper() + QStringLiteral(".MVB");
            const QString destination = QDir(result->mvbDirectory).filePath(fileName);
            if (QFile::exists(destination)) {
                *errorMessage = QStringLiteral("Duplicate MVB name while normalizing: %1").arg(fileName);
                return false;
            }
            if (!copyFileInto(source, destination, errorMessage))
                return false;
            const QString normalizedFile = QStringLiteral("normalized/mvb/") + fileName;
            result->mvbFiles.append(normalizedFile);
            if (!appendNormalizedDigest(destination, normalizedFile, result, errorMessage))
                return false;
            continue;
        }
        if (sourceInfo.fileName().compare(QStringLiteral("MYDBF01.DBF"), Qt::CaseInsensitive) == 0) {
            if (QFile::exists(result->dbfPath)) {
                *errorMessage = QStringLiteral("Duplicate MYDBF01.DBF while normalizing.");
                return false;
            }
            if (!copyFileInto(source, result->dbfPath, errorMessage))
                return false;
            if (!appendNormalizedDigest(result->dbfPath, QStringLiteral("normalized/dbf/MYDBF01.DBF"),
                                        result, errorMessage))
                return false;
            continue;
        }
        if (parts.isEmpty())
            continue;
        const QString topLevel = parts.first().toLower();
        if (topLevel != QStringLiteral("bmp") && topLevel != QStringLiteral("wav")
            && topLevel != QStringLiteral("myavi"))
            continue;
        QStringList normalizedParts = parts;
        for (QString &part : normalizedParts)
            part = part.toLower();
        const QString normalizedRelative = normalizedParts.join(QLatin1Char('/'));
        const QString destination = QDir(result->assetDirectory).filePath(normalizedRelative);
        if (QFile::exists(destination)) {
            *errorMessage = QStringLiteral("Duplicate normalized asset path: %1").arg(normalizedRelative);
            return false;
        }
        if (!copyFileInto(source, destination, errorMessage))
            return false;
        const QString normalizedFile = QStringLiteral("normalized/assets/") + normalizedRelative;
        result->assetFiles.append(normalizedFile);
        if (!appendNormalizedDigest(destination, normalizedFile, result, errorMessage))
            return false;
    }
    result->mvbFiles.sort(Qt::CaseInsensitive);
    result->assetFiles.sort(Qt::CaseInsensitive);
    std::sort(result->files.begin(), result->files.end(), [](const NormalizedArchiveSource::FileDigest &left,
                                                               const NormalizedArchiveSource::FileDigest &right) {
        return left.path.compare(right.path, Qt::CaseInsensitive) < 0;
    });
    if (result->mvbFiles.isEmpty() || !QFileInfo::exists(result->dbfPath)) {
        *errorMessage = QStringLiteral("ISO extraction did not contain MVB files and MYDBF01.DBF.");
        return false;
    }
    return true;
}

bool extractIsoWith7z(const QString &sevenZip, const QString &isoPath, const QString &temporaryDirectory,
                      QString *errorMessage)
{
    QProcess process;
    process.setProgram(sevenZip);
    process.setArguments({QStringLiteral("x"), QStringLiteral("-y"), isoPath,
                          QStringLiteral("*.MVB"), QStringLiteral("*.mvb"),
                          QStringLiteral("DBF/MYDBF01.DBF"), QStringLiteral("dbf/MYDBF01.DBF"),
                          QStringLiteral("BMP/*"), QStringLiteral("bmp/*"),
                          QStringLiteral("WAV/*"), QStringLiteral("wav/*"),
                          QStringLiteral("MYAVI/*"), QStringLiteral("myavi/*"),
                          QStringLiteral("-o") + temporaryDirectory});
    process.start();
    if (!process.waitForStarted()) {
        *errorMessage = QStringLiteral("Cannot start 7z executable '%1': %2")
                            .arg(sevenZip, process.errorString());
        return false;
    }
    if (!process.waitForFinished(-1) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *errorMessage = QStringLiteral("7z extraction failed: %1")
                            .arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        return false;
    }
    return true;
}

QString sha256File(const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *errorMessage = QStringLiteral("Cannot hash ISO %1: %2").arg(path, file.errorString());
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError) {
            *errorMessage = QStringLiteral("Cannot hash ISO %1: %2").arg(path, file.errorString());
            return {};
        }
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool writeArchiveManifest(const QString &archiveDirectory, const QFileInfo &isoInfo,
                          const NormalizedArchiveSource &source, const QJsonArray &books,
                          int dbfRecordCount, const QString &isoSha256,
                          bool includesReviewHtml, bool includesTopicPages, QString *errorMessage)
{
    QJsonObject normalized;
    normalized.insert(QStringLiteral("mvbDirectory"), QStringLiteral("normalized/mvb"));
    normalized.insert(QStringLiteral("dbf"), QStringLiteral("normalized/dbf/MYDBF01.DBF"));
    normalized.insert(QStringLiteral("assetDirectory"), QStringLiteral("normalized/assets"));
    normalized.insert(QStringLiteral("mvbFiles"), QJsonArray::fromStringList(source.mvbFiles));
    normalized.insert(QStringLiteral("assetFiles"), QJsonArray::fromStringList(source.assetFiles));
    QJsonArray files;
    for (const NormalizedArchiveSource::FileDigest &file : source.files) {
        QJsonObject item;
        item.insert(QStringLiteral("path"), file.path);
        item.insert(QStringLiteral("bytes"), static_cast<double>(file.bytes));
        item.insert(QStringLiteral("sha256"), file.sha256);
        files.append(item);
    }
    normalized.insert(QStringLiteral("files"), files);

    QJsonObject sourceInfo;
    sourceInfo.insert(QStringLiteral("isoFileName"), isoInfo.fileName());
    sourceInfo.insert(QStringLiteral("isoBytes"), static_cast<double>(isoInfo.size()));
    sourceInfo.insert(QStringLiteral("isoLastModifiedUtc"), isoInfo.lastModified().toUTC().toString(Qt::ISODate));
    sourceInfo.insert(QStringLiteral("isoSha256"), isoSha256);

    QJsonObject validation;
    validation.insert(QStringLiteral("mvbCount"), source.mvbFiles.size());
    validation.insert(QStringLiteral("assetCount"), source.assetFiles.size());
    validation.insert(QStringLiteral("normalizedFileCount"), source.files.size());
    validation.insert(QStringLiteral("dbfRecordCount"), dbfRecordCount);
    validation.insert(QStringLiteral("conversionBookCount"), books.size());

    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), QStringLiteral("mycom-archive/v1"));
    manifest.insert(QStringLiteral("schemaVersion"), 1);
    manifest.insert(QStringLiteral("createdAtUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    manifest.insert(QStringLiteral("builder"), QStringLiteral("mycom-archive-build/1"));
    manifest.insert(QStringLiteral("source"), sourceInfo);
    manifest.insert(QStringLiteral("normalized"), normalized);
    manifest.insert(QStringLiteral("contentDirectory"), QStringLiteral("content"));
    manifest.insert(QStringLiteral("contentFormat"), QStringLiteral("mycom-mvb-salvage/v1"));
    QJsonObject optionalContent;
    optionalContent.insert(QStringLiteral("reviewHtml"), includesReviewHtml);
    optionalContent.insert(QStringLiteral("topicPages"), includesTopicPages);
    manifest.insert(QStringLiteral("optionalContent"), optionalContent);
    manifest.insert(QStringLiteral("books"), books);
    manifest.insert(QStringLiteral("validation"), validation);

    QFile manifestFile(QDir(archiveDirectory).filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *errorMessage = QStringLiteral("Cannot write archive manifest: %1").arg(manifestFile.errorString());
        return false;
    }
    manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mycom-archive-build"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Build a normalized, portable MYCOM archive from an ISO image."));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption minimumOption({QStringLiteral("m"), QStringLiteral("min-bytes")},
                                     QStringLiteral("Minimum contiguous text run in bytes (default: 8)."),
                                     QStringLiteral("count"), QStringLiteral("8"));
    parser.addOption(minimumOption);
    QCommandLineOption sevenZipOption(QStringLiteral("seven-zip"),
                                      QStringLiteral("7z executable used to extract the ISO (default: 7z)."),
                                      QStringLiteral("path"), QStringLiteral("7z"));
    parser.addOption(sevenZipOption);
    QCommandLineOption topicPagesOption(QStringLiteral("topic-pages"),
                                        QStringLiteral("Also write standalone article HTML pages under content/topics."));
    parser.addOption(topicPagesOption);
    QCommandLineOption reviewHtmlOption(QStringLiteral("review-html"),
                                        QStringLiteral("Also write standalone raw-text review and navigation HTML pages."));
    parser.addOption(reviewHtmlOption);
    QCommandLineOption rebuildOption(QStringLiteral("rebuild"),
                                     QStringLiteral("Delete a valid existing archive directory before rebuilding it."));
    parser.addOption(rebuildOption);
    parser.addPositionalArgument(QStringLiteral("iso"), QStringLiteral("Read-only MYCOM ISO input file."));
    parser.addPositionalArgument(QStringLiteral("archive-directory"),
                                 QStringLiteral("New directory for normalized data, converted content, and manifest.json."));
    parser.process(application);

    const QStringList positional = parser.positionalArguments();
    if (positional.size() != 2)
        parser.showHelp(1);

    bool validMinimum = false;
    const int minimumBytes = parser.value(minimumOption).toInt(&validMinimum);
    if (!validMinimum || minimumBytes < 3) {
        QTextStream(stderr) << "--min-bytes must be an integer of at least 3.\n";
        return 1;
    }
    const QFileInfo isoInfo(positional.at(0));
    if (!isoInfo.isFile()) {
        QTextStream(stderr) << "ISO input was not found: " << isoInfo.absoluteFilePath() << "\n";
        return 1;
    }
    const QString archiveDirectory = QDir(positional.at(1)).absolutePath();
    QDir archive(archiveDirectory);
    if (archive.exists() && !archive.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
        if (!parser.isSet(rebuildOption)) {
            QTextStream(stderr) << "Archive directory must be new or empty (use --rebuild for a valid existing archive): "
                                << archiveDirectory << "\n";
            return 1;
        }
        const QFileInfo manifestInfo(archive.filePath(QStringLiteral("manifest.json")));
        if (!manifestInfo.isFile()) {
            QTextStream(stderr) << "--rebuild refuses to delete a directory without manifest.json: "
                                << archiveDirectory << "\n";
            return 1;
        }
        QFile manifestFile(manifestInfo.absoluteFilePath());
        if (!manifestFile.open(QIODevice::ReadOnly)
            || QJsonDocument::fromJson(manifestFile.readAll()).object().value(QStringLiteral("format")).toString()
                   != QStringLiteral("mycom-archive/v1")) {
            QTextStream(stderr) << "--rebuild refuses to delete a directory without a valid MYCOM archive manifest: "
                                << archiveDirectory << "\n";
            return 1;
        }
        QTextStream(stdout) << "Removing previous MYCOM archive: " << archiveDirectory << "\n";
        if (!archive.removeRecursively()) {
            QTextStream(stderr) << "Cannot remove previous archive directory: " << archiveDirectory << "\n";
            return 1;
        }
        archive = QDir(archiveDirectory);
    }
    if (!archive.exists() && !QDir().mkpath(archiveDirectory)) {
        QTextStream(stderr) << "Cannot create archive directory " << archiveDirectory << ".\n";
        return 1;
    }
    QTemporaryDir extraction;
    if (!extraction.isValid()) {
        QTextStream(stderr) << "Cannot create temporary ISO extraction directory.\n";
        return 1;
    }
    QString error;
    QTextStream(stdout) << "Hashing " << isoInfo.fileName() << "...\n";
    const QString isoSha256 = sha256File(isoInfo.absoluteFilePath(), &error);
    if (isoSha256.isEmpty()) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    QTextStream(stdout) << "Extracting " << isoInfo.fileName() << " with " << parser.value(sevenZipOption) << "...\n";
    if (!extractIsoWith7z(parser.value(sevenZipOption), isoInfo.absoluteFilePath(), extraction.path(), &error)) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    NormalizedArchiveSource source;
    if (!normalizeIsoExtraction(extraction.path(), archiveDirectory, &source, &error)) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    QTextStream(stdout) << "Normalized " << source.mvbFiles.size() << " MVB files and "
                        << source.assetFiles.size() << " assets.\n";

    QHash<QString, ArticleMetadata> metadata = readMycomDbf(source.dbfPath, &error);
    if (!error.isEmpty()) {
        QTextStream(stderr) << error << '\n';
        return 1;
    }
    QTextStream(stdout) << "Loaded " << metadata.size() << " DBF article records.\n";
    const QDir contentDirectory(archive.filePath(QStringLiteral("content")));
    if (!QDir().mkpath(contentDirectory.absolutePath())) {
        QTextStream(stderr) << "Cannot create content directory " << contentDirectory.absolutePath() << ".\n";
        return 1;
    }
    // Normalized assets are part of the archive.  Content pages link to them
    // relatively, so a second copy under content/assets is unnecessary.
    const AssetCatalog assets = createAssetCatalog(source.assetDirectory, contentDirectory.absolutePath(), false);
    QStringList inputs = collectInputs(source.mvbDirectory);

    std::sort(inputs.begin(), inputs.end(), [](const QString &left, const QString &right) {
        const bool leftIsHeada = QFileInfo(left).completeBaseName().compare(QStringLiteral("HEADA"),
                                                                              Qt::CaseInsensitive) == 0;
        const bool rightIsHeada = QFileInfo(right).completeBaseName().compare(QStringLiteral("HEADA"),
                                                                                Qt::CaseInsensitive) == 0;
        if (leftIsHeada != rightIsHeada)
            return leftIsHeada;
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });

    int failures = 0;
    QSet<QString> unresolvedArticleIds = QSet<QString>(metadata.keyBegin(), metadata.keyEnd());
    QJsonArray books;
    for (const QString &input : inputs) {
        const ConversionSummary result = convertFile(input, contentDirectory.absolutePath(), minimumBytes,
                                                     metadata, parser.isSet(topicPagesOption), parser.isSet(reviewHtmlOption), assets,
                                                     unresolvedArticleIds, &error);
        if (!result.succeeded) {
            QTextStream(stderr) << error << '\n';
            ++failures;
            continue;
        }
        QTextStream(stdout) << "Converted " << QFileInfo(input).fileName() << " ("
                            << result.textFragmentCount
                            << " text fragments, "
                            << result.articleTopicCount
                            << " article topics).\n";
        QJsonObject book;
        book.insert(QStringLiteral("name"), QFileInfo(input).completeBaseName().toUpper());
        book.insert(QStringLiteral("normalizedMvb"),
                    QStringLiteral("normalized/mvb/") + QFileInfo(input).fileName());
        book.insert(QStringLiteral("contentJson"),
                    QStringLiteral("content/") + QFileInfo(input).completeBaseName().toUpper() + QStringLiteral(".json"));
        book.insert(QStringLiteral("textFragmentCount"), result.textFragmentCount);
        book.insert(QStringLiteral("articleTopicCount"), result.articleTopicCount);
        books.append(book);
        for (const QString &id : result.articleIds)
            unresolvedArticleIds.remove(id);
    }
    if (failures > 0)
        return 2;
    if (!writeArchiveManifest(archiveDirectory, isoInfo, source, books, metadata.size(), isoSha256,
                              parser.isSet(reviewHtmlOption), parser.isSet(topicPagesOption), &error)) {
        QTextStream(stderr) << error << '\n';
        return 2;
    }
    QTextStream(stdout) << "Archive ready: " << archiveDirectory << "\n";
    return 0;
}
