// #879 (part of #851): the mock mail adapter + mailbox page slice. Proves the
// folder list with unread counts, per-folder message lists, row selection
// driving the reader placeholder, and the loading / empty / error+retry states
// against the in-process mock server. Live mail integration is #852.
#include <QtTest>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QGuiApplication>
#include <QHash>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTranslator>
#include <QUrlQuery>
#include <QVariant>

#include "api-client.h"
#include "mail-store.h"
#include "mock-server.h"

#ifndef WORKSPACE_NATIVE_MAIL_QML_SOURCE_DIR
#define WORKSPACE_NATIVE_MAIL_QML_SOURCE_DIR "qml"
#endif

using namespace workspace;
using namespace workspace::test;

namespace {

QJsonArray fixtureFolders()
{
    auto row = [](const char *id, const char *title, int unread) {
        return QJsonObject{{"id", id}, {"title", title}, {"unread", unread}};
    };
    return {row("inbox", "Inbox", 12), row("sent", "Sent", 0), row("drafts", "Drafts", 3),
            row("archive", "Archive", 0), row("spam", "Spam", 1), row("trash", "Trash", 0)};
}

QJsonArray fixtureMessages(const QString &folder)
{
    if (folder == "inbox") {
        auto row = [](const char *uuid, const char *sender, const char *subject,
                      const char *snippet, const char *time) {
            QJsonObject message{{"uuid", uuid}, {"sender", sender}, {"subject", subject},
                                {"snippet", snippet}, {"time", time}, {"unread", true}};
            if (message.value("uuid") == "mail-inbox-1") {
                message["subject"] = "A deliberately long subject for our upcoming workspace planning meeting";
                message["recipients"] = QJsonArray{"design-team@example.test", "workspace-team@example.test",
                                                   "product-team@example.test"};
                message["date"] = "13 October 2026, 10:24";
                message["body"] = "Hello team,\n\n<Review the fixtures> before Friday.";
            }
            if (message.value("uuid") == "mail-inbox-3")
                message["body"] = QStringLiteral("");
            return message;
        };
        return {row("mail-inbox-1", "Build Bot", "Release 0.5.0 checks passed",
                    "All pipelines are green and artifacts are ready.", "10:24"),
                row("mail-inbox-2", "Product Team", "Demo agenda for Friday",
                    "Please review talking points before 15:00.", "09:02"),
                row("mail-inbox-3", "Design", "Light theme polish proposal",
                    "Attached updated spacing and color recommendations.", "Mon")};
    }
    if (folder == "sent") {
        return {QJsonObject{{"uuid", "mail-sent-1"}, {"sender", "me"},
                             {"subject", "Re: demo slot"}, {"snippet", "Works for me."},
                             {"time", "Tue"}, {"unread", false}}};
    }
    return {};
}

QVariantMap lightColors()
{
    return {{"dark", false}, {"bg", "#f4f7fc"}, {"bg-elevated", "#ffffff"},
            {"border-subtle", "#d9e2f0"}, {"text-primary", "#162033"}, {"text-muted", "#64738d"},
            {"accent", "#4a73ff"}, {"accent-soft", "#eaf0ff"}, {"card-bg-active", "#f4f7ff"},
            {"on-accent", "#ffffff"}, {"card-bg", "#ffffff"}};
}

} // namespace

