#include <QColor>
#include <QFile>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QTest>

static QString readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
}

static QColor sourceColor(const QString &value)
{
    static const QRegularExpression rgba("^rgba\\(([0-9]+),\\s*([0-9]+),\\s*([0-9]+),\\s*([0-9.]+)\\)$");
    const auto match = rgba.match(value);
    if (!match.hasMatch())
        return QColor(value);
    auto result = QColor(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt());
    result.setAlphaF(match.captured(4).toFloat());
    return result;
}

class NativeDesignTest final : public QObject {
    Q_OBJECT
private slots:
    void palettesMatchExistingDesktopSource()
    {
        QQmlEngine engine;
        const auto nativePath = QFINDTESTDATA("../qml/NativeTheme.qml");
        QVERIFY(!nativePath.isEmpty());
        QQmlComponent component(&engine, QUrl::fromLocalFile(nativePath));
        QScopedPointer<QObject> theme(component.create());
        QVERIFY2(theme, qPrintable(component.errorString()));
        const auto palettes = theme->property("palettes").value<QJSValue>();
        for (const auto *id : {"orange-warm", "blue-cold", "blue-mist", "emerald-chat"}) {
            const auto sourcePath = QFINDTESTDATA(qPrintable(QString("../../web/src/shared/lib/themes/%1.ts").arg(id)));
            const auto source = readFile(sourcePath);
            QVERIFY2(!source.isEmpty(), id);
            const auto native = palettes.property(QString::fromLatin1(id));
            QVERIFY(native.isObject());
            for (const auto *mode : {"light", "dark"}) {
                const QRegularExpression section(QString::fromLatin1(mode) + ":\\s*\\{([\\s\\S]*?)\\n\\s*\\}");
                const auto match = section.match(source);
                QVERIFY2(match.hasMatch(), mode);
                const QRegularExpression token("(?:\"([^\"]+)\"|([a-zA-Z0-9_-]+))\\s*:\\s*\"([^\"]+)\"");
                auto tokens = token.globalMatch(match.captured(1));
                const auto nativeMode = native.property(QString::fromLatin1(mode));
                int count = 0;
                while (tokens.hasNext()) {
                    const auto entry = tokens.next();
                    const auto key = entry.captured(1).isEmpty() ? entry.captured(2) : entry.captured(1);
                    const auto expected = sourceColor(entry.captured(3));
                    const auto raw = nativeMode.property(key).toVariant();
                    const auto actual = raw.canConvert<QColor>() ? raw.value<QColor>() : QColor(raw.toString());
                    const auto location = QString("%1.%2.%3").arg(id, mode, key);
                    QVERIFY2(expected.isValid() && actual.isValid(), qPrintable(location));
                    QVERIFY2(actual == expected, qPrintable(location + ": " + actual.name(QColor::HexArgb)
                                                           + " != " + expected.name(QColor::HexArgb)));
                    ++count;
                }
                QVERIFY(count >= 44);
                QCOMPARE(nativeMode.toVariant().toMap().size(), count);
            }
        }
    }
};

QTEST_MAIN(NativeDesignTest)
#include "native-design-test.moc"
