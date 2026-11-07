#include "native-app.h"
#include "native-preferences.h"
#include "api-client.h"
#include "file-client.h"
#include "native-cache-service.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLocale>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <QProcess>
#include <QTextStream>
#include <algorithm>

using namespace workspace;

namespace {
QStringList catalogArguments(const QString &text)
{
    const QRegularExpression placeholder("%L?[0-9]+|%n");
    QStringList result;
    auto matches = placeholder.globalMatch(text);
    while (matches.hasNext()) result.append(matches.next().captured());
    std::sort(result.begin(), result.end());
    return result;
}

QStringList catalogTranslationForms(QXmlStreamReader &xml, bool numerus)
{
    if (!numerus) return {xml.readElementText()};
    QStringList forms;
    while (xml.readNextStartElement()) {
        if (xml.name() != "numerusform") {
            xml.raiseError("Unexpected child in plural translation");
            return {};
        }
        forms.append(xml.readElementText());
    }
    return forms;
}

bool catalogFormPreservesArguments(const QString &source, const QString &translation)
{
    return !translation.isEmpty() && catalogArguments(source) == catalogArguments(translation);
}
} // namespace

class NativeLocaleTest final : public QObject {
    Q_OBJECT
    const QLocale originalLocale;
private slots:
    void cleanup() { QLocale::setDefault(originalLocale); }