class NativeMailTest : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        server.handler = [this](const Request &request) { return respond(request); };
        api.setServer(server.origin());
        api.setAccessToken("test-token");
        store = new MailStore(&api, this);
    }
    void cleanup()
    {
        store->deleteLater();
        brokenFoldersOnce = false;
        slowInboxOnce = false;
        missingMailEndpoints = false;
        missingMessagesOnly = false;
    }

    // exordy finding 4712325987: a real deployment has no /mail endpoints yet
    // (#852 owns live integration), so opening Mail must show the sample
    // mailbox compiled into the client instead of a dead error screen.
    void missingEndpointsServeSampleMailbox()
    {
        missingMailEndpoints = true;
        store->refresh();
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("ready"), 5000);
        QCOMPARE(store->folders().size(), 6);
        QCOMPARE(store->messages().size(), 3);
        store->selectFolder(QStringLiteral("sent"));
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("ready"), 5000);
        QCOMPARE(store->messages().size(), 1);
    }

    // exordy finding 4712650373: sample mode is decided by the FOLDERS
    // response. When folders loaded from the server (200), a 404 on the
    // messages endpoint must stay an error - never sample rows listed under
    // server-provided folders (that would fabricate mailbox content).
    void serverFoldersNeverMixWithSampleMessages()
    {
        missingMessagesOnly = true;
        store->refresh();
        // Folders serve; the messages 404 lands straight after - the store
        // must reach the error state with an empty list, not sample rows.
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("error"), 5000);
        QVERIFY(store->messages().isEmpty());
        QCOMPARE(store->folders().size(), 6);
        // Folder switches keep honouring the same rule.
        store->selectFolder(QStringLiteral("sent"));
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("error"), 5000);
        QVERIFY(store->messages().isEmpty());
    }

    void foldersLoadWithFixtureCounts()
    {
        store->refresh();
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("ready"), 5000);
        QStringList ids;
        QStringList titles;
        QList<int> unread;
        const auto folders = store->folders();
        for (const auto &value : folders) {
            const auto row = value.toMap();
            ids << row.value(QStringLiteral("id")).toString();
            titles << row.value(QStringLiteral("title")).toString();
            unread << row.value(QStringLiteral("unread")).toInt();
        }
        const QStringList expectedIds{"inbox", "sent", "drafts", "archive", "spam", "trash"};
        const QStringList expectedTitles{"Inbox", "Sent", "Drafts", "Archive", "Spam", "Trash"};
        const QList<int> expectedUnread{12, 0, 3, 0, 1, 0};
        QCOMPARE(ids, expectedIds);
        QCOMPARE(titles, expectedTitles);
        QCOMPARE(unread, expectedUnread);
    }

    void selectingFolderLoadsItsMessages()
    {
        store->refresh();
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("ready"), 5000);
        QCOMPARE(store->messages().size(), 3);
        store->selectFolder(QStringLiteral("sent"));
        QTRY_COMPARE_WITH_TIMEOUT(store->messages().size(), 1, 5000);
        QCOMPARE(store->state(), QStringLiteral("ready"));
        QCOMPARE(store->folderId(), QStringLiteral("sent"));
        const auto row = store->messages().first().toMap();
        QCOMPARE(row.value(QStringLiteral("subject")).toString(), QStringLiteral("Re: demo slot"));
        bool sawSentQuery = false;
        for (const auto &request : server.requests) {
            if (request.target.path() == "/mail/messages"
                && QUrlQuery(request.target.query()).queryItemValue("folder") == "sent")
                sawSentQuery = true;
        }
        QVERIFY2(sawSentQuery, "folder switch must re-fetch the messages endpoint");
    }

    void mailboxRetiresWithItsSession()
    {
        store->refresh();
        QTRY_COMPARE(store->state(), QStringLiteral("ready"));
        store->selectMessage(QStringLiteral("mail-inbox-1"));
        api.setAccessToken("renewed-test-token", true);
        QCOMPARE(store->messages().size(), 3);
        QCOMPARE(store->selectedMessageUuid(), QStringLiteral("mail-inbox-1"));
        api.clearSession();
        QCOMPARE(store->state(), QStringLiteral("idle"));
        QVERIFY(store->messages().isEmpty());
        QVERIFY(store->folders().isEmpty());
        QVERIFY(store->selectedMessageUuid().isEmpty());

        api.setAccessToken("second-test-token");
        slowInboxOnce = true;
        store->refresh();
        QTRY_VERIFY(!slowInboxOnce);
        api.clearSession();
        QCOMPARE(store->state(), QStringLiteral("idle"));
        QVERIFY(store->folders().isEmpty());
        // Process the retired transport callback before checking the new state.
        QCoreApplication::processEvents();
        QCOMPARE(store->state(), QStringLiteral("idle"));
        QVERIFY(store->messages().isEmpty());
    }

    void emptyFolderState()
    {
        store->refresh();
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("ready"), 5000);
        store->selectFolder(QStringLiteral("archive"));
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("ready"), 5000);
        QVERIFY(store->messages().isEmpty());
    }

    void errorStateAndRetryRecovers()
    {
        brokenFoldersOnce = true;
        store->refresh();
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("error"), 5000);
        // The retry control re-issues the whole load; the second attempt serves.
        store->refresh();
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("ready"), 5000);
        QCOMPARE(store->folders().size(), 6);
    }

    void staleFolderReplyCannotWin()
    {
        store->refresh();
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("ready"), 5000);
        // Slow inbox + fast sent: kick off the slow inbox reload, immediately
        // switch to sent; the stale inbox reply must not overwrite sent rows.
        slowInboxOnce = true;
        store->selectFolder(QStringLiteral("inbox"));
        store->selectFolder(QStringLiteral("sent"));
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("ready"), 5000);
        QTest::qWait(700); // let the delayed stale reply land and be dropped
        QCOMPARE(store->folderId(), QStringLiteral("sent"));
        QCOMPARE(store->messages().size(), 1);
        QCOMPARE(store->messages().first().toMap().value(QStringLiteral("uuid")).toString(),
                 QStringLiteral("mail-sent-1"));
    }

    void qmlPageRendersFoldersRowsAndReader()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(readFile("NativeMailPage.qml"),
                          QUrl::fromLocalFile(QStringLiteral(
                              WORKSPACE_NATIVE_MAIL_QML_SOURCE_DIR "/NativeMailPage.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const QVariantMap initial {
            {QStringLiteral("colors"), lightColors()},
            {QStringLiteral("mail"), QVariant::fromValue(store)}};
        QScopedPointer<QObject> object(component.createWithInitialProperties(initial));
        QVERIFY(object);
        auto *page = qobject_cast<QQuickItem *>(object.data());
        QVERIFY(page);
        QQuickWindow window;
        window.resize(900, 700);
        page->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        store->refresh();
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("ready"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(item(page, QStringLiteral("mailFolderRow-inbox")), 5000);
        QVERIFY(textOf(page, "mailFolderTitle-inbox").contains("Inbox"));
        QCOMPARE(textOf(page, "mailFolderUnread-inbox"), QStringLiteral("12"));
        QTRY_VERIFY_WITH_TIMEOUT(item(page, QStringLiteral("mailMessageRow-mail-inbox-1")), 5000);
        QCOMPARE(textOf(page, "mailReaderTitle"), QStringLiteral("Select a message"));
        store->selectMessage(QStringLiteral("mail-inbox-2"));
        QTRY_COMPARE(textOf(page, "mailReaderTitle"),
                     QStringLiteral("Demo agenda for Friday"));
        // Error state: the retry control is present and wired to refresh().
        brokenFoldersOnce = true;
        store->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(visible(page, "mailErrorState"), 5000);
        QVERIFY(!visible(page, "mailLoadingState"));
        store->refresh();
        QTRY_VERIFY_WITH_TIMEOUT(!visible(page, "mailErrorState"), 5000);
        // Empty folder: the empty-state label shows for a ready-but-empty list.
        store->selectFolder(QStringLiteral("archive"));
        QTRY_VERIFY_WITH_TIMEOUT(visible(page, "mailEmptyState"), 5000);
    }

    void selectedMessageOpensReader_data()
    {
        QTest::addColumn<bool>("russian");
        QTest::addColumn<bool>("dark");
        QTest::newRow("en-light") << false << false;
        QTest::newRow("en-dark") << false << true;
        QTest::newRow("ru-light") << true << false;
        QTest::newRow("ru-dark") << true << true;
    }

    void selectedMessageOpensReader()
    {
        QFETCH(bool, russian);
        QFETCH(bool, dark);
        QTranslator translation;
        if (russian) {
            QVERIFY(translation.load(":/workspace/i18n/workspace_ru.qm"));
            QVERIFY(QCoreApplication::installTranslator(&translation));
        }
        QQmlEngine engine;
        QQmlComponent palette(&engine, QUrl::fromLocalFile(QStringLiteral(
            WORKSPACE_NATIVE_MAIL_QML_SOURCE_DIR "/NativeTheme.qml")));
        QScopedPointer<QObject> theme(palette.createWithInitialProperties(
            {{"paletteId", QStringLiteral("orange-warm")}, {"dark", dark}}));
        QVERIFY2(theme, qPrintable(palette.errorString()));
        auto colors = theme->property("colors").value<QJSValue>().toVariant().toMap();
        QVERIFY(colors.contains("bg"));
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(
            WORKSPACE_NATIVE_MAIL_QML_SOURCE_DIR "/NativeMailPage.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const QVariantMap properties{{"colors", colors}, {"dark", dark}, {"mail", QVariant::fromValue(store)}};
        QScopedPointer<QObject> object(component.createWithInitialProperties(properties));
        auto *page = qobject_cast<QQuickItem *>(object.data());
        QVERIFY(page);
        QQuickWindow window;
        window.setColor(colors.value("bg").value<QColor>());
        window.resize(900, 700);
        page->setSize(window.size());
        page->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        store->refresh();
        QTRY_COMPARE(store->state(), QStringLiteral("ready"));
        QCOMPARE(textOf(page, "mailReaderTitle"),
                 russian ? QStringLiteral("Выберите сообщение") : QStringLiteral("Select a message"));
        QVERIFY(!visible(page, "mailReaderActionNotice"));
        auto *row = item(page, "mailMessageRow-mail-inbox-1");
        QVERIFY(row);
        QTRY_VERIFY(row->width() > 0 && row->height() > 0);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                          row->mapToScene(row->boundingRect().center()).toPoint());
        QTRY_COMPARE(store->selectedMessageUuid(), QStringLiteral("mail-inbox-1"));
        const auto message = fixtureMessages("inbox").first().toObject();
        QCOMPARE(textOf(page, "mailReaderTitle"), message.value("subject").toString());
        QCOMPARE(textOf(page, "mailReaderFrom"), russian ? QStringLiteral("От: Build Bot") : QStringLiteral("From: Build Bot"));
        QCOMPARE(textOf(page, "mailReaderDate"),
                 (russian ? QStringLiteral("Дата: ") : QStringLiteral("Date: ")) + message.value("date").toString());
        QCOMPARE(textOf(page, "mailReaderTo"),
                 (russian ? QStringLiteral("Кому: ") : QStringLiteral("To: "))
                 + message.value("recipients").toVariant().toStringList().join(", "));
        QCOMPARE(textOf(page, "mailReaderBody"), message.value("body").toString());
        QCOMPARE(item(page, "mailReaderBody")->property("textFormat").toInt(), int(Qt::PlainText));
        QTRY_VERIFY(item(page, "mailReaderTitle")->property("lineCount").toInt() > 1);
        QVERIFY(item(page, "mailReaderTo")->property("lineCount").toInt() > 1);
        QCOMPARE(textOf(page, "mailReaderActionNotice"),
                 russian ? QStringLiteral("Действия с письмами недоступны в режиме предпросмотра.")
                         : QStringLiteral("Mail actions are unavailable in this preview."));
        const auto requests = server.requests.size();
        for (const auto &key : {"reply", "forward", "mark", "delete"}) {
            auto *action = item(page, QStringLiteral("mailReaderAction-") + key);
            QVERIFY(action && action->isVisible());
            QVERIFY(!action->isEnabled());
            const QPoint position = action->mapToScene(action->boundingRect().center()).toPoint();
            QVERIFY(QRect(QPoint(), window.size()).contains(position));
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, position);
        }
        QCOMPARE(store->selectedMessageUuid(), QStringLiteral("mail-inbox-1"));
        QCOMPARE(server.requests.size(), requests);
        store->selectMessage("mail-inbox-2");
        QCOMPARE(textOf(page, "mailReaderBody"),
                 russian ? QStringLiteral("Текст письма недоступен") : QStringLiteral("Message body unavailable"));
        store->selectMessage("mail-inbox-3");
        QVERIFY(visible(page, "mailReaderBody"));
        QCOMPARE(textOf(page, "mailReaderBody"), QStringLiteral(""));
        store->selectFolder("archive");
        QTRY_COMPARE(store->state(), QStringLiteral("ready"));
        QVERIFY(!visible(page, "mailReaderBody"));
        QVERIFY(!visible(page, "mailReaderAction-reply"));
    }

    // exordy finding 4714792891: the reader wrapper's title and body Text
    // items must keep their fill-width hints. Without them both retain their
    // implicit widths, so a long subject (and the body note) overflows the
    // constrained reader column and elide/wrap never engage. Measured at a
    // narrow page width so the reader column is genuinely constrained.
    void readerTextStaysWithinColumn()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(readFile("NativeMailPage.qml"),
                          QUrl::fromLocalFile(QStringLiteral(
                              WORKSPACE_NATIVE_MAIL_QML_SOURCE_DIR "/NativeMailPage.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const QVariantMap initial {
            {QStringLiteral("colors"), lightColors()},
            {QStringLiteral("mail"), QVariant::fromValue(store)}};
        QScopedPointer<QObject> object(component.createWithInitialProperties(initial));
        QVERIFY(object);
        auto *page = qobject_cast<QQuickItem *>(object.data());
        QVERIFY(page);
        QQuickWindow window;
        window.resize(480, 700);
        page->setParentItem(window.contentItem());
        page->setWidth(480);
        page->setHeight(700);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        store->refresh();
        QTRY_COMPARE_WITH_TIMEOUT(store->state(), QStringLiteral("ready"), 5000);
        store->selectMessage(QStringLiteral("mail-inbox-2"));
        QTRY_COMPARE(textOf(page, "mailReaderTitle"),
                     QStringLiteral("Demo agenda for Friday"));
        auto *title = item(page, QStringLiteral("mailReaderTitle"));
        auto *body = item(page, QStringLiteral("mailReaderBody"));
        QVERIFY(title);
        QVERIFY(body);
        auto *column = title->parentItem();
        QVERIFY(column);
        QVERIFY2(column->width() > 0 && column->width() < 200,
                 qPrintable(QStringLiteral("reader column unexpectedly %1 px wide")
                                .arg(column->width())));
        QVERIFY2(title->width() <= column->width(),
                 qPrintable(QStringLiteral("reader title %1 px overflows its %2 px column")
                                .arg(title->width()).arg(column->width())));
        QVERIFY2(body->width() <= column->width(),
                 qPrintable(QStringLiteral("reader body %1 px overflows its %2 px column")
                                .arg(body->width()).arg(column->width())));
        // The fill hint genuinely engages: the painted width tracks the
        // column (not the implicit width), so elide/wrap operate on the
        // constrained layout.
        const qreal painted = title->width();
        QVERIFY2(qAbs(painted - column->width()) <= 1.0,
                 qPrintable(QStringLiteral("title width %1 not at column %2")
                                .arg(painted).arg(column->width())));
    }

private:
    Response respond(const Request &request)
    {
        Response response;
        const QString path = request.target.path();
        const QString folder = QUrlQuery(request.target.query()).queryItemValue("folder");
        if (missingMailEndpoints
            && (path == "/mail/folders" || path == "/mail/messages")) {
            response.status = 404;
            return response;
        }
        if (missingMessagesOnly && path == "/mail/messages") {
            response.status = 404;
            return response;
        }
        if (path == "/mail/folders") {
            if (brokenFoldersOnce) {
                brokenFoldersOnce = false;
                response.status = 500;
                response.data = QJsonObject{{"error", "mailbox unavailable"}};
                return response;
            }
            response.data = QJsonObject{{"folders", fixtureFolders()}};
            return response;
        }
        if (path == "/mail/messages") {
            if (slowInboxOnce && folder == "inbox") {
                slowInboxOnce = false;
                response.delay = 400;
            }
            response.data = QJsonObject{{"messages", fixtureMessages(folder)}};
            return response;
        }
        response.status = 404;
        return response;
    }

    static QByteArray readFile(const QString &name)
    {
        QFile file(QStringLiteral(WORKSPACE_NATIVE_MAIL_QML_SOURCE_DIR "/") + name);
        if (!file.open(QIODevice::ReadOnly))
            qFatal("missing %s", qPrintable(name));
        return file.readAll();
    }

    QQuickItem *item(QQuickItem *root, const QString &name)
    {
        // Visual-tree search (the native-window-test convention): Repeater
        // delegates keep their creation context as QObject parent, so a
        // findChild object walk never sees them.
        if (root->objectName() == name)
            return root;
        for (auto *child : root->childItems())
            if (auto *found = item(child, name))
                return found;
        return nullptr;
    }

    bool visible(QQuickItem *root, const QString &name)
    {
        auto *found = item(root, name);
        return found && found->isVisible();
    }

    QString textOf(QQuickItem *root, const QString &name)
    {
        auto *found = item(root, name);
        return found ? found->property("text").toString() : QString();
    }

    MockServer server;
    ApiClient api{nullptr, true};
    MailStore *store = nullptr;
    bool brokenFoldersOnce = false;
    bool slowInboxOnce = false;
    bool missingMailEndpoints = false;
    bool missingMessagesOnly = false;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    NativeMailTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "native-mail-test.moc"
