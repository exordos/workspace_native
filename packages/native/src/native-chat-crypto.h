#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

namespace workspace {

// Binary provider boundary, matching chat-real-crypto-provider.lib.ts. No key
// storage, HTTP, presentation, ownership decisions or plaintext persistence.
class NativeChatCrypto final {
public:
    enum class Failure { None, Unavailable, InvalidKey, InvalidCiphertext, ContextMismatch };
    struct Result {
        Failure failure = Failure::Unavailable;
        QByteArray bytes;
        explicit operator bool() const { return failure == Failure::None; }
    };
    struct KeyPair {
        Failure failure = Failure::Unavailable;
        QByteArray publicKey;
        QByteArray privateKey;
        explicit operator bool() const { return failure == Failure::None; }
    };
    struct Context { QString streamUuid; QString keyUuid; };
    struct GrantContext {
        QString streamUuid;
        QString keyUuid;
        QString requestMessageUuid;
        QString senderDeviceId;
        QString recipientDeviceId;
    };
    // Matches the native HTTP response bound, rather than attempting multi-GB
    // allocations. The composer must separately enforce its character limit.
    static constexpr qsizetype MaximumMessageBytes = 16 * 1024 * 1024;
    static Result createGroupKey();
    static KeyPair createRequestKeyPair();
    static Result encryptMessage(QByteArrayView groupKey, QByteArrayView plaintext, const Context &context);
    static Result decryptMessage(QByteArrayView groupKey, QByteArrayView envelope, const Context &context);
    static Result encryptGroupKeyGrant(QByteArrayView groupKey, QByteArrayView recipientPublicKey,
                                      const GrantContext &context);
    static Result decryptGroupKeyGrant(QByteArrayView privateKey, QByteArrayView publicKey,
                                      QByteArrayView envelope, const GrantContext &context);
};

} // namespace workspace
