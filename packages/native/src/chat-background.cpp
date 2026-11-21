#include "chat-background.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace workspace {

namespace {
const QStringList &presetIds()
{
    static const QStringList ids{QStringLiteral("mountains"), QStringLiteral("sea"),
        QStringLiteral("forest")}; // reference CHAT_BACKGROUNDS order
    return ids;
}

const QRegularExpression &chatKeyPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^(stream|topic):[0-9a-f:-]+$"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern;
}

const QRegularExpression &jpegDataUrlPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^data:image/jpeg;base64,[A-Za-z0-9+/]+={0,2}$"));
    return pattern;
}
} // namespace

ChatBackground::ChatBackground(QSettings *settings, QObject *parent)
    : QObject(parent), m_settings(settings)
{
}

bool ChatBackground::isValidChatKey(const QString &chatKey)
{
    return chatKeyPattern().match(chatKey).hasMatch();
}

bool ChatBackground::validateValue(const QJsonValue &candidate, Value *out)
{
    *out = Value{};
    if (!candidate.isObject())
        return false;
    const auto object = candidate.toObject();
    if (object.contains(QStringLiteral("preset"))) {
        const auto id = candidate.toObject().value(QStringLiteral("preset")).toVariant().toString();
        if (presetIds().contains(id)) {
            out->kind = Kind::Preset;
            out->text = id;
            return true;
        }
        return false;
    }
    if (object.contains(QStringLiteral("image"))) {
        const auto image = object.value(QStringLiteral("image"));
        if (!image.isString() || image.toString().size() > kMaxImageLength
            || !jpegDataUrlPattern().match(image.toString()).hasMatch())
            return false;
        out->kind = Kind::Image;
        out->text = image.toString();
        return true;
    }
    return false;
}

QJsonObject ChatBackground::parsePreferences(const QString &raw)
{
    const auto empty = [] {
        QJsonObject result;
        result.insert(QStringLiteral("global"), QJsonValue::Null);
        result.insert(QStringLiteral("chats"), QJsonObject{});
        return result;
    };
    if (raw.isEmpty() || raw.size() > kMaxRawLength)
        return empty();
    const auto parsed = QJsonDocument::fromJson(raw.toUtf8());
    if (!parsed.isObject())
        return empty();
    const auto value = parsed.object();
    const bool wrapped = value.contains(QStringLiteral("global"))
        && value.contains(QStringLiteral("chats"))
        && value.value(QStringLiteral("chats")).isObject();
    if (!wrapped) {
        // Legacy bare value: only honoured for raw strings up to the stored
        // cap (reference fallback branch).
        Value legacy;
        QJsonObject result;
        result.insert(QStringLiteral("chats"), QJsonObject{});
        QJsonValue global = QJsonValue::Null;
        if (raw.size() <= kMaxStoredLength && validateValue(value, &legacy))
            global = QJsonValue{QJsonObject{{legacy.kind == Kind::Preset
                    ? QStringLiteral("preset") : QStringLiteral("image"), legacy.text}}};
        result.insert(QStringLiteral("global"), global);
        return result;
    }
    QJsonObject chats;
    const auto chatsIn = value.value(QStringLiteral("chats")).toObject();
    for (auto it = chatsIn.begin(); it != chatsIn.end(); ++it) {
        if (!isValidChatKey(it.key()))
            continue;
        if (it.value().isNull()) { // explicit per-chat default survives
            chats.insert(it.key(), QJsonValue::Null);
            continue;
        }
        Value validated;
        if (validateValue(it.value(), &validated))
            chats.insert(it.key(), QJsonValue{QJsonObject{
                {validated.kind == Kind::Preset ? QStringLiteral("preset")
                                                : QStringLiteral("image"),
                    validated.text}}});
    }
    Value global;
    QJsonObject result;
    result.insert(QStringLiteral("chats"), chats);
    const auto globalIn = value.value(QStringLiteral("global"));
    QJsonValue globalOut = QJsonValue::Null;
    if (!globalIn.isNull() && validateValue(globalIn, &global))
        globalOut = QJsonValue{QJsonObject{{global.kind == Kind::Preset
                ? QStringLiteral("preset") : QStringLiteral("image"), global.text}}};
    result.insert(QStringLiteral("global"), globalOut);
    return result;
}

void ChatBackground::setOwner(const QString &owner)
{
    if (m_owner == owner)
        return;
    m_owner = owner;
    m_global = Value{};
    m_chats.clear();
    m_error.clear();
    if (m_settings && !owner.isEmpty()) {
        const auto stored = m_settings
            ->value(QStringLiteral("chatBackground/v1/%1").arg(owner)).toString();
        const auto prefs = parsePreferences(stored);
        const auto global = prefs.value(QStringLiteral("global"));
        validateValue(global, &m_global); // null/invalid lands back on default
        const auto chats = prefs.value(QStringLiteral("chats")).toObject();
        for (auto it = chats.begin(); it != chats.end(); ++it) {
            Value value;
            if (it.value().isNull())
                m_chats.insert(it.key(), Value{}); // explicit default
            else if (validateValue(it.value(), &value))
                m_chats.insert(it.key(), value);
        }
    }
    emit changed();
}

ChatBackground::Value ChatBackground::resolve(const QString &chatKey) const
{
    if (auto found = m_chats.constFind(chatKey); found != m_chats.constEnd())
        return found.value(); // an explicit default (Value{}) wins here
    return m_global;
}

bool ChatBackground::applyChat(const QString &chatKey, const Value &value)
{
    if (m_owner.isEmpty() || !isValidChatKey(chatKey))
        return false;
    m_chats.insert(chatKey, value);
    return store();
}

bool ChatBackground::applyAll(const Value &value)
{
    if (m_owner.isEmpty())
        return false;
    m_global = value;
    m_chats.clear(); // reference scope "all" clears every chat override
    return store();
}

QJsonObject ChatBackground::preferences() const
{
    QJsonObject chats;
    for (const auto &entry : m_chats.asKeyValueRange()) {
        if (entry.second.kind == Kind::Default)
            chats.insert(entry.first, QJsonValue::Null);
        else
            chats.insert(entry.first,
                QJsonObject{{entry.second.kind == Kind::Preset ? QStringLiteral("preset")
                                                                : QStringLiteral("image"),
                    entry.second.text}});
    }
    QJsonObject global;
    if (m_global.kind == Kind::Preset)
        global.insert(QStringLiteral("preset"), m_global.text);
    else if (m_global.kind == Kind::Image)
        global.insert(QStringLiteral("image"), m_global.text);
    QJsonObject result;
    result.insert(QStringLiteral("global"),
        m_global.kind == Kind::Default ? QJsonValue::Null : QJsonValue(global));
    result.insert(QStringLiteral("chats"), chats);
    return result;
}

bool ChatBackground::store()
{
    const auto raw = QString::fromUtf8(
        QJsonDocument(preferences()).toJson(QJsonDocument::Compact));
    // Reference saveChatBackgroundPreferences: an over-cap payload is a
    // storage error and the previous stored value stays authoritative.
    if (raw.size() > kMaxRawLength || !m_settings) {
        m_error = QStringLiteral("storage");
        emit changed();
        return false;
    }
    m_settings->setValue(QStringLiteral("chatBackground/v1/%1").arg(m_owner), raw);
    if (m_settings->status() != QSettings::NoError) {
        m_error = QStringLiteral("storage");
        emit changed();
        return false;
    }
    if (!m_error.isEmpty())
        m_error.clear();
    emit changed();
    return true;
}

} // namespace workspace
