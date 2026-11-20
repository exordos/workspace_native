#include "support-report.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSysInfo>
#include <QUuid>
#include <QtConcurrentRun>
#include <array>

namespace workspace {
namespace {
    constexpr int MaxReportBytes = 64 * 1024;
    constexpr qint64 MaxExecutableBytes = 128 * 1024 * 1024;

    QString allowed(const QString& value, const QStringList& choices)
    {
        return choices.contains(value) ? value : QStringLiteral("unavailable");
    }

    QString packageChannel()
    {
        const auto path = QDir(QCoreApplication::applicationDirPath()).filePath("../release.json");
        const QFileInfo info(path);
        if (!info.isFile() || info.size() <= 0 || info.size() > 4096)
            return "unavailable";
        QFile metadata(path);
        if (!metadata.open(QIODevice::ReadOnly))
            return "unavailable";
        const auto release = QJsonDocument::fromJson(metadata.read(4097)).object();
        if (release.value("version").toString() == QStringLiteral(WORKSPACE_NATIVE_VERSION)
            && release.value("developmentPreview").toBool())
            return "development-preview";
        return "unavailable";
    }

    QJsonObject buildIdentity(const std::atomic_bool& cancelled)
    {
        QJsonObject build { { "version", QStringLiteral(WORKSPACE_NATIVE_VERSION) },
            { "packageChannel", packageChannel() }, { "executableSha256", QJsonValue::Null }, { "fingerprintStatus", "unavailable" } };
        QFile file(QCoreApplication::applicationFilePath());
        if (!file.open(QIODevice::ReadOnly))
            return build;
        const auto size = file.size();
        if (size <= 0 || size > MaxExecutableBytes) {
            build["fingerprintStatus"] = "size-limit";
            return build;
        }
        QCryptographicHash digest(QCryptographicHash::Sha256);
        QElapsedTimer clock;
        clock.start();
        std::array<char, 64 * 1024> buffer;
        qint64 consumed = 0;
        while (consumed < size) {
            if (cancelled.load() || clock.elapsed() >= 2000) {
                build["fingerprintStatus"] = "interrupted";
                return build;
            }
            const auto read = file.read(buffer.data(), qMin<qint64>(buffer.size(), size - consumed));
            if (read <= 0)
                return build;
            digest.addData(QByteArrayView(buffer.data(), read));
            consumed += read;
        }
        if (file.size() != consumed)
            return build;
        build["fingerprintStatus"] = "available";
        build["executableSha256"] = QString::fromLatin1(digest.result().toHex());
        return build;
    }

    QByteArray collect(const QString& display, const QString& language, const QString& theme,
        const QJsonObject& measurements, const std::atomic_bool& cancelled)
    {
        const auto now = QDateTime::currentDateTime();
        const QJsonObject environment {
            { "os", allowed(QSysInfo::kernelType(), { "linux", "darwin", "winnt" }) },
            { "cpu", allowed(QSysInfo::currentCpuArchitecture(), { "arm", "arm64", "x86_64", "i386", "riscv64" }) },
            { "qt", QString::fromLatin1(qVersion()) },
            { "display", allowed(display, { "xcb", "wayland", "cocoa", "windows", "offscreen", "minimal", "headless" }) },
            { "language", allowed(language, { "en", "ru" }) },
            { "theme", allowed(theme, { "system", "light", "dark" }) }
        };
        QJsonObject report { { "schemaVersion", 1 }, { "reportId", QUuid::createUuid().toString(QUuid::WithoutBraces) },
            { "collectedAt", now.toUTC().toString(Qt::ISODateWithMs) },
            { "utcOffsetSeconds", now.offsetFromUtc() }, { "build", buildIdentity(cancelled) },
            { "environment", environment }, { "eventCounts", measurements.value("levels").isObject() ? measurements.value("levels") : QJsonValue(QJsonValue::Null) },
            { "apiLatency", measurements.value("latency").isObject() ? measurements.value("latency") : QJsonValue(QJsonValue::Null) } };
        const auto bytes = QJsonDocument(report).toJson();
        return !cancelled.load() && bytes.size() <= MaxReportBytes ? bytes : QByteArray { };
    }
}

SupportReport::SupportReport(QObject* parent)
    : QObject(parent)
{
}
SupportReport::~SupportReport()
{
    if (m_pending)
        m_pending->store(true);
}

QByteArray SupportReport::basicReport()
{
    return collect("headless", { }, { }, { }, std::atomic_bool(false));
}

void SupportReport::prepare(const QString& display, const QString& language, const QString& theme,
    const QVariantMap& sourceCounts)
{
    cancel();
    QJsonObject measurements;
    if (!sourceCounts.isEmpty()) {
        const auto levels = sourceCounts.value("levels").toMap();
        QJsonObject counts;
        int remaining = 500;
        for (const auto& level : { "debug", "info", "warn", "error" }) {
            const auto value = QJsonValue::fromVariant(levels.value(level));
            const int count = qBound(0, value.toInt(), remaining);
            counts[level] = count;
            remaining -= count;
        }
        measurements["levels"] = counts;
        const auto number = [&](const char* field) { return QJsonValue::fromVariant(sourceCounts.value(field)).toInt(-1); };
        const int n = qBound(0, number("requests"), 200);
        const int median = number("medianMs"), p95 = number("p95Ms");
        QJsonObject latency { { "sampleCount", n }, { "medianMs", QJsonValue::Null }, { "p95Ms", QJsonValue::Null } };
        if (n && median >= 0 && p95 >= median) {
            latency["medianMs"] = median;
            latency["p95Ms"] = p95;
        }
        measurements["latency"] = latency;
    }
    const auto pending = std::make_shared<std::atomic_bool>(false);
    m_pending = pending;
    auto* watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, pending] {
        watcher->deleteLater();
        if (pending != m_pending)
            return;
        m_preview = watcher->result();
        m_pending.reset();
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run([=] { return collect(display, language, theme, measurements, *pending); }));
    emit changed();
}

void SupportReport::cancel()
{
    if (!m_pending && m_preview.isEmpty())
        return;
    if (m_pending)
        m_pending->store(true);
    m_pending.reset();
    m_preview.clear();
    emit changed();
}

bool SupportReport::save(const QUrl& destination) const
{
    if (m_preview.isEmpty() || !destination.isLocalFile() || !destination.host().isEmpty())
        return false;
    QSaveFile file(destination.toLocalFile());
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return false;
    return file.write(m_preview) == m_preview.size() && file.commit();
}
}
