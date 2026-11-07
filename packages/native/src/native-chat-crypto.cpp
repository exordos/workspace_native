#include "native-chat-crypto.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hpke.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <array>
#include <memory>

namespace workspace {
namespace {
using Failure = NativeChatCrypto::Failure;
using Result = NativeChatCrypto::Result;
constexpr int KeyBytes = 32, TagBytes = 16, SaltBytes = 32;
const QByteArray GrantMagic = QByteArray::fromHex("5753434701");
constexpr OSSL_HPKE_SUITE Suite{OSSL_HPKE_KEM_ID_X25519, OSSL_HPKE_KDF_ID_HKDF_SHA256,
                              OSSL_HPKE_AEAD_ID_CHACHA_POLY1305};
using Cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using Pkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Hpke = std::unique_ptr<OSSL_HPKE_CTX, decltype(&OSSL_HPKE_CTX_free)>;

const unsigned char *data(QByteArrayView bytes)
{
    return reinterpret_cast<const unsigned char *>(bytes.data());
}
unsigned char *data(QByteArray &bytes)
{
    return reinterpret_cast<unsigned char *>(bytes.data());
}
void wipe(QByteArray &bytes)
{
    // Used only for freshly allocated, uniquely owned temporary outputs. Input
    // views and caller-owned key/plaintext lifetimes remain the caller's job.
    if (!bytes.isEmpty()) OPENSSL_cleanse(bytes.data(), size_t(bytes.size()));
    bytes.clear();
}
struct DerivedKey {
    std::array<unsigned char, KeyBytes> bytes{};
    ~DerivedKey() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
    DerivedKey() = default;
    DerivedKey(const DerivedKey &) = delete;
    DerivedKey &operator=(const DerivedKey &) = delete;
};
bool derive(QByteArrayView key, QByteArrayView salt, QByteArrayView info, DerivedKey &output)
{
    const std::unique_ptr<EVP_KDF, decltype(&EVP_KDF_free)> kdf(EVP_KDF_fetch(nullptr, "HKDF", nullptr), EVP_KDF_free);
    if (!kdf) return false;
    const std::unique_ptr<EVP_KDF_CTX, decltype(&EVP_KDF_CTX_free)> ctx(EVP_KDF_CTX_new(kdf.get()), EVP_KDF_CTX_free);
    if (!ctx) return false;
    char digest[] = "SHA256", mode[] = "EXTRACT_AND_EXPAND";
    OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_MODE, mode, 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, const_cast<char *>(key.data()), size_t(key.size())),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<char *>(salt.data()), size_t(salt.size())),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, const_cast<char *>(info.data()), size_t(info.size())),
        OSSL_PARAM_construct_end()};
    return EVP_KDF_derive(ctx.get(), output.bytes.data(), output.bytes.size(), parameters) == 1;
}
QByteArray messageInfo(const NativeChatCrypto::Context &context)
{
    if (context.streamUuid.isEmpty() || context.keyUuid.isEmpty()) return {};
    return QJsonDocument(QJsonArray{"workspace-chat-crypto-v1", "message", context.streamUuid, context.keyUuid})
        .toJson(QJsonDocument::Compact);
}
QByteArray grantInfo(const NativeChatCrypto::GrantContext &context)
{
    if (context.streamUuid.isEmpty() || context.keyUuid.isEmpty() || context.requestMessageUuid.isEmpty()
        || context.senderDeviceId.isEmpty() || context.recipientDeviceId.isEmpty()) return {};
    const auto info = QJsonDocument(QJsonArray{"workspace-chat-crypto-v1", "grant", context.streamUuid,
        context.keyUuid, context.requestMessageUuid, context.senderDeviceId, context.recipientDeviceId})
        .toJson(QJsonDocument::Compact);
    // Valid Workspace UUID/device contexts fit OpenSSL's documented HPKE bound.
    return info.size() <= OSSL_HPKE_MAX_INFOLEN ? info : QByteArray{};
}
} // namespace

NativeChatCrypto::Result NativeChatCrypto::createGroupKey()
{
    QByteArray key(KeyBytes, Qt::Uninitialized);
    if (RAND_priv_bytes(data(key), KeyBytes) != 1) { wipe(key); return {}; }
    return {Failure::None, std::move(key)};
}