    void deployedMergedQtCatalogIsUsedByTheRealLocaleBinding()
    {
        QTemporaryDir directory;
        const auto bin = directory.filePath("bin");
        const auto translations = directory.filePath("translations");
        QVERIFY(QDir().mkpath(bin));
        QVERIFY(QDir().mkpath(translations));
        // Qt deployment merges the Qt modules into qt_ru.qm. There is no
        // qtbase_ru.qm alongside it, unlike the development SDK.
        const auto sdkCatalog = QDir(QLibraryInfo::path(QLibraryInfo::TranslationsPath))
                                    .filePath("qtbase_ru.qm");
        QVERIFY2(QFileInfo::exists(sdkCatalog), "Install the Qt Russian translation catalog for this test");
        QVERIFY(QFile::copy(sdkCatalog, translations + "/qt_ru.qm"));
        const auto executable = bin + '/' + QFileInfo(QCoreApplication::applicationFilePath()).fileName();
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), executable));
        QFile conf(bin + "/qt.conf");
        QVERIFY(conf.open(QIODevice::WriteOnly));
        conf.write("[Paths]\nPrefix=..\nTranslations=translations\n");
        conf.close();
        QProcess probe;
        probe.start(executable, {"--runtime-translation-probe"});
        QVERIFY2(probe.waitForFinished(5000), qPrintable(probe.errorString()));
        const auto output = probe.readAllStandardOutput();
        QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
        QVERIFY2(probe.exitCode() == 0, output.constData());
        const auto result = QJsonDocument::fromJson(output).object();
        QCOMPARE(result.value("qtCancel").toString(), QString::fromUtf8("Отмена"));
        QCOMPARE(result.value("nativeSignIn").toString(), QString::fromUtf8("Войти"));
        QCOMPARE(QFileInfo(result.value("translationsPath").toString()).canonicalFilePath(),
                 QFileInfo(translations).canonicalFilePath());
    }

    void currentPublicSourceMessagesExistInTheReviewedCatalog()
    {
        QTemporaryDir directory;
        const auto generated = directory.filePath("extracted.ts");
        const auto src = QFINDTESTDATA("../src");
        const auto qml = QFINDTESTDATA("../qml");
        const auto catalog = QFINDTESTDATA("../translations/workspace_ru.ts");
        QVERIFY(!src.isEmpty() && !qml.isEmpty() && !catalog.isEmpty());
        QProcess update;
        update.start(QString::fromUtf8(WORKSPACE_NATIVE_LUPDATE_EXECUTABLE),
            {src, qml, "-no-obsolete", "-locations", "none", "-ts", generated});
        QVERIFY2(update.waitForFinished(5000), qPrintable(update.errorString()));
        QCOMPARE(update.exitStatus(), QProcess::NormalExit);
        QCOMPARE(update.exitCode(), 0);
        const auto keys = [](const QString &path) {
            QSet<QString> result;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return result;
            QXmlStreamReader xml(&file);
            QString context;
            while (!xml.atEnd()) {
                xml.readNext();
                if (!xml.isStartElement()) continue;
                if (xml.name() == "name") context = xml.readElementText();
                else if (xml.name() == "source") result.insert(context + '\n' + xml.readElementText());
            }
            return xml.hasError() ? QSet<QString>{} : result;
        };
        const auto extracted = keys(generated), reviewed = keys(catalog);
        QVERIFY(extracted.size() >= 207);
        QStringList missing;
        for (const auto &key : extracted)
            if (!reviewed.contains(key)) missing.append(key);
        std::sort(missing.begin(), missing.end());
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join("\n\n")));
    }

    void allCatalogMessagesAreReviewedAndPreserveArguments()
    {
        const auto path = QFINDTESTDATA("../translations/workspace_ru.ts");
        QVERIFY(!path.isEmpty());
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QXmlStreamReader xml(&file);
        QString context;
        QSet<QString> seen;
        int messages = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            if (xml.name() == "name") context = xml.readElementText();
            if (xml.name() != "message") continue;
            const bool numerus = xml.attributes().value("numerus") == "yes";
            QString source, type;
            QStringList translations;
            while (xml.readNextStartElement()) {
                if (xml.name() == "source") source = xml.readElementText();
                else if (xml.name() == "translation") {
                    type = xml.attributes().value("type").toString();
                    translations = catalogTranslationForms(xml, numerus);
                } else xml.skipCurrentElement();
            }
            const auto key = context + '\n' + source;
            QVERIFY2(!context.isEmpty() && !source.isEmpty(), qPrintable(key));
            QVERIFY2(!seen.contains(key), qPrintable(key));
            seen.insert(key);
            QVERIFY2(type != "unfinished" && !translations.isEmpty(), qPrintable(key));
            for (qsizetype form = 0; form < translations.size(); ++form) {
                const auto detail = key + " [translation form " + QString::number(form + 1) + ']';
                QVERIFY2(catalogFormPreservesArguments(source, translations.at(form)), qPrintable(detail));
            }
            ++messages;
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QVERIFY(messages >= 207);
    }

    void catalogPluralValidatorChecksEveryFormAndArgumentMultiplicity_data()
    {
        QTest::addColumn<bool>("numerus");
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("translationXml");
        QTest::addColumn<bool>("accepted");
        const QString source = "%n event for %1 %1 (%L2)";
        const QString first = "<numerusform>%n событие для %1 %1 (%L2)</numerusform>";
        const QString second = "<numerusform>%n события для %1 %1 (%L2)</numerusform>";
        const QString third = "<numerusform>%n событий для %1 %1 (%L2)</numerusform>";
        const auto plural = [](const QString &forms) { return "<translation>" + forms + "</translation>"; };
        QTest::newRow("valid-three-forms") << true << source << plural(first + second + third) << true;
        QTest::newRow("missing-arg-in-second-form") << true << source
            << plural(first + "<numerusform>%n события для %1 (%L2)</numerusform>" + third) << false;
        QTest::newRow("wrong-arg-in-third-form") << true << source
            << plural(first + second + "<numerusform>%n событий для %1 %1 (%2)</numerusform>") << false;
        QTest::newRow("duplicate-arg-in-third-form") << true << source
            << plural(first + second + "<numerusform>%n событий для %1 %1 %1 (%L2)</numerusform>") << false;
        QTest::newRow("nonplural-preserves-repeated-arg") << false << QString("%1 and %1 (%L2)")
            << QString("<translation>%1 и %1 (%L2)</translation>") << true;
        QTest::newRow("nonplural-extra-arg-rejected") << false << QString("%1 and %1 (%L2)")
            << QString("<translation>%1 и %1 %1 (%L2)</translation>") << false;
    }

    void catalogPluralValidatorChecksEveryFormAndArgumentMultiplicity()
    {
        QFETCH(bool, numerus); QFETCH(QString, source); QFETCH(QString, translationXml); QFETCH(bool, accepted);
        QXmlStreamReader xml(translationXml);
        QVERIFY(xml.readNextStartElement()); QCOMPARE(xml.name(), QStringView(u"translation"));
        const auto forms = catalogTranslationForms(xml, numerus);
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QCOMPARE(forms.size(), numerus ? qsizetype(3) : qsizetype(1));
        const bool valid = !forms.isEmpty() && std::all_of(forms.cbegin(), forms.cend(), [&](const QString &form) {
            return catalogFormPreservesArguments(source, form);
        });
        QCOMPARE(valid, accepted);
    }

    void switchesExistingQmlAndCppMessagesWithoutRestart()
    {
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setLocale("en");
        QQmlEngine engine;
        bindNativeLocale(engine, &preferences);
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQml
            QtObject {
                property string label: qsTranslate("Main", "Sign in")
                property string withArgs: qsTranslate("Main", "Continue as %1 in %2").arg("Alice").arg("Acme")
            }
        )", QUrl("qrc:/locale-test.qml"));
        QScopedPointer<QObject> item(component.create());
        QVERIFY2(item, qPrintable(component.errorString()));
        QCOMPARE(item->property("label").toString(), QString("Sign in"));
        QCOMPARE(item->property("withArgs").toString(), QString("Continue as Alice in Acme"));
        preferences.setLocale("ru");
        QTRY_COMPARE(item->property("label").toString(), QString::fromUtf8("Войти"));
        QCOMPARE(item->property("withArgs").toString(), QString::fromUtf8("Продолжить как Alice в Acme"));
        QCOMPARE(ApiClient::tr("Access denied (HTTP 403)."), QString::fromUtf8("Доступ запрещён (HTTP 403)."));
        QCOMPARE(FileClient::tr("File request failed (HTTP %1).").arg(503),
                 QString::fromUtf8("Ошибка запроса файла (HTTP 503)."));
        QCOMPARE(NativeCacheService::tr("The local message cache is unavailable."),
                 QString::fromUtf8("Локальный кэш сообщений недоступен."));
        QCOMPARE(NativeCacheService::tr("The local cache cleanup queue is busy. Please try again."),
                 QString::fromUtf8("Очередь очистки кэша занята. Повторите попытку."));
        QCOMPARE(QLocale().language(), QLocale::Russian);
        preferences.setLocale("en");
        QTRY_COMPARE(item->property("label").toString(), QString("Sign in"));
        QCOMPARE(ApiClient::tr("Access denied (HTTP 403)."), QString("Access denied (HTTP 403)."));
        QCOMPARE(QLocale().language(), QLocale::English);
    }

    void ownerLanguagesSurviveRestartAndDoNotLeakToOtherOwners()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        const QString ownerA(64, u'a'), ownerB(64, u'b');
        {
            NativePreferences preferences(path);
            preferences.setOwner(ownerA);
            preferences.setLocale("ru");
            preferences.setOwner(ownerB);
            preferences.setLocale("en");
        }
        NativePreferences restored(path);
        restored.setOwner(ownerA);
        QQmlEngine engine;
        bindNativeLocale(engine, &restored);
        QCOMPARE(restored.locale(), QString("ru"));
        QCOMPARE(QCoreApplication::translate("Main", "Sign in"), QString::fromUtf8("Войти"));
        restored.setOwner(ownerB);
        QCOMPARE(restored.locale(), QString("en"));
        QCOMPARE(QCoreApplication::translate("Main", "Sign in"), QString("Sign in"));
        restored.setOwner(ownerA);
        QCOMPARE(QCoreApplication::translate("Main", "Sign in"), QString::fromUtf8("Войти"));
    }

    void rejectsUnsupportedOrMalformedLocaleAndUninstallsOnEngineDestruction()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.ini");
        {
            QSettings settings(path, QSettings::IniFormat);
            settings.setValue("appearance/anonymous/locale", "ru_RU/../unexpected");
        }
        NativePreferences preferences(path);
        const auto expected = QLocale::system().language() == QLocale::Russian ? QString("ru") : QString("en");
        QCOMPARE(preferences.locale(), expected);
        preferences.setLocale("ru");
        QSignalSpy changes(&preferences, &NativePreferences::localeChanged);
        for (const auto *invalid : {"", "fr", "RU", "ru_RU", "ru\n", "../ru"})
            preferences.setLocale(QString::fromLatin1(invalid));
        QCOMPARE(preferences.locale(), QString("ru"));
        QCOMPARE(changes.count(), 0);
        {
            QQmlEngine engine;
            bindNativeLocale(engine, &preferences);
            QCOMPARE(ApiClient::tr("Request timed out."), QString::fromUtf8("Время ожидания запроса истекло."));
        }
        QCOMPARE(ApiClient::tr("Request timed out."), QString("Request timed out."));
    }
};

int main(int argc, char **argv)
{
    if (argc > 1 && QByteArray(argv[1]) == "--runtime-translation-probe") {
        QCoreApplication app(argc, argv);
        QTemporaryDir directory;
        NativePreferences preferences(directory.filePath("preferences.ini"));
        preferences.setLocale("ru");
        QQmlEngine engine;
        bindNativeLocale(engine, &preferences);
        const auto qtCancel = QCoreApplication::translate("QPlatformTheme", "Cancel");
        const auto nativeSignIn = QCoreApplication::translate("Main", "Sign in");
        QTextStream(stdout) << QJsonDocument(QJsonObject{
            {"translationsPath", QLibraryInfo::path(QLibraryInfo::TranslationsPath)},
            {"qtCancel", qtCancel}, {"nativeSignIn", nativeSignIn}})
            .toJson(QJsonDocument::Compact) << Qt::endl;
        return qtCancel == QString::fromUtf8("Отмена") && nativeSignIn == QString::fromUtf8("Войти") ? 0 : 1;
    }
    QGuiApplication app(argc, argv);
    NativeLocaleTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "native-locale-test.moc"
