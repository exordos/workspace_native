#include "chat-background.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <QUrl>

namespace workspace {

namespace {
const QStringList &presetIdTable()
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
        if (presetIdTable().contains(id)) {
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
    const bool wasAvailable = m_authenticated;
    m_authenticated = true;
    if (m_owner == owner) {
        if (!wasAvailable)
            emit changed();
        return;
    }
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

void ChatBackground::setSession(const QString &owner, bool authenticated)
{
    if (!authenticated) {
        if (!m_authenticated && m_owner.isEmpty())
            return;
        m_owner.clear();
        m_global = Value{};
        m_chats.clear();
        m_error.clear();
        m_authenticated = false;
        emit changed();
        return;
    }
    setOwner(owner);
}

ChatBackground::Value ChatBackground::resolve(const QString &chatKey) const
{
    if (auto found = m_chats.constFind(chatKey); found != m_chats.constEnd())
        return found.value(); // an explicit default (Value{}) wins here
    return m_global;
}

bool ChatBackground::applyChat(const QString &chatKey, const Value &value)
{
    if (!m_authenticated || !isValidChatKey(chatKey))
        return false;
    auto staged = m_chats;
    staged.insert(chatKey, value);
    return storeStaged(m_global, staged);
}

bool ChatBackground::applyAll(const Value &value)
{
    if (!m_authenticated)
        return false;
    // reference scope "all" clears every chat override - on success only.
    return storeStaged(value, {});
}

QJsonObject ChatBackground::preferences() const
{
    return preferencesOf(m_global, m_chats);
}

QJsonObject ChatBackground::preferencesOf(const Value &global, const QHash<QString, Value> &chats)
{
    QJsonObject chatsJson;
    for (const auto &entry : chats.asKeyValueRange()) {
        if (entry.second.kind == Kind::Default)
            chatsJson.insert(entry.first, QJsonValue::Null);
        else
            chatsJson.insert(entry.first,
                QJsonObject{{entry.second.kind == Kind::Preset ? QStringLiteral("preset")
                                                               : QStringLiteral("image"),
                    entry.second.text}});
    }
    QJsonObject globalJson;
    if (global.kind == Kind::Preset)
        globalJson.insert(QStringLiteral("preset"), global.text);
    else if (global.kind == Kind::Image)
        globalJson.insert(QStringLiteral("image"), global.text);
    QJsonObject result;
    result.insert(QStringLiteral("global"),
        global.kind == Kind::Default ? QJsonValue::Null : QJsonValue(globalJson));
    result.insert(QStringLiteral("chats"), chatsJson);
    return result;
}

bool ChatBackground::storeStaged(const Value &global, const QHash<QString, Value> &chats)
{
    const auto raw = QString::fromUtf8(
        QJsonDocument(preferencesOf(global, chats)).toJson(QJsonDocument::Compact));
    // Reference saveChatBackgroundPreferences: an over-cap payload is a
    // storage error and the previous stored value stays authoritative.
    // Bug #677: live values are swapped only after a successful persist, so a
    // rejected apply leaves the previous live configuration untouched too.
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
    m_global = global;
    m_chats = chats;
    if (!m_error.isEmpty())
        m_error.clear();
    emit changed();
    return true;
}

namespace {
// Reference IMAGE_MAGIC_BYTES (shared/lib/validation.ts:52-68): table order
// png -> jpeg -> gif -> webp, webp additionally requires byte 8 == 'W'.
// Returns nullptr when nothing matches; gif is detectable but never in the
// accepted set, so a gif payload always lands on "type".
const char *sniffImageMagic(const QByteArray &bytes)
{
    if (bytes.size() < 12)
        return nullptr;
    const auto *u = reinterpret_cast<const uchar *>(bytes.constData());
    if (u[0] == 0x89 && u[1] == 0x50 && u[2] == 0x4e && u[3] == 0x47)
        return "image/png";
    if (u[0] == 0xff && u[1] == 0xd8 && u[2] == 0xff)
        return "image/jpeg";
    if (u[0] == 0x47 && u[1] == 0x49 && u[2] == 0x46 && u[3] == 0x38)
        return "image/gif";
    if (u[0] == 0x52 && u[1] == 0x49 && u[2] == 0x46 && u[3] == 0x46 && u[8] == 0x57)
        return "image/webp";
    return nullptr;
}
} // namespace

ChatBackground::Prepared ChatBackground::prepareImage(const QByteArray &bytes, const QString &declaredMime)
{
    Prepared out;
    const auto fail = [&out](const char *code) {
        out.error = QString::fromLatin1(code);
        return out;
    };
    const QStringList allowed{QStringLiteral("image/jpeg"), QStringLiteral("image/png"),
        QStringLiteral("image/webp")};
    if (!allowed.contains(declaredMime))
        return fail("type");
    if (bytes.isEmpty() || bytes.size() > 8 * 1024 * 1024)
        return fail("size");
    const auto sniffed = sniffImageMagic(bytes);
    if (!sniffed || QString::fromLatin1(sniffed) != declaredMime)
        return fail("type");
    // Orientation-aware decode (Bug #678): a JPEG may store its display
    // rotation in EXIF, so raw pixels must be read through QImageReader with
    // autoTransform - the same configuration the avatar pipeline uses
    // (avatar-images.cpp thumbnail()). The corrected pixels are baked into
    // the canvas below and survive the re-encode (the saved JPEG carries no
    // orientation metadata, matching the reference canvas contract).
    QImage image;
    {
        QBuffer buffer;
        buffer.setData(bytes);
        if (!buffer.open(QIODevice::ReadOnly))
            return fail("decode");
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);
        image = reader.read();
    }
    if (image.isNull())
        return fail("decode");
    const int width = image.width();
    const int height = image.height();
    if (!width || !height || qint64(width) * qint64(height) > 24'000'000)
        return fail("size");
    // Reference canvas: scale = min(1, 1600/w, 1600/h), white fill under the
    // draw (flattens alpha onto #ffffff), JPEG quality 80.
    const double scale = qMin(1.0, qMin(1600.0 / width, 1600.0 / height));
    const int targetWidth = qMax(1, qRound(width * scale));
    const int targetHeight = qMax(1, qRound(height * scale));
    QImage canvas(targetWidth, targetHeight, QImage::Format_RGB32);
    canvas.fill(Qt::white);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRect(0, 0, targetWidth, targetHeight), image, image.rect());
    painter.end();
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    const bool saved = canvas.save(&buffer, "JPEG", 80);
    buffer.close();
    if (!saved)
        return fail("decode");
    const QString dataUrl = QStringLiteral("data:image/jpeg;base64,")
        + QString::fromLatin1(encoded.toBase64());
    if (!dataUrl.startsWith(QStringLiteral("data:image/jpeg;base64,"))
        || dataUrl.size() > kMaxStoredLength - 100)
        return fail("size");
    out.ok = true;
    out.dataUrl = dataUrl;
    return out;
}

QStringList ChatBackground::presetIds() { return presetIdTable(); }

QStringList ChatBackground::presetIdList() const { return presetIdTable(); }

QString ChatBackground::backgroundUrl(const QString &chatKey) const
{
    const auto value = resolve(chatKey);
    if (value.kind == Kind::Preset)
        return QStringLiteral("qrc:/workspace/chat-background/%1.svg").arg(value.text);
    if (value.kind == Kind::Image)
        return value.text;
    return QString();
}

QString ChatBackground::selectedPreset(const QString &chatKey) const
{
    const auto value = resolve(chatKey);
    return value.kind == Kind::Preset ? value.text : QString();
}

QString ChatBackground::customImage(const QString &chatKey) const
{
    const auto value = resolve(chatKey);
    return value.kind == Kind::Image ? value.text : QString();
}

namespace {
bool scopeApplies(const QString &scope) { return scope == QLatin1String("all") || scope == QLatin1String("chat"); }
} // namespace

bool ChatBackground::applyPreset(const QString &chatKey, const QString &presetId,
                                 const QString &scope)
{
    if (!scopeApplies(scope) || !presetIdTable().contains(presetId))
        return false;
    const Value value{Kind::Preset, presetId};
    return scope == QLatin1String("all") ? applyAll(value) : applyChat(chatKey, value);
}

bool ChatBackground::applyImage(const QString &chatKey, const QString &dataUrl,
                                const QString &scope)
{
    QJsonValue candidate;
    if (!scopeApplies(scope))
        return false;
    // The same validator the restore path uses: a JPEG data URL within the
    // stored cap, nothing else (reference apply() receives prepared URLs).
    if (!jpegDataUrlPattern().match(dataUrl).hasMatch()
        || dataUrl.size() > kMaxImageLength)
        return false;
    Q_UNUSED(candidate);
    const Value value{Kind::Image, dataUrl};
    return scope == QLatin1String("all") ? applyAll(value) : applyChat(chatKey, value);
}

bool ChatBackground::applyDefault(const QString &chatKey, const QString &scope)
{
    if (!scopeApplies(scope))
        return false;
    return scope == QLatin1String("all") ? applyAll(Value{}) : applyChat(chatKey, Value{});
}

void ChatBackground::clearError()
{
    if (m_error.isEmpty())
        return;
    m_error.clear();
    emit changed();
}

QVariantMap ChatBackground::prepareImageFromFile(const QString &localPath)
{
    QVariantMap result;
    QString path = localPath;
    if (path.startsWith(QLatin1String("file:")))
        path = QUrl(path).toLocalFile();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // Reference upload() maps any non-BackgroundImageError failure to
        // the decode line (chat-background.ui.tsx catch branch).
        result[QStringLiteral("ok")] = false;
        result[QStringLiteral("error")] = QStringLiteral("decode");
        return result;
    }
    const auto suffix = QFileInfo(path).suffix().toLower();
    QString mime;
    if (suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg"))
        mime = QStringLiteral("image/jpeg");
    else if (suffix == QLatin1String("png"))
        mime = QStringLiteral("image/png");
    else if (suffix == QLatin1String("webp"))
        mime = QStringLiteral("image/webp");
    // Any other extension keeps the declared MIME empty, so prepareImage
    // rejects it on the allow-list exactly like an unknown file.type does.
    const auto prepared = prepareImage(file.readAll(), mime);
    file.close();
    result[QStringLiteral("ok")] = prepared.ok;
    result[QStringLiteral("dataUrl")] = prepared.dataUrl;
    result[QStringLiteral("error")] = prepared.error;
    return result;
}

} // namespace workspace