NativeChatCrypto::KeyPair NativeChatCrypto::createRequestKeyPair()
{
    const Pkey key(EVP_PKEY_Q_keygen(nullptr, nullptr, "X25519"), EVP_PKEY_free);
    if (!key) return {};
    QByteArray publicKey(KeyBytes, Qt::Uninitialized), privateKey(KeyBytes, Qt::Uninitialized);
    size_t publicLength = KeyBytes, privateLength = KeyBytes;
    if (EVP_PKEY_get_raw_public_key(key.get(), data(publicKey), &publicLength) != 1 || publicLength != KeyBytes
        || EVP_PKEY_get_raw_private_key(key.get(), data(privateKey), &privateLength) != 1 || privateLength != KeyBytes) {
        wipe(privateKey); return {};
    }
    return {Failure::None, std::move(publicKey), std::move(privateKey)};
}

NativeChatCrypto::Result NativeChatCrypto::encryptMessage(QByteArrayView groupKey, QByteArrayView plaintext,
                                                         const Context &context)
{
    if (groupKey.size() != KeyBytes) return {Failure::InvalidKey, {}};
    const auto info = messageInfo(context);
    if (info.isEmpty()) return {Failure::ContextMismatch, {}};
    if (plaintext.size() > MaximumMessageBytes) return {Failure::InvalidCiphertext, {}};
    QByteArray salt(SaltBytes, Qt::Uninitialized);
    if (RAND_bytes(data(salt), SaltBytes) != 1) return {};
    DerivedKey key;
    if (!derive(groupKey, salt, info, key)) return {};
    const Cipher cipher(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!cipher) return {};
    const std::array<unsigned char, 12> nonce{};
    const auto aad = info + salt;
    QByteArray ciphertext(plaintext.size() + TagBytes, Qt::Uninitialized);
    int length = 0, finalLength = 0;
    if (EVP_EncryptInit_ex(cipher.get(), EVP_chacha20_poly1305(), nullptr, key.bytes.data(), nonce.data()) != 1
        || EVP_EncryptUpdate(cipher.get(), nullptr, &length, data(aad), int(aad.size())) != 1
        || EVP_EncryptUpdate(cipher.get(), data(ciphertext), &length, data(plaintext), int(plaintext.size())) != 1
        || EVP_EncryptFinal_ex(cipher.get(), data(ciphertext) + length, &finalLength) != 1
        || length + finalLength != plaintext.size()
        || EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_AEAD_GET_TAG, TagBytes, data(ciphertext) + plaintext.size()) != 1) {
        wipe(ciphertext); return {Failure::InvalidCiphertext, {}};
    }
    return {Failure::None, salt + ciphertext};
}

NativeChatCrypto::Result NativeChatCrypto::decryptMessage(QByteArrayView groupKey, QByteArrayView envelope,
                                                         const Context &context)
{
    if (groupKey.size() != KeyBytes) return {Failure::InvalidKey, {}};
    const auto info = messageInfo(context);
    if (info.isEmpty()) return {Failure::ContextMismatch, {}};
    if (envelope.size() < SaltBytes + TagBytes || envelope.size() - SaltBytes - TagBytes > MaximumMessageBytes)
        return {Failure::InvalidCiphertext, {}};
    const auto salt = envelope.first(SaltBytes), ciphertext = envelope.sliced(SaltBytes);
    DerivedKey key;
    if (!derive(groupKey, salt, info, key)) return {};
    const Cipher cipher(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!cipher) return {};
    const std::array<unsigned char, 12> nonce{};
    const auto aad = info + QByteArray(salt.data(), salt.size());
    const int bodyBytes = int(ciphertext.size()) - TagBytes;
    QByteArray plaintext(bodyBytes + TagBytes, Qt::Uninitialized);
    int length = 0, finalLength = 0;
    if (EVP_DecryptInit_ex(cipher.get(), EVP_chacha20_poly1305(), nullptr, key.bytes.data(), nonce.data()) != 1
        || EVP_DecryptUpdate(cipher.get(), nullptr, &length, data(aad), int(aad.size())) != 1
        || EVP_DecryptUpdate(cipher.get(), data(plaintext), &length, data(ciphertext), bodyBytes) != 1
        || EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_AEAD_SET_TAG, TagBytes,
                              const_cast<unsigned char *>(data(ciphertext) + bodyBytes)) != 1
        || EVP_DecryptFinal_ex(cipher.get(), data(plaintext) + length, &finalLength) != 1
        || length + finalLength != bodyBytes) {
        wipe(plaintext); return {Failure::InvalidCiphertext, {}};
    }
    plaintext.resize(bodyBytes);
    return {Failure::None, std::move(plaintext)};
}

