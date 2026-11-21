#pragma once

#include "native-test-harness.h"

class ControllerMemorySecrets final : public SessionSecretStore {
public:
    QMap<QString, QByteArray> values;
    void write(const QString &key, const QByteArray &secret, Completion done) override {
        values.insert(key, secret); QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
    void read(const QString &key, Loaded done) override {
        const auto value = values.value(key);
        QTimer::singleShot(0, this, [done, value] { done(value, value.isEmpty() ? Error::NotFound : Error::None); });
    }
    void remove(const QString &key, Completion done) override {
        values.remove(key); QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
};

// Secure store that rejects removals: writes and reads behave like
// ControllerMemorySecrets, but a forget leaves the credential on disk.
class ControllerFailingRemovalSecrets final : public SessionSecretStore {
public:
    QMap<QString, QByteArray> values;
    void write(const QString &key, const QByteArray &secret, Completion done) override {
        values.insert(key, secret); QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
    void read(const QString &key, Loaded done) override {
        const auto value = values.value(key);
        QTimer::singleShot(0, this, [done, value] { done(value, value.isEmpty() ? Error::NotFound : Error::None); });
    }
    void remove(const QString &, Completion done) override {
        QTimer::singleShot(0, this, [done] { done(Error::Failed); });
    }
};

// Secure store that holds a removal in flight: the completion is parked until
// the test invokes releaseRemove, mirroring the mock-server Response `defer`
// pattern so races can be reproduced without sleeping.
class ControllerHeldRemovalSecrets final : public SessionSecretStore {
public:
    QMap<QString, QByteArray> values;
    std::function<void()> releaseRemove;
    void write(const QString &key, const QByteArray &secret, Completion done) override {
        values.insert(key, secret); QTimer::singleShot(0, this, [done] { done(Error::None); });
    }
    void read(const QString &key, Loaded done) override {
        const auto value = values.value(key);
        QTimer::singleShot(0, this, [done, value] { done(value, value.isEmpty() ? Error::NotFound : Error::None); });
    }
    void remove(const QString &key, Completion done) override {
        releaseRemove = [this, key, done] { values.remove(key); done(Error::None); };
    }
};

inline QString conversationPreferenceOwner(const QUrl &origin, const QString &user = userId,
                                           const QString &project = projectId)
{
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(QJsonArray{
        origin.toString(QUrl::FullyEncoded), project, user}).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}
