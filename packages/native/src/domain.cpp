#include "domain.h"

#include "workspace-reference.h"
#include <QHash>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace workspace {

QString sourceTrimmed(const QString &text)
{
    const auto space = [](QChar c) {
        const auto u = c.unicode();
        return (u >= 9 && u <= 13) || u == 0x20 || u == 0xa0 || u == 0x1680
            || (u >= 0x2000 && u <= 0x200a) || u == 0x2028 || u == 0x2029
            || u == 0x202f || u == 0x205f || u == 0x3000 || u == 0xfeff;
    };
    qsizetype begin = 0, end = text.size();
    while (begin < end && space(text[begin])) ++begin;
    while (end > begin && space(text[end - 1])) --end;
    return text.mid(begin, end - begin);
}

QString imageAvatarReference(const QJsonValue &value)
{
    if (!value.isString()) return {};
    const auto avatar = sourceTrimmed(value.toString());
    // Match Source's avatar UUID shape, including the nil UUID. isUuid() is a
    // stronger non-null domain identity predicate and remains unchanged.
    static const QRegularExpression pattern(QStringLiteral(
        "\\Aurn:image:[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\z"));
    return pattern.match(avatar).hasMatch() ? avatar : QString{};
}

bool validReactionName(const QString &name)
{
    // Match the web DTO's non-empty String.trim() predicate. NEL is not
    // ECMAScript whitespace; BOM is. Unknown provider identities stay inert.
    bool visible = false;
    for (const auto c : name)
        if (!((c.isSpace() && c.unicode() != 0x85) || c.unicode() == 0xfeff)) visible = true;
    if (!visible) return false;
    int points = 0;
    for (qsizetype i = 0; i < name.size(); ++i) {
        if (name.at(i).isHighSurrogate()) {
            if (++i == name.size() || !name.at(i).isLowSurrogate()) return false;
        } else if (name.at(i).isLowSurrogate()) return false;
        if (++points > 128) return false;
    }
    return true;
}