NativeChatCrypto::Result NativeChatCrypto::encryptGroupKeyGrant(QByteArrayView groupKey,
        QByteArrayView recipientPublicKey, const GrantContext &context)
{
    if (groupKey.size() != KeyBytes || recipientPublicKey.size() != KeyBytes) return {Failure::InvalidKey, {}};
    const auto info = grantInfo(context);
    if (info.isEmpty()) return {Failure::ContextMismatch, {}};
    const Hpke hpke(OSSL_HPKE_CTX_new(OSSL_HPKE_MODE_BASE, Suite, OSSL_HPKE_ROLE_SENDER, nullptr, nullptr),
                    OSSL_HPKE_CTX_free);
    if (!hpke) return {};
    QByteArray enc(KeyBytes, Qt::Uninitialized), ciphertext(KeyBytes + TagBytes, Qt::Uninitialized);
    size_t encLength = size_t(enc.size()), ciphertextLength = size_t(ciphertext.size());
    const auto aad = info + GrantMagic + QByteArray(recipientPublicKey.data(), recipientPublicKey.size());
    if (OSSL_HPKE_encap(hpke.get(), data(enc), &encLength, data(recipientPublicKey), size_t(recipientPublicKey.size()),
                        data(info), size_t(info.size())) != 1 || encLength != KeyBytes
        || OSSL_HPKE_seal(hpke.get(), data(ciphertext), &ciphertextLength, data(aad), size_t(aad.size()),
                         data(groupKey), size_t(groupKey.size())) != 1 || ciphertextLength != KeyBytes + TagBytes)
        return {Failure::InvalidKey, {}};
    return {Failure::None, GrantMagic + enc + ciphertext};
}

NativeChatCrypto::Result NativeChatCrypto::decryptGroupKeyGrant(QByteArrayView privateKey, QByteArrayView publicKey,
        QByteArrayView envelope, const GrantContext &context)
{
    const auto info = grantInfo(context);
    // Retain the source provider's validation order for incomplete request state.
    if (privateKey.size() != KeyBytes || publicKey.size() != KeyBytes || info.isEmpty()) return {Failure::InvalidKey, {}};
    if (envelope.size() != GrantMagic.size() + KeyBytes * 2 + TagBytes
        || envelope.first(GrantMagic.size()) != QByteArrayView(GrantMagic)) return {Failure::InvalidCiphertext, {}};
    const Pkey recipient(EVP_PKEY_new_raw_private_key_ex(nullptr, "X25519", nullptr, data(privateKey), KeyBytes),
                         EVP_PKEY_free);
    if (!recipient) return {Failure::InvalidKey, {}};
    const Hpke hpke(OSSL_HPKE_CTX_new(OSSL_HPKE_MODE_BASE, Suite, OSSL_HPKE_ROLE_RECEIVER, nullptr, nullptr),
                    OSSL_HPKE_CTX_free);
    if (!hpke) return {};
    const auto enc = envelope.sliced(GrantMagic.size(), KeyBytes), ciphertext = envelope.sliced(GrantMagic.size() + KeyBytes);
    const auto aad = info + GrantMagic + QByteArray(publicKey.data(), publicKey.size());
    QByteArray key(KeyBytes, Qt::Uninitialized); size_t keyLength = KeyBytes;
    if (OSSL_HPKE_decap(hpke.get(), data(enc), size_t(enc.size()), recipient.get(), data(info), size_t(info.size())) != 1
        || OSSL_HPKE_open(hpke.get(), data(key), &keyLength, data(aad), size_t(aad.size()),
                         data(ciphertext), size_t(ciphertext.size())) != 1 || keyLength != KeyBytes) {
        wipe(key); return {Failure::InvalidCiphertext, {}};
    }
    return {Failure::None, std::move(key)};
}

} // namespace workspace
