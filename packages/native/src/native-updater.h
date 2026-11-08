#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTemporaryDir;
class QTemporaryFile;

namespace workspace {

// Public release transport only: never shares the authenticated Workspace client.
class NativeUpdater final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(qreal progress READ progress NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY changed)
    Q_PROPERTY(QString downloadedPath READ downloadedPath NOTIFY changed)

public:
    static constexpr qint64 MaximumMetadataBytes = 4 * 1024 * 1024;
    static constexpr qint64 MaximumArchiveBytes = 512LL * 1024 * 1024;
    explicit NativeUpdater(QObject *parent = nullptr);
    // C++ fixture injection only, never a QML invokable or runtime setting.
    // The alternative metadata endpoint must be literal IPv4/IPv6 loopback HTTP or HTTPS.
    // HTTPS is metadata-only; normal production download/redirect validation remains unchanged.
    NativeUpdater(const QUrl &releaseUrl, const QString &currentVersion,
                  const QString &cachePath, QObject *parent = nullptr);
    ~NativeUpdater() override;

    QString state() const { return m_state; }
    qreal progress() const { return m_progress; }
    QString error() const { return m_error; }
    QString currentVersion() const { return m_currentVersion; }
    QString availableVersion() const { return m_availableVersion; }
    QString downloadedPath() const { return m_downloadedPath; }

    Q_INVOKABLE void check();
    Q_INVOKABLE void download();
    Q_INVOKABLE void install();
    Q_INVOKABLE void restart();
    Q_INVOKABLE void cancel();

signals:
    void changed();
    void restartRequested(const QString &managedExecutable);

private:
    void stopReply();
    void discardFile();
    void stopProcess();
    void startProcess(quint64 ticket, const QString &phase, const QString &program,
                      const QStringList &arguments);
    void readProcess(quint64 ticket);
    void processFinished(quint64 ticket, int exitCode, bool normalExit);
    bool validateListing() const;
    bool installedVersionMatches() const;
    void fail(quint64 ticket, const QString &message);
    void startRequest(quint64 ticket, const QUrl &url, bool metadata);
    void consume(quint64 ticket, QNetworkReply *reply, bool metadata);
    void finished(quint64 ticket, QNetworkReply *reply, bool metadata);
    void parseRelease(quint64 ticket);
    bool allowedDownload(const QUrl &url, bool redirect) const;
    bool busy() const;

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QPointer<QProcess> m_process;
    QTimer m_deadline;
    QUrl m_releaseUrl;
    QString m_currentVersion;
    QString m_cachePath;
    bool m_fixture = false;
    bool m_endpointValid = false;
    bool m_metadataRetried = false;
    quint64 m_ticket = 0;
    QString m_state = QStringLiteral("idle");
    QString m_error;
    qreal m_progress = 0;
    QString m_availableVersion;
    QString m_assetName;
    QUrl m_downloadUrl;
    QByteArray m_expectedSha256;
    qint64 m_expectedSize = 0;
    qint64 m_received = 0;
    int m_redirects = 0;
    QByteArray m_metadata;
    std::unique_ptr<QTemporaryFile> m_file;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QString m_downloadedPath;
    std::unique_ptr<QTemporaryDir> m_staging;
    QString m_installPhase;
    QByteArray m_listing;
    qint64 m_processOutputBytes = 0;
};

} // namespace workspace