namespace {
// Exact backend workspace/common/unicode_emoji.json: Zulip 12.1.2 / emojibase
// 17.0.0. Provenance and Apache-2.0/MIT notices accompany the pinned resource.
// Presentation is Unicode text only; provider names never resolve to image URLs.
struct EmojiCatalog {
    QHash<QString, QString> canonical;
    QHash<QString, QString> glyphs;
    QVariantList choices;
    void loadPickerPresentation() {
        QFile file(":/workspace/data/emoji_picker.json");
        if (!file.open(QIODevice::ReadOnly)) return;
        const auto document = QJsonDocument::fromJson(file.readAll()).object();
        if (document.value("schema").toInt() != 1
            || document.value("source").toObject().value("version").toString() != "4.19.1") return;
        const auto categories = document.value("categories").toArray();
        const auto entries = document.value("entries").toArray();
        if (categories.size() != 8 || entries.size() != 1898) return;
        QHash<QString, QVariantMap> metadata;
        int order = 0;
        for (const auto &value : entries) {
            const auto entry = value.toArray();
            if (entry.size() != 6) return;
            const auto base = entry.at(0).toString();
            const auto category = entry.at(1).toInt(-1);
            if (!glyphs.contains(base) || metadata.contains(base)
                || category < 0 || category >= categories.size() || !entry.at(5).isBool()) return;
            QVariantList variants;
            for (const auto &variantValue : entry.at(4).toArray()) {
                const auto variant = variantValue.toArray();
                if (variant.size() != 2) return;
                variants.append(variant.at(0).toString());
            }
            const QVariantMap common{{"pickerEligible", entry.at(5).toBool()}, {"pickerBase", base},
                {"pickerCategory", categories.at(category).toString()}, {"pickerOrder", order++},
                {"pickerSearch", entry.at(3).toArray().toVariantList()}};
            auto insert = [&](const QString &name, const QString &display, const QVariantList &children) {
                auto plain = display; plain.remove(QChar(0xfe0f));
                auto wire = glyphs.value(name); wire.remove(QChar(0xfe0f));
                if (display.isEmpty() || !glyphs.contains(name) || plain != wire || metadata.contains(name)) return false;
                auto fields = common;
                fields.insert("pickerUnicode", display);
                fields.insert("pickerVariants", children);
                metadata.insert(name, fields);
                return true;
            };
            if (!insert(base, entry.at(2).toString(), variants)) return;
            for (const auto &variantValue : entry.at(4).toArray()) {
                const auto variant = variantValue.toArray();
                if (!insert(variant.at(0).toString(), variant.at(1).toString(), {})) return;
            }
        }
        if (metadata.size() != 3773) return;
        for (auto &value : choices) {
            auto row = value.toMap();
            const auto name = row.value("emojiName").toString();
            row.insert("pickerEligible", false);
            row.insert("pickerOrder", -1);
            row.insert("pickerBase", QString{});
            row.insert("pickerCategory", QString{});
            row.insert("pickerUnicode", row.value("unicode"));
            row.insert("pickerSearch", QVariantList{});
            row.insert("pickerVariants", QVariantList{});
            const auto fields = metadata.value(name);
            for (auto it = fields.cbegin(); it != fields.cend(); ++it) row.insert(it.key(), it.value());
            if (!fields.isEmpty()) glyphs.insert(name, fields.value("pickerUnicode").toString());
            value = row;
        }
    }
    EmojiCatalog() {
        QFile file(":/workspace/data/unicode_emoji.json");
        if (!file.open(QIODevice::ReadOnly)) return;
        const auto entries = QJsonDocument::fromJson(file.readAll()).object().value("workspace").toObject();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            const auto names = it.value().toArray();
            if (names.isEmpty()) continue;
            const auto name = names.first().toString();
            QString glyph;
            for (const auto &point : it.key().split('-')) {
                const auto value = static_cast<char32_t>(point.toUInt(nullptr, 16));
                glyph += QString::fromUcs4(&value, 1);
            }
            glyphs.insert(name, glyph);
            canonical.insert(glyph, name);
            for (const auto &alias : names) canonical.insert(alias.toString(), name);
            choices.append(QVariantMap{{"emojiName", name}, {"unicode", glyph}, {"aliases", names.toVariantList()}});
        }
        loadPickerPresentation();
    }
};
const EmojiCatalog &emojiCatalog() { static const EmojiCatalog catalog; return catalog; }
// Python str.isspace(), used by the backend's strip() and re [\s-]+.
bool pythonSpace(QChar c) {
    const auto u = c.unicode();
    return (u >= 9 && u <= 13) || (u >= 0x1c && u <= 0x20) || u == 0x85 || u == 0xa0
        || u == 0x1680 || (u >= 0x2000 && u <= 0x200a) || u == 0x2028 || u == 0x2029
        || u == 0x202f || u == 0x205f || u == 0x3000;
}
}

QString canonicalReactionName(const QString &input)
{
    if (!validReactionName(input)) return {};
    auto symbol = input; symbol.remove(QChar(0xfe0f));
    const auto &catalog = emojiCatalog();
    if (catalog.canonical.contains(symbol)) return catalog.canonical.value(symbol);
    auto name = input.toLower();
    while (!name.isEmpty() && pythonSpace(name.front())) name.remove(0, 1);
    while (!name.isEmpty() && pythonSpace(name.back())) name.chop(1);
    while (name.startsWith(':')) name.remove(0, 1);
    while (name.endsWith(':')) name.chop(1);
    QString shortcode;
    for (const auto c : name) {
        const auto normalized = pythonSpace(c) || c == '-' ? QChar('_') : c;
        if (normalized != '_' || !shortcode.endsWith('_')) shortcode += normalized;
    }
    while (shortcode.startsWith('_')) shortcode.remove(0, 1);
    while (shortcode.endsWith('_')) shortcode.chop(1);
    return catalog.canonical.value(shortcode);
}

QString reactionUnicode(const QString &name)
{
    return emojiCatalog().glyphs.value(canonicalReactionName(name));
}

QVariantList knownReactionEmojis() { return emojiCatalog().choices; }

