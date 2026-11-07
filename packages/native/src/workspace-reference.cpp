#include "workspace-reference.h"

#include <QDateTime>
#include <QMap>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTimeZone>
#include <QUrl>

namespace workspace {
namespace {
constexpr qsizetype MaxReferenceLength = 256 * 1024;
constexpr quint64 MaxSafeInteger = 9007199254740991ULL;
const QString UuidSource = QStringLiteral("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}");
const QRegularExpression UuidPattern('^' + UuidSource + '$', QRegularExpression::CaseInsensitiveOption);
const QRegularExpression Base64Pattern(QStringLiteral("^[A-Za-z0-9_-]*$"));

bool validUuid(const QString &value) { return UuidPattern.match(value).hasMatch(); }
QString encoded(const QString &value) { return QString::fromLatin1(QUrl::toPercentEncoding(value)); }
QString base64(const QString &value) { return QString::fromLatin1(value.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)); }

bool jsWhitespace(QChar character)
{
    const auto value = character.unicode();
    return (value >= 0x09 && value <= 0x0d) || value == 0x20 || value == 0xa0 || value == 0x1680
        || (value >= 0x2000 && value <= 0x200a) || value == 0x2028 || value == 0x2029
        || value == 0x202f || value == 0x205f || value == 0x3000 || value == 0xfeff;
}

QString trimJs(const QString &value)
{
    qsizetype start = 0, end = value.size();
    while (start < end && jsWhitespace(value[start])) ++start;
    while (end > start && jsWhitespace(value[end - 1])) --end;
    return value.mid(start, end - start);
}

QString simplifyJs(const QString &value)
{
    QString result;
    bool space = false;
    for (const auto character : value) {
        if (jsWhitespace(character)) { space = !result.isEmpty(); continue; }
        if (space) { result += ' '; space = false; }
        result += character;
    }
    return result;
}

std::optional<QByteArray> canonicalBase64(const QString &value)
{
    if (!Base64Pattern.match(value).hasMatch() || value.size() % 4 == 1) return {};
    const auto decoded = QByteArray::fromBase64Encoding(value.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals) != value.toLatin1()) return {};
    return decoded.decoded;
}

std::optional<QString> utf8(const QByteArray &bytes)
{
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
    const QString value = decoder.decode(bytes);
    return decoder.hasError() ? std::optional<QString>{} : value;
}

std::optional<QString> decodeBase64Text(const QString &value)
{
    const auto bytes = canonicalBase64(value);
    if (!bytes) return {};
    const auto text = utf8(*bytes);
    // TextDecoder strips its initial BOM. The source then requires an exact
    // re-encoding match, so a BOM-prefixed forward/source is noncanonical.
    return text && !text->startsWith(QChar(0xfeff)) ? text : std::optional<QString>{};
}

