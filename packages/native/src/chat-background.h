#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSettings>

namespace workspace {

// Chat backgrounds (issue #637, docs/NATIVE_DESKTOP_CHAT_BACKGROUNDS.md):
// a faithful clone of the reference chat-background.lib.ts at workspace_ui
// ca73acf. Values are one of three bundled presets, a bounded JPEG data URL,
// or null (the default background = untouched theme surface). Storage is
// local-only per runtime owner - nothing is uploaded or synced (reference
// docs/CHAT_BACKGROUNDS.md:7,11).
class ChatBackground final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    enum class Kind { Default, Preset, Image };
    struct Value {
        Kind kind = Kind::Default;
        QString text; // preset id or data:image/jpeg;base64,... URL
    };

    explicit ChatBackground(QSettings *settings, QObject *parent = nullptr);

    bool available() const { return !m_owner.isEmpty(); }
    QString error() const { return m_error; }
    QString owner() const { return m_owner; }

    // Runtime owner switch: reloads that owner's stored preferences and drops
    // the previous owner's in-memory state (the reference keys localStorage
    // per owner; writes from a departed owner are dropped by the stale guard).
    void setOwner(const QString &owner);

    // chatKey format from the reference: ^(stream|topic):[0-9a-f:-]+$ (case
    // insensitive on the prefix). A stored explicit chat default (null) wins
    // over the global value; a missing key falls back to the global value.
    Value resolve(const QString &chatKey) const;
    // "Only this chat": invalid (and a no-op) without a well-formed chatKey.
    bool applyChat(const QString &chatKey, const Value &value);
    // "For all chats": replaces the global value and clears every chat
    // override (reference apply scope "all").
    bool applyAll(const Value &value);
    // "Default background": clears the value for the scope, exactly the two
    // apply calls above with a default Value.
    bool resetChat(const QString &chatKey) { return applyChat(chatKey, Value{}); }

    // Reference restore rules over the stored raw JSON string (also the
    // contract test surface). Returns the normalized {"global":...,"chats":{}}
    // object; everything malformed collapses to the empty preferences.
    static QJsonObject parsePreferences(const QString &raw);
    // Reference validateChatBackground: preset id from the fixed table, or a
    // JPEG data URL within the stored-image cap. Returns false for anything
    // else (including JSON null - the caller keeps explicit-null chat keys).
    static bool validateValue(const QJsonValue &candidate, Value *out);
    static bool isValidChatKey(const QString &chatKey);

    // Caps cloned verbatim from the reference (characters, not bytes).
    static constexpr qsizetype kMaxRawLength = 4'000'000;
    static constexpr qsizetype kMaxStoredLength = 2'000'000;
    static constexpr qsizetype kMaxImageLength = 1'998'990; // kMaxStoredLength - len('{"image":""}')

signals:
    void changed();

private:
    QJsonObject preferences() const;
    bool store();

    QPointer<QSettings> m_settings;
    QString m_owner;
    Value m_global;
    QHash<QString, Value> m_chats; // only keys explicitly present in storage
    QString m_error;
};

} // namespace workspace