std::optional<ReactionFact> parseReactionFact(const QJsonObject &json, const QString &project)
{
    ReactionFact fact{json.value("uuid").toString(), json.value("message_uuid").toString(),
        json.value("user_uuid").toString(), json.value("emoji_name").toString()};
    if (json.value("project_id").toString() != project || !isUuid(fact.uuid) || !isUuid(fact.message)
        || !isUuid(fact.user) || !validReactionName(fact.emoji)
        || !QDateTime::fromString(json.value("created_at").toString(), Qt::ISODateWithMs).isValid()
        || !QDateTime::fromString(json.value("updated_at").toString(), Qt::ISODateWithMs).isValid()) return {};
    return fact;
}

bool isUuid(const QString &value)
{
    return value.size() == 36 && !QUuid(value).isNull()
        && QUuid(value).toString(QUuid::WithoutBraces) == value.toLower();
}

static std::optional<qint64> counter(const QJsonObject &json, const QString &key)
{
    if (!json.contains(key)) return 0;
    const auto value = json.value(key);
    if (!value.isDouble()) return {};
    const auto number = value.toDouble(-1);
    if (!std::isfinite(number) || number < 0 || number > 9007199254740991.0 || std::floor(number) != number) return {};
    return static_cast<qint64>(number);
}

static bool validLastMessage(const QJsonObject &json)
{
    const auto value = json.value("last_message_uuid");
    return value.isUndefined() || value.isNull() || (value.isString() && isUuid(value.toString()));
}

static bool optionalMode(const QJsonObject &json, const QStringList &allowed)
{
    const auto value = json.value("notification_mode");
    return value.isUndefined() || (value.isString() && allowed.contains(value.toString()));
}

static std::optional<qint64> optionalUpdatedMicros(const QJsonObject &json)
{
    if (!json.contains("updated_at")) return 0;
    if (!json.value("updated_at").isString()) return {};
    const auto text = json.value("updated_at").toString();
    const auto date = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!date.isValid()) return {};
    static const QRegularExpression fraction("\\.([0-9]{1,6})(?:Z|[+-][0-9]{2}:[0-9]{2})$");
    const auto match = fraction.match(text);
    const auto micros = match.hasMatch() ? match.captured(1).leftJustified(6, QLatin1Char('0')).toInt() : 0;
    return date.toMSecsSinceEpoch() * 1000 + micros % 1000;
}

namespace {
bool wireText(const QJsonObject &json, const char *field, qsizetype minimum, qsizetype maximum)
{
    const auto value = json.value(QLatin1String(field));
    if (!value.isString()) return false;
    const auto text = value.toString();
    if (text.size() > maximum * 2) return false;
    qsizetype points = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isHighSurrogate()) {
            if (++i == text.size() || !text[i].isLowSurrogate()) return false;
        } else if (text[i].isLowSurrogate()) return false;
        if (++points > maximum) return false;
    }
    return points >= minimum;
}

bool optionalUuid(const QJsonObject &json, const char *field, bool nullable)
{
    const auto value = json.value(QLatin1String(field));
    return value.isUndefined() || (nullable && value.isNull()) || (value.isString() && isUuid(value.toString()));
}

// Issue #214 mirrors of the reference guard primitives (messenger.types.ts):
// isNonNegativeInteger and isDateTime (a non-empty trimmed string).
bool nonNegativeInteger(const QJsonValue &value)
{
    return value.isDouble() && std::isfinite(value.toDouble())
        && std::floor(value.toDouble()) == value.toDouble() && value.toDouble() >= 0;
}

bool dateTimeText(const QJsonValue &value)
{
    return value.isString() && !sourceTrimmed(value.toString()).isEmpty();
}

bool datetimeOrNull(const QJsonObject &json, const char *field)
{
    const auto value = json.value(QLatin1String(field));
    return value.isUndefined() || value.isNull() || dateTimeText(value);
}

bool everyUuid(const QJsonArray &values)
{
    for (const auto &value : values)
        if (!isUuid(value.toString())) return false;
    return true;
}

bool parsePollOption(const QJsonValue &value, PollOption &out)
{
    if (!value.isObject()) return false;
    const auto option = value.toObject();
    if (!isUuid(option.value("uuid").toString()) || !option.value("text").isString()) return false;
    out = {option.value("uuid").toString(), option.value("text").toString()};
    return true;
}

