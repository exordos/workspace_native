#pragma once

#include <QString>
#include <QVariantMap>

#include <optional>

namespace workspace {

enum class WorkspaceReferenceKind {
    User, Message, Stream, Topic, Quote, Forward, File, Image, Video, External, Time, Secret
};

struct WorkspaceReference {
    WorkspaceReferenceKind kind = WorkspaceReferenceKind::Message;
    QString id;
    QString streamUuid;
    QString target;
    QString label;
    bool silent = false;
    std::optional<QString> text;
    QString snapshotMarkdown;
    bool plainSnapshot = false;
    QString sourceLabel;
    QString sourceCreatedAt;
    bool directSource = false;
    QString contentType;
    std::optional<qint64> sizeBytes;
    std::optional<qint64> width;
    std::optional<qint64> height;
    QString url;
    QString utc;
    QString keyUuid;
    bool secretValid = false;

    // Secret parse results contain inert identity/validity metadata only. The
    // parser and this map never retain an envelope, ciphertext, or secret target.
    QVariantMap toVariantMap() const;
};

struct WorkspaceForwardOptions {
    bool plainSnapshot = false;
    std::optional<QString> sourceLabel;
    std::optional<QString> sourceCreatedAt;
    bool directSource = false;
};

// Parsing performs no I/O, routing, user resolution, resource loading or decryption.
std::optional<WorkspaceReference> parseWorkspaceReference(const QString &target,
    const QString &label = {}, bool silent = false);
bool isWorkspaceSecretReference(const QString &target);
QString parseWorkspaceMessageTime(const QString &value);
QString escapeWorkspaceMarkdownInline(const QString &value);
QString buildWorkspaceQuoteUrn(const QString &messageUuid, const QString &text = {});
QString buildWorkspaceForwardUrn(const QString &messageUuid, const QString &snapshotMarkdown,
    const WorkspaceForwardOptions &options = {});
QString buildWorkspaceUserMention(const QString &label, const QString &userUuid, bool silent = false);
QString buildWorkspaceQuoteReference(const QString &label, const QString &messageUuid,
    const QString &text = {});
QString buildWorkspaceForwardReference(const QString &label, const QString &messageUuid,
    const QString &snapshotMarkdown, const WorkspaceForwardOptions &options = {});

} // namespace workspace
