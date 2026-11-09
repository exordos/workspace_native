#pragma once

#include "native-draft-service.h"
#include <QObject>
#include <memory>

namespace workspace {

// Composer state over the existing storage worker. The caller supplies only a
// verified account owner and an authorized ordinary conversation.
class NativeDrafts final : public QObject {
    Q_OBJECT
public:
    struct Token {
        QString owner, stream, topic, uuid, snapshot, text;
        // Retain the conversation/snapshot through an asynchronous send or
        // attachment operation. Copies share one lifetime; no extra body copy.
        std::shared_ptr<const void> residency{};
    };
    // Clean cache only. Current input and pending user work are retained
    // separately; serialized payload bytes are not a process-RSS claim.
    static constexpr int MaximumCachedRecords = 64;
    static constexpr int MaximumCachedRoutes = 128;
    static constexpr qsizetype MaximumCachedBytes = 4 * 1024 * 1024;
    struct Residency {
        int records = 0, cachedRecords = 0, routes = 0, cachedRoutes = 0, owners = 0;
        qsizetype cachedBytes = 0;
    };
    Residency residency() const;
    bool retained(const QString &stream, const QString &topic) const;
    using Done = std::function<void(bool)>;
    explicit NativeDrafts(NativeDraftService *storage, QObject *parent = nullptr);
    ~NativeDrafts() override;
    void setOwner(const QString &owner);
    QString owner() const;
    void select(const QString &stream, const QString &topic);
    QString selectedStream() const;
    QString selectedTopic() const;
    QString text() const;
    QString text(const QString &stream, const QString &topic) const;
    bool ready(const QString &stream, const QString &topic) const;
    bool edit(const QString &stream, const QString &topic, const QString &text);
    Token capture(const QString &stream, const QString &topic) const;
    bool consume(const Token &token);
    NativeDraftService::Failure failure() const;
    void retry();
    // Wait for coalesced input and every write receipt, rather than merely the
    // worker's FIFO barrier. A failure keeps unsaved work in memory.
    void flush(QObject *receiver, Done done);

signals:
    void changed(const QString &owner, const QString &stream, const QString &topic);
    void stateChanged();

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace workspace