int hex(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

// decodeURIComponent is strict about both '%' triplets and UTF-8. In particular,
// it preserves '+', unlike form decoding and URLSearchParams used by file URNs.
std::optional<QString> decodeComponent(const QString &value)
{
    const auto source = value.toUtf8();
    QByteArray decoded;
    decoded.reserve(source.size());
    for (qsizetype i = 0; i < source.size(); ++i) {
        if (source[i] != '%') { decoded += source[i]; continue; }
        if (i + 2 >= source.size() || hex(source[i + 1]) < 0 || hex(source[i + 2]) < 0) return {};
        decoded += static_cast<char>(hex(source[i + 1]) * 16 + hex(source[i + 2]));
        i += 2;
    }
    return utf8(decoded);
}

QString decodeForm(QString value)
{
    value.replace('+', ' ');
    const auto source = value.toUtf8();
    QByteArray decoded;
    for (qsizetype i = 0; i < source.size(); ++i) {
        if (source[i] == '%' && i + 2 < source.size() && hex(source[i + 1]) >= 0 && hex(source[i + 2]) >= 0) {
            decoded += static_cast<char>(hex(source[i + 1]) * 16 + hex(source[i + 2]));
            i += 2;
        } else decoded += source[i];
    }
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
    return decoder.decode(decoded);
}

QMap<QString, QString> formQuery(const QString &query)
{
    QMap<QString, QString> values;
    for (const auto &part : query.split('&', Qt::SkipEmptyParts)) {
        const auto separator = part.indexOf('=');
        const auto key = decodeForm(separator < 0 ? part : part.left(separator));
        if (!values.contains(key)) values.insert(key, separator < 0 ? QString() : decodeForm(part.mid(separator + 1)));
    }
    return values;
}

std::optional<qint64> integerParam(const QMap<QString, QString> &query, const QString &key, bool allowZero)
{
    static const QRegularExpression integer(QStringLiteral("^[0-9]+$"));
    const auto raw = simplifyJs(query.value(key));
    if (!integer.match(raw).hasMatch()) return {};
    bool valid = false;
    const auto value = raw.toULongLong(&valid);
    if (!valid || value > MaxSafeInteger || (value == 0 && !allowZero)) return {};
    return static_cast<qint64>(value);
}

bool validSourceDate(const QString &value)
{
    // Actual Workspace DTO/builders use ISO8601. RFC2822/TextDate retain common
    // older imports without embedding a JavaScript engine for Date.parse.
    return QDateTime::fromString(value, Qt::ISODateWithMs).isValid()
        || QDateTime::fromString(value, Qt::RFC2822Date).isValid()
        || QDateTime::fromString(value, Qt::TextDate).isValid()
        || QDate::fromString(value, Qt::ISODate).isValid();
}

QString nameForKind(WorkspaceReferenceKind kind)
{
    switch (kind) {
    case WorkspaceReferenceKind::User: return "user";
    case WorkspaceReferenceKind::Message: return "message";
    case WorkspaceReferenceKind::Stream: return "stream";
    case WorkspaceReferenceKind::Topic: return "topic";
    case WorkspaceReferenceKind::Quote: return "quote";
    case WorkspaceReferenceKind::Forward: return "forward";
    case WorkspaceReferenceKind::File: return "file";
    case WorkspaceReferenceKind::Image: return "image";
    case WorkspaceReferenceKind::Video: return "video";
    case WorkspaceReferenceKind::External: return "external";
    case WorkspaceReferenceKind::Time: return "time";
    case WorkspaceReferenceKind::Secret: return "secret";
    }
    return {};
}
} // namespace

QVariantMap WorkspaceReference::toVariantMap() const
{
    QVariantMap result{{"kind", nameForKind(kind)}};
    if (kind == WorkspaceReferenceKind::Secret) {
        result.insert("valid", secretValid);
        if (secretValid) { result.insert("stream", streamUuid); result.insert("keyId", keyUuid); }
        return result;
    }
    if (!id.isEmpty()) result.insert("id", id);
    if (!target.isEmpty()) result.insert("target", target);
    if (!streamUuid.isEmpty()) result.insert("stream", streamUuid);
    if (!label.isEmpty()) result.insert("label", label);
    if (silent) result.insert("silent", true);
    if (text) result.insert("text", *text);
    if (kind == WorkspaceReferenceKind::Forward) {
        result.insert("snapshotMarkdown", snapshotMarkdown);
        if (plainSnapshot) result.insert("snapshotFormat", "plain");
        if (!sourceLabel.isEmpty()) { result.insert("sourceLabel", sourceLabel); result.insert("sourceCreatedAt", sourceCreatedAt); }
        if (directSource) result.insert("sourceKind", "direct");
    }
    if (!contentType.isEmpty()) result.insert("contentType", contentType);
    if (sizeBytes) result.insert("sizeBytes", *sizeBytes);
    if (width) result.insert("width", *width);
    if (height) result.insert("height", *height);
    if (!url.isEmpty()) result.insert("url", url);
    if (!utc.isEmpty()) result.insert("utc", utc);
    return result;
}