// Strict poll payload guard: every boolean is typed, results_visibility is the
// closed enum, closes_at/closed_at are datetime-or-null.
bool parsePollPayload(const QJsonObject &payload, PollPayload &out)
{
    const auto questions = payload.value(QStringLiteral("questions"));
    if (!questions.isArray()) return false;
    PollPayload parsed;
    for (const auto &question : questions.toArray()) {
        if (!question.isObject()) return false;
        const auto row = question.toObject();
        if (!isUuid(row.value("uuid").toString()) || !row.value("text").isString()) return false;
        const auto options = row.value(QStringLiteral("options"));
        if (!options.isArray()) return false;
        PollQuestion parsedQuestion{row.value("uuid").toString(), row.value("text").toString(), {}, {}};
        for (const auto &option : options.toArray()) {
            PollOption parsedOption;
            if (!parsePollOption(option, parsedOption)) return false;
            parsedQuestion.options.append(parsedOption);
        }
        const auto correct = row.value(QStringLiteral("correct_option_uuids"));
        if (!correct.isUndefined()) {
            if (!correct.isArray() || !everyUuid(correct.toArray())) return false;
            for (const auto &value : correct.toArray())
                parsedQuestion.correctOptionUuids.append(value.toString());
        }
        parsed.questions.append(parsedQuestion);
    }
    for (const auto *field : {"multiple_answers", "is_anonymous", "allow_revote"})
        if (!payload.value(QLatin1String(field)).isBool()) return false;
    const auto visibility = payload.value(QStringLiteral("results_visibility")).toString();
    if (visibility != QLatin1String("always") && visibility != QLatin1String("after_vote")) return false;
    const auto adding = payload.value(QStringLiteral("allow_adding_options"));
    if (!adding.isUndefined() && !adding.isBool()) return false;
    if (!datetimeOrNull(payload, "closes_at") || !datetimeOrNull(payload, "closed_at")) return false;
    parsed.multipleAnswers = payload.value(QStringLiteral("multiple_answers")).toBool();
    parsed.isAnonymous = payload.value(QStringLiteral("is_anonymous")).toBool();
    parsed.allowRevote = payload.value(QStringLiteral("allow_revote")).toBool();
    parsed.resultsVisibility = visibility;
    parsed.allowAddingOptions = adding.toBool(false);
    parsed.closesAt = payload.value(QStringLiteral("closes_at")).toString();
    parsed.closedAt = payload.value(QStringLiteral("closed_at")).toString();
    out = std::move(parsed);
    return true;
}

// option_voter_previews: UUID-keyed non-empty, duplicate-free UUID arrays.
bool parseVoterPreviews(const QJsonValue &value, QMap<QString, QStringList> &out)
{
    if (!value.isObject()) return false;
    const auto previews = value.toObject();
    for (auto it = previews.begin(); it != previews.end(); ++it) {
        if (!isUuid(it.key()) || !it.value().isArray()) return false;
        const auto voters = it.value().toArray();
        if (voters.isEmpty() || !everyUuid(voters)) return false;
        QStringList ids;
        for (const auto &voter : voters) {
            if (ids.contains(voter.toString())) return false;
            ids.append(voter.toString());
        }
        out.insert(it.key(), ids);
    }
    return true;
}

