#pragma once

#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QVariantList>
#include <QWindow>

class QKeyEvent;

namespace workspace {

class NativeHotkeys final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList actions READ actions NOTIFY changed)
    Q_PROPERTY(QVariantList fixed READ fixed CONSTANT)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(bool recording READ recording WRITE setRecording NOTIFY recordingChanged)
public:
    explicit NativeHotkeys(QSettings& settings, QObject* parent = nullptr);
    void load(const QString& group);
    void attach(QWindow* window);
    QVariantList actions() const;
    QVariantList fixed() const;
    bool enabled() const
    {
        return m_enabled;
    }
    bool recording() const
    {
        return m_recording;
    }
    void setRecording(bool value);
    Q_INVOKABLE QString validate(const QString& id, const QString& binding) const;
    Q_INVOKABLE QString assign(const QString& id, const QString& binding);
    Q_INVOKABLE QString reset(const QString& id);
    Q_INVOKABLE QString resetAll();
    Q_INVOKABLE QString setEnabled(bool enabled);
    Q_INVOKABLE QString binding(const QString& id) const;
    static QString fromEvent(const QKeyEvent& event);
signals:
    void changed();
    void recordingChanged();
    void recorded(const QString& binding);
    void recordingCancelled();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QString write(const QMap<QString, QString>& overrides, bool enabled);
    QSettings& m_settings;
    QString m_group;
    QMap<QString, QString> m_overrides;
    QHash<QString, QString> m_actionsByChord;
    void rebuildBindings();
    QPointer<QWindow> m_window;
    bool m_enabled = true;
    bool m_recording = false;
};

} // namespace workspace