bool isWorkspaceSecretReference(const QString &target)
{
    return trimJs(target).startsWith("urn:secret:", Qt::CaseInsensitive);
}

QString parseWorkspaceMessageTime(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^(\\d{4})-(\\d{2})-(\\d{2})T(\\d{2}):(\\d{2})(?::(\\d{2})(?:\\.(\\d{1,3}))?)?(Z|[+-]\\d{2}:\\d{2})$"));
    const auto match = pattern.match(value);
    if (!match.hasMatch()) return {};
    const auto year = match.captured(1).toInt();
    const QDate date(year, match.captured(2).toInt(), match.captured(3).toInt());
    const auto milliseconds = match.captured(7).isEmpty() ? 0 : match.captured(7).leftJustified(3, '0').toInt();
    const QTime time(match.captured(4).toInt(), match.captured(5).toInt(), match.captured(6).toInt(), milliseconds);
    if (year < 1 || !date.isValid() || !time.isValid()) return {};
    const auto zone = match.captured(8);
    qint64 offset = 0;
    if (zone != "Z") {
        const auto hours = zone.mid(1, 2).toInt();
        const auto minutes = zone.mid(4, 2).toInt();
        if (hours > 23 || minutes > 59) return {};
        offset = (hours * 3600 + minutes * 60) * (zone[0] == '-' ? -1 : 1);
    }
    const auto utc = QDateTime(date, time, QTimeZone::UTC).addSecs(-offset);
    const auto utcDate = utc.date();
    const auto utcTime = utc.time();
    // QDate omits year zero; JavaScript's ISO calendar uses astronomical years.
    const auto utcYear = utcDate.year() < 0 ? utcDate.year() + 1 : utcDate.year();
    const auto formattedYear = utcYear > 9999 ? '+' + QString::number(utcYear).rightJustified(6, '0') : QString::number(utcYear).rightJustified(4, '0');
    auto result = formattedYear + '-' + QString::number(utcDate.month()).rightJustified(2, '0')
        + '-' + QString::number(utcDate.day()).rightJustified(2, '0') + 'T'
        + utcTime.toString(QStringLiteral("HH:mm:ss"));
    if (utcTime.msec() != 0) result += '.' + QString::number(utcTime.msec()).rightJustified(3, '0');
    return result + 'Z';
}