// Strict poll state guard: revision is an integer > 0, counters are
// non-negative integers keyed by UUID, previews are unique non-empty UUID lists.
bool parsePollState(const QJsonValue &value, PollState &out)
{
    if (!value.isObject()) return false;
    const auto state = value.toObject();
    const auto revision = state.value(QStringLiteral("revision"));
    if (!nonNegativeInteger(revision) || revision.toDouble() < 1) return false;
    if (!state.value(QStringLiteral("is_closed")).isBool()) return false;
    if (!nonNegativeInteger(state.value(QStringLiteral("voter_count")))) return false;
    const auto questions = state.value(QStringLiteral("questions"));
    if (!questions.isArray()) return false;
    PollState parsed;
    for (const auto &question : questions.toArray()) {
        if (!question.isObject()) return false;
        const auto row = question.toObject();
        if (!isUuid(row.value(QStringLiteral("question_uuid")).toString())) return false;
        if (!nonNegativeInteger(row.value(QStringLiteral("voter_count")))) return false;
        const auto counts = row.value(QStringLiteral("option_counts"));
        if (!counts.isObject()) return false;
        PollQuestionState parsedQuestion;
        parsedQuestion.questionUuid = row.value(QStringLiteral("question_uuid")).toString();
        parsedQuestion.voterCount = qint64(row.value(QStringLiteral("voter_count")).toDouble());
        const auto countsObject = counts.toObject();
        for (auto it = countsObject.begin(); it != countsObject.end(); ++it) {
            if (!isUuid(it.key()) || !nonNegativeInteger(it.value())) return false;
            parsedQuestion.optionCounts.insert(it.key(), qint64(it.value().toDouble()));
        }
        const auto previews = row.value(QStringLiteral("option_voter_previews"));
        if (!previews.isUndefined()) {
            if (!parseVoterPreviews(previews, parsedQuestion.optionVoterPreviews)) return false;
            parsedQuestion.hasVoterPreviews = true;
        }
        parsed.questions.append(parsedQuestion);
    }
    const auto myVotes = state.value(QStringLiteral("my_votes"));
    if (!myVotes.isArray()) return false;
    for (const auto &vote : myVotes.toArray()) {
        if (!vote.isObject()) return false;
        const auto row = vote.toObject();
        if (!isUuid(row.value(QStringLiteral("question_uuid")).toString())) return false;
        const auto options = row.value(QStringLiteral("option_uuids"));
        if (!options.isArray() || !everyUuid(options.toArray())) return false;
        QStringList optionUuids;
        for (const auto &option : options.toArray())
            optionUuids.append(option.toString());
        parsed.myVotes.append({row.value(QStringLiteral("question_uuid")).toString(), optionUuids});
    }
    parsed.revision = qint64(revision.toDouble());
    parsed.isClosed = state.value(QStringLiteral("is_closed")).toBool();
    parsed.voterCount = qint64(state.value(QStringLiteral("voter_count")).toDouble());
    out = std::move(parsed);
    return true;
}
} // namespace

std::optional<Stream> parseStream(const QJsonObject &json, const QString &project)
{
    const auto id = json.value("uuid").toString();
    const auto unread = counter(json, "unread_count"), active = counter(json, "active_unread_count"), passive = counter(json, "passive_unread_count");
    const auto updated = optionalUpdatedMicros(json);
    if (!updated || (json.contains("is_archived") && !json.value("is_archived").isBool()) || !isUuid(id) || json.value("project_id").toString() != project
        || !json.value("name").isString() || !json.value("encryption").isBool()
        || (json.contains("private") && !json.value("private").isBool())
        || !unread || !active || !passive || !validLastMessage(json)
        || !optionalMode(json, {"all_messages", "mentions_only", "muted"}))
        return std::nullopt;
    for (const auto *field : {"owner", "user_uuid"})
        if (!optionalUuid(json, field, false)) return {};
    for (const auto *field : {"current_encryption_key_uuid", "default_topic_uuid", "provider_uuid"})
        if (!optionalUuid(json, field, true)) return {};
    if (json.contains("source_name") && json.value("source_name") != "native" && json.value("source_name") != "zulip") return {};
    if (json.contains("provider_identity") && !json.value("provider_identity").isNull()
        && !wireText(json, "provider_identity", 1, 255)) return {};
    Stream stream{id, json.value("name").toString(), json.value("description").toString(),
                  json.value("direct_user_uuid").toString(), json.value("role").toString(),
                  *unread, json.value("encryption").toBool(),
                  json.value("is_archived").toBool(), *active, *passive, json.value("last_message_uuid").toString(),
                  json.value("notification_mode").toString(), json.value("private").toBool(), *updated};
    bool complete = true;
    for (const auto *field : {"owner", "user_uuid", "source_name", "current_encryption_key_uuid",
                             "default_topic_uuid", "provider_uuid", "provider_identity"}) complete = complete && json.contains(field);
    if (complete) stream.security = StreamCryptoMetadata{json.value("owner").toString(), json.value("user_uuid").toString(),
        json.value("source_name").toString(), json.value("current_encryption_key_uuid").toString(),
        json.value("default_topic_uuid").toString(), json.value("provider_uuid").toString(), json.value("provider_identity").toString()};
    const auto color = json.value("color").toDouble(-1);
    if (json.value("color").isDouble() && std::isfinite(color) && color >= 0 && color <= 0xffffff && std::floor(color) == color)
        stream.color = quint32(color);
    if (json.value("default_topic_uuid").isString()) stream.defaultTopic = json.value("default_topic_uuid").toString();
    return stream;
}

