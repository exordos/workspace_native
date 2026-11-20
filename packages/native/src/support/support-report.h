#pragma once

#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <atomic>
#include <memory>

namespace workspace {

class SupportReport final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString preview READ preview NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
public:
    explicit SupportReport(QObject* parent = nullptr);
    ~SupportReport() override;
    QString preview() const { return QString::fromUtf8(m_preview); }
    bool busy() const { return bool(m_pending); }

    Q_INVOKABLE void prepare(const QString& display, const QString& language, const QString& theme,
        const QVariantMap& sourceCounts);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool save(const QUrl& destination) const;

    static QByteArray basicReport();
signals:
    void changed();

private:
    QByteArray m_preview;
    std::shared_ptr<std::atomic_bool> m_pending;
};

}