std::optional<WorkspaceReference> parseWorkspaceReference(const QString &target, const QString &label, bool silent)
{
    const auto normalized = trimJs(target);
    if (isWorkspaceSecretReference(normalized)) {
        WorkspaceReference result;
        result.kind = WorkspaceReferenceKind::Secret;
        // Unsupported versions and malformed secrets still require a closed,
        // neutral placeholder. Their untrusted label must never be displayed.
        if (normalized.size() > MaxReferenceLength) return result;
        const auto parts = normalized.split(':');
        if (parts.size() != 6 || parts[0] != "urn" || parts[1] != "secret" || parts[2] != "v1"
            || !validUuid(parts[3]) || !validUuid(parts[4]) || parts[5].isEmpty() || parts[5].size() > 40000
            || !canonicalBase64(parts[5])) return result;
        result.secretValid = true;
        result.streamUuid = parts[3].toLower();
        result.keyUuid = parts[4].toLower();
        return result;
    }
    if (normalized.isEmpty() || normalized.size() > MaxReferenceLength) return {};
    if (normalized.startsWith("urn:quote:", Qt::CaseInsensitive)
        || normalized.startsWith("urn:forward:", Qt::CaseInsensitive)) {
        // JavaScript's source patterns use '.', which excludes all four JS line
        // terminators. PCRE's default dot excludes only LF. Encoded text is valid.
        for (const auto character : normalized)
            if (character == '\r' || character == '\n' || character == QChar(0x2028) || character == QChar(0x2029)) return {};
    }
    static const QRegularExpression simple("^urn:(user|message|stream|topic):(" + UuidSource + ")(?::(" + UuidSource + "))?$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression quote("^urn:quote:(" + UuidSource + ")(?:\\?(.*))?$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression forward("^urn:forward:(" + UuidSource + ")\\?(.*)$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression file("^urn:(file|image|video):(" + UuidSource + ")(?:\\?([\\s\\S]*))?$", QRegularExpression::CaseInsensitiveOption);
    WorkspaceReference result;
    result.target = normalized;
    if (const auto match = simple.match(normalized); match.hasMatch()) {
        const auto kind = match.captured(1).toLower();
        if (!match.captured(3).isEmpty() && kind != "topic") return {};
        result.kind = kind == "user" ? WorkspaceReferenceKind::User : kind == "message" ? WorkspaceReferenceKind::Message
            : kind == "stream" ? WorkspaceReferenceKind::Stream : WorkspaceReferenceKind::Topic;
        result.id = match.captured(2).toLower();
        if (kind == "topic" && !match.captured(3).isEmpty()) { result.streamUuid = result.id; result.id = match.captured(3).toLower(); }
        if (kind == "user") {
            result.label = simplifyJs(label);
            while (result.label.startsWith('@')) result.label.remove(0, 1);
            result.silent = silent;
        }
        return result;
    }
    if (const auto match = quote.match(normalized); match.hasMatch()) {
        result.kind = WorkspaceReferenceKind::Quote;
        result.id = match.captured(1).toLower();
        if (match.capturedStart(2) >= 0) {
            const auto query = match.captured(2);
            if (!query.startsWith("text=") || query.contains('&')) return {};
            const auto text = decodeComponent(query.mid(5));
            if (!text) return {};
            if (!text->isEmpty()) result.text = *text;
        }
        return result;
    }
    if (const auto match = forward.match(normalized); match.hasMatch()) {
        QMap<QString, QString> values;
        for (const auto &part : match.captured(2).split('&')) {
            const auto separator = part.indexOf('=');
            if (separator < 1) return {};
            const auto key = part.left(separator);
            if ((key != "snapshot" && key != "format" && key != "source" && key != "source_kind" && key != "created_at") || values.contains(key)) return {};
            values.insert(key, part.mid(separator + 1));
        }
        if (!values.contains("snapshot")) return {};
        const auto snapshot = decodeBase64Text(values.value("snapshot"));
        if (!snapshot || (values.contains("format") && values.value("format") != "plain")) return {};
        if (values.contains("source") != values.contains("created_at")) return {};
        if (values.contains("source_kind") && (values.value("source_kind") != "direct" || !values.contains("source"))) return {};
        result.kind = WorkspaceReferenceKind::Forward;
        result.id = match.captured(1).toLower();
        result.snapshotMarkdown = *snapshot;
        result.plainSnapshot = values.value("format") == "plain";
        if (values.contains("source")) {
            const auto source = decodeBase64Text(values.value("source"));
            const auto created = decodeComponent(values.value("created_at"));
            if (!source || trimJs(*source).isEmpty() || trimJs(*source).size() > 512 || !created
                || trimJs(*created).isEmpty() || !validSourceDate(trimJs(*created))) return {};
            result.sourceLabel = trimJs(*source); result.sourceCreatedAt = trimJs(*created);
            result.directSource = values.value("source_kind") == "direct";
        }
        return result;
    }
    if (const auto match = file.match(normalized); match.hasMatch()) {
        const auto kind = match.captured(1).toLower();
        result.kind = kind == "image" ? WorkspaceReferenceKind::Image : kind == "video" ? WorkspaceReferenceKind::Video : WorkspaceReferenceKind::File;
        result.id = match.captured(2).toLower();
        const auto query = formQuery(match.captured(3));
        const auto name = simplifyJs(query.value("name"));
        result.label = name.isEmpty() ? simplifyJs(label) : name;
        result.contentType = simplifyJs(query.value("content_type"));
        result.width = integerParam(query, "w", false); result.height = integerParam(query, "h", false); result.sizeBytes = integerParam(query, "size", true);
        return result;
    }
    if (normalized.startsWith("urn:time:", Qt::CaseInsensitive)) {
        result.utc = parseWorkspaceMessageTime(normalized.mid(9));
        if (result.utc.isEmpty()) return {};
        result.kind = WorkspaceReferenceKind::Time;
        return result;
    }
    const bool urlUrn = normalized.startsWith("urn:url:", Qt::CaseInsensitive);
    const auto external = urlUrn ? normalized.mid(8) : target;
    if (external.isEmpty()) return {};
    for (const auto character : external)
        if (jsWhitespace(character) || character.unicode() < 0x20 || character.unicode() == 0x7f) return {};
    const QUrl url(external, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != "https" && url.scheme() != "http") || url.host().isEmpty() || !url.userInfo().isEmpty()) return {};
    result.kind = WorkspaceReferenceKind::External;
    result.url = url.toString(QUrl::FullyEncoded);
    return result;
}

QString escapeWorkspaceMarkdownInline(const QString &value)
{
    const QString special = QStringLiteral("\\`*_{()[]#+.!|>~-");
    QString escaped;
    escaped.reserve(value.size());
    for (const auto character : value) { if (special.contains(character)) escaped += '\\'; escaped += character; }
    return escaped;
}

QString buildWorkspaceQuoteUrn(const QString &messageUuid, const QString &text)
{
    const auto id = trimJs(messageUuid);
    if (!validUuid(id)) return {};
    return "urn:quote:" + id + (text.isEmpty() ? QString() : "?text=" + encoded(text));
}

QString buildWorkspaceForwardUrn(const QString &messageUuid, const QString &snapshotMarkdown, const WorkspaceForwardOptions &options)
{
    const auto id = trimJs(messageUuid);
    if (!validUuid(id)) return {};
    auto urn = "urn:forward:" + id + "?snapshot=" + base64(snapshotMarkdown);
    if (options.plainSnapshot) urn += "&format=plain";
    if (options.sourceLabel.has_value() != options.sourceCreatedAt.has_value() || (options.directSource && !options.sourceLabel)) return {};
    if (options.sourceLabel) {
        const auto source = trimJs(*options.sourceLabel); const auto created = trimJs(*options.sourceCreatedAt);
        if (source.isEmpty() || source.size() > 512 || created.isEmpty() || !validSourceDate(created)) return {};
        urn += "&source=" + base64(source);
        if (options.directSource) urn += "&source_kind=direct";
        urn += "&created_at=" + encoded(created);
    }
    return urn.size() <= MaxReferenceLength ? urn : QString();
}

QString buildWorkspaceUserMention(const QString &label, const QString &userUuid, bool silent)
{
    const auto id = trimJs(userUuid);
    if (!validUuid(id)) return {};
    return '[' + escapeWorkspaceMarkdownInline(trimJs(label)) + "](urn:user:" + id + (silent ? " \"silent\")" : ")");
}

QString buildWorkspaceQuoteReference(const QString &label, const QString &messageUuid, const QString &text)
{
    const auto urn = buildWorkspaceQuoteUrn(messageUuid, text);
    return urn.isEmpty() || trimJs(label).isEmpty() ? QString() : '[' + escapeWorkspaceMarkdownInline(trimJs(label)) + "](" + urn + ')';
}

QString buildWorkspaceForwardReference(const QString &label, const QString &messageUuid, const QString &snapshotMarkdown, const WorkspaceForwardOptions &options)
{
    const auto urn = buildWorkspaceForwardUrn(messageUuid, snapshotMarkdown, options);
    return urn.isEmpty() || trimJs(label).isEmpty() ? QString() : '[' + escapeWorkspaceMarkdownInline(trimJs(label)) + "](" + urn + ')';
}
} // namespace workspace