ProfileDirectNavigation resolveProfileDirectNavigation(
    const QString &directUserUuid,
    const QVector<QPair<QString, QString>> &directStreams,
    const QHash<QString, QString> &defaultTopics,
    const QString &currentStreamUuid)
{
    ProfileDirectNavigation result;
    for (const auto &[streamUuid, peer] : directStreams) {
        if (peer != directUserUuid) continue;
        result.streamUuid = streamUuid;
        // Already on this DM stream (stream view or any topic): stay put.
        if (!currentStreamUuid.isEmpty() && currentStreamUuid == streamUuid) {
            result.status = ProfileDirectNavigation::Status::AlreadyOpen;
            return result;
        }
        const auto defaultTopic = defaultTopics.value(streamUuid);
        if (!isUuid(defaultTopic)) {
            result.status = ProfileDirectNavigation::Status::MissingDefaultTopic;
            return result;
        }
        result.status = ProfileDirectNavigation::Status::OpenDefaultTopic;
        result.topicUuid = defaultTopic;
        return result;
    }
    result.status = ProfileDirectNavigation::Status::StreamMissing;
    return result;
}

std::optional<Topic> parseTopic(const QJsonObject &json, const QString &project,
                              const QString &stream)
{
    const auto id = json.value("uuid").toString();
    const auto unread = counter(json, "unread_count"), active = counter(json, "active_unread_count"), passive = counter(json, "passive_unread_count");
    const auto updated = optionalUpdatedMicros(json);
    if (!isUuid(id) || json.value("project_id").toString() != project
        || json.value("stream_uuid").toString() != stream || !json.value("name").isString()
        || !unread || !active || !passive || !validLastMessage(json) || !updated
        || !optionalMode(json, {"default", "mute", "unmute", "follow"}))
        return std::nullopt;
    return Topic{id, stream, json.value("name").toString(), *unread,
                 json.value("is_done").toBool(), json.value("is_default").toBool(),
                 *active, *passive, json.value("last_message_uuid").toString(),
                 json.value("notification_mode").toString("default"), *updated};
}

std::optional<Message> parseMessage(const QJsonObject &json, const QString &project)
{
    Message message;
    message.uuid = json.value("uuid").toString();
    message.project = json.value("project_id").toString();
    message.stream = json.value("stream_uuid").toString();
    message.topic = json.value("topic_uuid").toString();
    message.author = json.value("author_uuid").toString();
    message.created = QDateTime::fromString(json.value("created_at").toString(), Qt::ISODateWithMs);
    if (!message.created.isValid())
        message.created = QDateTime::fromString(json.value("created_at").toString(), Qt::ISODate);
    static const QRegularExpression fractionalSeconds("\\.([0-9]{1,6})(?:Z|[+-][0-9]{2}:[0-9]{2})$");
    const auto fraction = fractionalSeconds.match(json.value("created_at").toString());
    const auto micros = fraction.hasMatch()
        ? fraction.captured(1).leftJustified(6, QLatin1Char('0')).toInt() : 0;
    message.createdMicros = message.created.toMSecsSinceEpoch() * 1000 + micros % 1000;
    const auto payload = json.value("payload").toObject();
    message.kind = payload.value("kind").toString();
    if (!isUuid(message.uuid) || message.project != project || !isUuid(message.stream)
        || !isUuid(message.topic) || !isUuid(message.author) || !message.created.isValid()
        || message.kind.isEmpty())
        return std::nullopt;
    // Ciphertext and control payloads must never become visible message text.
    if (message.kind == "markdown") {
        if (!payload.value("content").isString())
            return std::nullopt;
        message.content = payload.value("content").toString();
    } else if (message.kind == "e2ee" || message.kind == "key_request" || message.kind == "key_grant"
               || message.kind == "key_reject" || message.kind == "key_announced") {
        if (!isUuid(payload.value("key_uuid").toString())) return {};
        MessageCryptoPayload crypto;
        crypto.keyUuid = payload.value("key_uuid").toString();
        if (message.kind == "e2ee" || message.kind == "key_grant") {
            if (!wireText(payload, "content", 1, message.kind == "e2ee" ? 213336 : 40000)) return {};
            crypto.encodedContent = payload.value("content").toString();
        }
        if (message.kind == "key_request") {
            if (!wireText(payload, "device_id", 0, 255) || !wireText(payload, "device_name", 0, 255)
                || !wireText(payload, "public_key", 1, 2048)) return {};
            crypto.deviceId = payload.value("device_id").toString();
            crypto.deviceName = payload.value("device_name").toString();
            crypto.encodedPublicKey = payload.value("public_key").toString();
        }
        if (message.kind == "key_grant" || message.kind == "key_reject") {
            if (!isUuid(payload.value("request_message_uuid").toString())
                || !wireText(payload, "sender_device_id", 0, 255) || !wireText(payload, "recipient_device_id", 0, 255)) return {};
            crypto.requestMessage = payload.value("request_message_uuid").toString();
            crypto.senderDevice = payload.value("sender_device_id").toString();
            crypto.recipientDevice = payload.value("recipient_device_id").toString();
        }
        message.crypto = std::move(crypto);
    } else if (message.kind == "poll") {
        // A poll message REQUIRES a valid poll_state beside its validated
        // payload (isWorkspaceMessengerMessageDto); malformed stays unparsed.
        PollPayload poll;
        if (!parsePollPayload(payload, poll)) return std::nullopt;
        PollState state;
        if (!parsePollState(json.value("poll_state"), state)) return std::nullopt;
        message.poll = std::move(poll);
        message.pollState = std::move(state);
    }
    // Every kind other than poll must carry poll_state == null (absent or null).
    if (message.kind != QLatin1String("poll")) {
        const auto state = json.value("poll_state");
        if (!state.isNull() && !state.isUndefined()) return std::nullopt;
    }
    message.own = json.value("is_own").toBool();
    message.read = json.value("read").toBool();
    message.starred = json.value("starred").toBool();
    if (json.value("mentioned").isBool())
        message.mentioned = json.value("mentioned").toBool();
    const auto counts = json.value("reactions"), users = json.value("reaction_users");
    if ((!counts.isUndefined() && !counts.isObject()) || (!users.isUndefined() && !users.isObject())) return {};
    message.reactionsKnown = counts.isObject() && users.isObject();
    const auto countObject = counts.toObject();
    for (auto it = countObject.begin(); it != countObject.end(); ++it) {
        const auto count = counter(countObject, it.key());
        if (!validReactionName(it.key()) || !count) return {};
        message.reactions.insert(it.key(), *count);
    }
    const auto userObject = users.toObject();
    for (auto it = userObject.begin(); it != userObject.end(); ++it) {
        if (!validReactionName(it.key()) || !it.value().isArray() || it.value().toArray().isEmpty()) return {};
        QStringList ids;
        for (const auto &value : it.value().toArray()) {
            if (!isUuid(value.toString()) || ids.contains(value.toString())) return {};
            ids.append(value.toString());
        }
        if (!message.reactions.contains(it.key()) || ids.size() > message.reactions.value(it.key())) return {};
        message.reactionUsers.insert(it.key(), ids);
    }
    return message;
}

std::optional<Draft> parseDraft(const QJsonObject &json, const QString &project,
                               const QString &user, const QByteArray &etag)
{
    Draft draft;
    draft.uuid = json.value("uuid").toString();
    draft.project = json.value("project_id").toString();
    draft.user = json.value("user_uuid").toString();
    draft.stream = json.value("stream_uuid").toString();
    draft.topic = json.value("topic_uuid").toString();
    const auto payload = json.value("payload").toObject();
    const auto revision = counter(json, "revision");
    draft.createdAt = json.value("created_at").toString();
    draft.updatedAt = json.value("updated_at").toString();
    const auto updated = optionalUpdatedMicros(json);
    if (!isUuid(project) || !isUuid(user) || draft.project != project || draft.user != user
        || !isUuid(draft.uuid) || !isUuid(draft.stream) || !isUuid(draft.topic)
        || !revision || *revision < 1 || !updated || !json.contains("updated_at")
        || !QDateTime::fromString(draft.createdAt, Qt::ISODateWithMs).isValid()
        || payload.value("kind").toString() != "markdown" || !wireText(payload, "content", 1, 40000)) return {};
    draft.content = payload.value("content").toString();
    // Preserve the server's canonical text, including Python's trimming rules.
    // The body must still contain a non-whitespace code point.
    if (std::all_of(draft.content.cbegin(), draft.content.cend(), pythonSpace)) return {};
    draft.revision = *revision;
    draft.updatedMicros = *updated;
    draft.etag = '"' + QByteArray::number(draft.revision) + '"';
    // Collection rows omit ETag. A supplied header must agree exactly with the
    // revision; weak/malformed/mismatched tags cannot authorize an overwrite.
    if (!etag.isEmpty() && etag != draft.etag) return {};
    return draft;
}

QVector<Message> mergeMessages(const QVector<Message> &existing, const QVector<Message> &incoming)
{
    QHash<QString, Message> byUuid;
    for (const auto &message : existing)
        byUuid.insert(message.uuid, message);
    for (const auto &message : incoming)
        byUuid.insert(message.uuid, message);
    QVector<Message> result;
    result.reserve(byUuid.size());
    for (const auto &message : byUuid)
        result.append(message);
    std::sort(result.begin(), result.end(), [](const Message &a, const Message &b) {
        return a.createdMicros == b.createdMicros ? a.uuid < b.uuid : a.createdMicros < b.createdMicros;
    });
    return result;
}

bool applyPollRevisionGuard(Message &incoming, const Message &previous)
{
    if (incoming.kind != QLatin1String("poll") || previous.kind != QLatin1String("poll")
        || !incoming.pollState || !previous.pollState)
        return false;
    if (previous.pollState->revision > incoming.pollState->revision) {
        // The reference store keeps the whole previous poll snapshot (payload
        // and state) when an older revision arrives (message.model.ts).
        incoming.poll = previous.poll;
        incoming.pollState = previous.pollState;
        return true;
    }
    if (previous.pollState->revision == incoming.pollState->revision
        && previous.pollState->isClosed && !incoming.pollState->isClosed)
        incoming.pollState->isClosed = true;
    return false;
}

QString serializeComposerMentions(const QString &value, const QVector<ComposerMention> &mentions)
{
    // message-composer-body.lib.ts serializeWorkspaceComposerMentions: visible
    // names convert to "[Name](urn:user:uuid)" only at a non-word boundary, the
    // longest visible text wins, emails and ambiguous bare names stay literal.
    if (value.isEmpty() || mentions.isEmpty()) return value;
    QVector<const ComposerMention *> unique;
    for (const auto &mention : mentions)
    {
        if (sourceTrimmed(mention.visibleText).isEmpty()) continue;
        const auto existing = std::find_if(unique.begin(), unique.end(),
            [&mention](const ComposerMention *candidate) { return candidate->visibleText == mention.visibleText; });
        if (existing != unique.end()) *existing = &mention;
        else unique.append(&mention);
    }
    if (unique.isEmpty()) return value;
    std::stable_sort(unique.begin(), unique.end(), [](const ComposerMention *left, const ComposerMention *right) {
        return left->visibleText.size() > right->visibleText.size();
    });
    QStringList alternatives;
    for (const auto *mention : unique)
        alternatives << QRegularExpression::escape(mention->visibleText);
    const QRegularExpression pattern(QStringLiteral("(^|[^\\p{L}\\p{N}_.-])@(")
        + alternatives.join(QLatin1Char('|')) + QStringLiteral(")(?![\\p{L}\\p{N}_.-])"));
    QString result;
    qsizetype consumed = 0;
    auto match = pattern.globalMatch(value);
    while (match.hasNext())
    {
        const auto found = match.next();
        const ComposerMention *mention = nullptr;
        for (const auto *candidate : unique)
            if (candidate->visibleText == found.captured(2)) { mention = candidate; break; }
        const auto converted = mention == nullptr ? QString()
            : buildWorkspaceUserMention(mention->displayName, mention->userUuid);
        if (converted.isEmpty()) continue; // ambiguous or unconvertible: stays literal
        result += value.mid(consumed, found.capturedStart() - consumed);
        result += found.captured(1) + converted;
        consumed = found.capturedEnd();
    }
    return result + value.mid(consumed);
}

}
