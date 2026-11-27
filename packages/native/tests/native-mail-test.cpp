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
#include <QTemporaryDir>
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
        missingFoldersOnly = false;
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

    void workspaceCollectionsUseFolderUuidsAndFollowBothMarkers()
    {
        const QString inbox = QStringLiteral("87654321-1111-4444-8888-111111111111");
        const QString archive = QStringLiteral("87654321-1111-4444-8888-222222222222");
        const QString first = QStringLiteral("87654321-1111-4444-8888-333333333333");
        const QString second = QStringLiteral("87654321-1111-4444-8888-444444444444");
        QStringList requests;
        bool failFolderRefresh = false;
        server.handler = [&](const Request &request) -> Response {
            const QUrlQuery query(request.target);
            const QString path = request.target.path();
            const QString marker = query.queryItemValue("page_marker");
            const QString folder = query.queryItemValue("folder_uuid");
            requests << path + "|" + folder + "|" + marker;
            if (path == "/api/workspace/v1/mail/folders/") {
                if (failFolderRefresh) return Response{QJsonObject{}, 503};
                Response page{QJsonArray{QJsonObject{{"uuid", marker.isEmpty() ? archive : inbox},
                    {"name", marker.isEmpty() ? "Archive" : "Inbox"}, {"kind", marker.isEmpty() ? "archive" : "inbox"},
                    {"unread_count", marker.isEmpty() ? 0 : 2}}}};
                if (marker.isEmpty()) page.headers["X-Pagination-Marker"] = "next-folders";
                return page;
            }
            if (path == "/api/workspace/v1/mail/messages/" && folder == archive)
                return Response{QJsonArray{}};
            if (path == "/api/workspace/v1/mail/messages/" && folder == inbox) {
                const QJsonObject html{{"kind","html"},{"content","<p>Hello <b>team</b></p>"}};
                const QJsonObject text{{"kind","text"},{"content","Hello as text"}};
                auto parts = marker.isEmpty() ? QJsonArray{html, text} : QJsonArray{html};
                Response page{QJsonArray{QJsonObject{{"uuid", marker.isEmpty() ? first : second},
                    {"folder_uuid",inbox},{"from_address","robot@example.test"},
                    {"to_addresses",QJsonArray{"team@example.test"}},{"subject","Updates"}, {"read",true},
                    {"created_at","2026-11-27T12:30:00Z"},
                    {"attachment_urns",QJsonArray{"urn:file:" + first, "unrecognized"}},
                    {"payload",QJsonObject{{"kind","multipart"},{"parts",parts}}}}}};
                if (marker.isEmpty()) page.headers["X-Pagination-Marker"] = "next-messages";
                return page;
            }
            return Response{QJsonObject{}, 404};
        };
        store->refresh();
        QTRY_COMPARE(store->state(), QStringLiteral("ready"));
        QCOMPARE(store->folderId(), inbox);
        QCOMPARE(store->folders().size(), 2);
        QVERIFY(!store->hasSampleFolders() && !store->hasSampleMessages());
        QCOMPARE(store->folders().last().toMap().value("unread").toInt(), 2);
        QCOMPARE(store->messages().size(), 2);
        const auto row = store->messages().first().toMap();
        QCOMPARE(row.value("body").toString(), QStringLiteral("Hello as text"));
        QCOMPARE(row.value("sender").toString(), QStringLiteral("robot@example.test"));
        QCOMPARE(row.value("recipients").toStringList(), QStringList{"team@example.test"});
        QVERIFY(!row.value("unread").toBool());
        QCOMPARE(store->messages().last().toMap().value("body").toString(), QStringLiteral("Hello team"));
        QQmlEngine engine;
        QQuickWindow window;
        window.resize(1024, 768);
        auto *page = showMailWindow(engine, window, false);
        QVERIFY(page);
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        store->selectMessage(first);
        QTRY_VERIFY(visible(page, "mailReaderAttachment-0"));
        QCOMPARE(item(page, "mailReaderAttachment-0")->property("text").toString(),
                 QStringLiteral("Attachment: urn:file:") + first);
        QVERIFY(!item(page, "mailReaderAttachment-1"));
        store->selectFolder(archive);
        QTRY_COMPARE(store->state(), QStringLiteral("ready"));
        QVERIFY(store->messages().isEmpty());
        QCOMPARE(requests.size(), 5);
        QCOMPARE(requests.last(), QStringLiteral("/api/workspace/v1/mail/messages/|") + archive + "|");
        failFolderRefresh = true;
        store->refresh();
        QTRY_COMPARE(store->state(), QStringLiteral("error"));
        QVERIFY(store->folders().isEmpty() && store->messages().isEmpty());
        failFolderRefresh = false;
        store->refresh();
        QTRY_COMPARE(store->state(), QStringLiteral("ready"));
        QCOMPARE(store->folderId(), archive);
        QVERIFY(store->messages().isEmpty());
    }

    void emptyWorkspaceMailboxStaysEmpty()
    {
        QStringList paths;
        server.handler = [&](const Request &request) -> Response {
            paths.append(request.target.path());
            return Response{QJsonArray{}};
        };
        store->refresh();
        QTRY_COMPARE(store->state(), QStringLiteral("ready"));
        QCOMPARE(paths, QStringList{"/api/workspace/v1/mail/folders/"});
        QVERIFY(store->folders().isEmpty() && store->folderId().isEmpty() && store->messages().isEmpty());
        QVERIFY(!store->hasSampleFolders() && !store->hasSampleMessages());
    }

    void workspaceCollectionErrorsDoNotShowSampleMail_data()
    {
        QTest::addColumn<bool>("loop");
        QTest::newRow("forbidden") << false;
        QTest::newRow("repeated-marker") << true;
    }

    void workspaceCollectionErrorsDoNotShowSampleMail()
    {
        QFETCH(bool, loop);
        int hits = 0;
        server.handler = [&](const Request &request) -> Response {
            if (request.target.path() != "/api/workspace/v1/mail/folders/")
                return Response{QJsonObject{}, 404};
            ++hits;
            Response response{QJsonArray{}, loop ? 200 : 403};
            if (loop) response.headers["X-Pagination-Marker"] = "same";
            return response;
        };
        store->refresh();
        QTRY_COMPARE(store->state(), QStringLiteral("error"));
        QCOMPARE(hits, loop ? 2 : 1);
        QVERIFY(!store->hasSampleFolders() && store->folders().isEmpty());
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
        const QLocale previousLocale;
        QLocale::setDefault(QLocale(russian ? "ru_RU" : "en_US"));
        const auto restoreLocale = qScopeGuard([&] { QLocale::setDefault(previousLocale); });
        QTranslator translation;
        if (russian) {
            QVERIFY(translation.load(":/workspace/i18n/workspace_ru.qm"));
            QVERIFY(QCoreApplication::installTranslator(&translation));
        }
        QQmlEngine engine;
        QQuickWindow window;
        window.resize(900, 700);
        auto *page = showMailWindow(engine, window, dark);
        QVERIFY(page);
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

    void composeDraft_data() { selectedMessageOpensReader_data(); }

    void composeDraft()
    {
        QFETCH(bool, russian);
        QFETCH(bool, dark);
        const QLocale previousLocale;
        QLocale::setDefault(QLocale(russian ? "ru_RU" : "en_US"));
        const auto restoreLocale = qScopeGuard([&] { QLocale::setDefault(previousLocale); });
        QTranslator translation;
        if (russian) {
            QVERIFY(translation.load(":/workspace/i18n/workspace_ru.qm"));
            QVERIFY(QCoreApplication::installTranslator(&translation));
        }
        QQmlEngine engine;
        QQuickWindow window;
        window.resize(1024, 768);
        auto *page = showMailWindow(engine, window, dark);
        QVERIFY(page);
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(store->state(), QStringLiteral("idle"));
        const auto requests = server.requests.size();
        auto *compose = item(page, "mailComposeButton");
        QVERIFY(compose);
        auto click = [&window](QQuickItem *control) {
            QSignalSpy rendered(&window, &QQuickWindow::frameSwapped);
            window.update();
            QTRY_VERIFY(!rendered.isEmpty());
            QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                              control->mapToScene(control->boundingRect().center()).toPoint());
        };
        QVERIFY(!page->findChild<QObject *>("mailComposeDialog"));
        click(compose);
        QPointer<QObject> dialog = page->findChild<QObject *>("mailComposeDialog");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        auto *overlay = window.contentItem();
        auto *to = item(overlay, "mailComposeRecipients");
        auto *subject = item(overlay, "mailComposeSubject");
        auto *body = item(overlay, "mailComposeBody");
        auto *send = item(overlay, "mailComposeSend");
        QVERIFY(to && subject && body && send);
        QTRY_VERIFY(to->hasActiveFocus());
        QVERIFY(!visible(overlay, "mailComposeRecipientsError"));
        click(subject);
        QTRY_VERIFY(visible(overlay, "mailComposeRecipientsError"));
        QCOMPARE(textOf(overlay, "mailComposeRecipientsError"),
                 russian ? QStringLiteral("Укажите хотя бы одного получателя.")
                         : QStringLiteral("Enter at least one recipient."));
        QVERIFY(!send->isEnabled());
        to->setProperty("text", "team@example.test");
        subject->setProperty("text", "Workspace review");
        body->setProperty("text", "Please review the attached notes.");
        QVERIFY(!visible(overlay, "mailComposeRecipientsError"));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile attachment(directory.filePath("review.txt"));
        QVERIFY(attachment.open(QIODevice::WriteOnly));
        QCOMPARE(attachment.write("Hello!"), 6);
        attachment.close();
        const QVariant files = QVariant::fromValue(QList<QUrl>{QUrl::fromLocalFile(attachment.fileName())});
        QVERIFY(QMetaObject::invokeMethod(dialog, "addAttachments", Q_ARG(QVariant, files)));
        QTRY_COMPARE(textOf(overlay, "mailComposeAttachmentName-0"), QStringLiteral("review.txt"));
        QCOMPARE(textOf(overlay, "mailComposeAttachmentSize-0"), QLocale().formattedDataSize(6));
        QVERIFY(!send->isEnabled());
        click(send);
        QCOMPARE(server.requests.size(), requests);
        auto *remove = item(overlay, "mailComposeRemove-0");
        QVERIFY(remove);
        click(remove);
        QTRY_VERIFY(!visible(overlay, "mailComposeAttachmentName-0"));
        auto *close = item(overlay, "mailComposeClose");
        QVERIFY(close);
        click(close);
        QTRY_VERIFY(dialog.isNull());
        click(compose);
        dialog = page->findChild<QObject *>("mailComposeDialog");
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("opened").toBool());
        QCOMPARE(textOf(overlay, "mailComposeRecipients"), QString());
        QCOMPARE(textOf(overlay, "mailComposeSubject"), QString());
        QCOMPARE(textOf(overlay, "mailComposeBody"), QString());
        QVERIFY(!visible(overlay, "mailComposeRecipientsError"));
        QVERIFY(!visible(overlay, "mailComposeAttachmentName-0"));
        body = item(overlay, "mailComposeBody");
        QVERIFY(body);
        body->setProperty("text", "Account-local draft");
        api.clearSession();
        QTRY_VERIFY(dialog.isNull());
        QVERIFY(!visible(overlay, "mailComposeBody"));
    }

    // exordy finding 4714792891: the reader wrapper's title and body Text
    // items must keep their fill-width hints. Without them both retain their
    // implicit widths, so a long subject (and the body note) overflows the
    // constrained reader column and elide/wrap never engage. Measured at a
    // narrow page width so the reader column is genuinely constrained.
    void sampleNoticeTracksDisplayedMailbox_data()
    {
        QTest::addColumn<bool>("russian");
        QTest::newRow("en") << false;
        QTest::newRow("ru") << true;
    }

    void sampleNoticeTracksDisplayedMailbox()
    {
        QFETCH(bool, russian);
        QTranslator translation;
        if (russian) {
            QVERIFY(translation.load(":/workspace/i18n/workspace_ru.qm"));
            QVERIFY(QCoreApplication::installTranslator(&translation));
        }
        QQmlEngine engine;
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(
            WORKSPACE_NATIVE_MAIL_QML_SOURCE_DIR "/NativeMailPage.qml")));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        const QVariantMap properties{{"colors", lightColors()}, {"mail", QVariant::fromValue(store)}};
        QScopedPointer<QObject> object(component.createWithInitialProperties(properties));
        auto *page = qobject_cast<QQuickItem *>(object.data());
        QVERIFY(page);
        QQuickWindow window;
        window.resize(900, 700);
        page->setSize(window.size());
        page->setParentItem(window.contentItem());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        missingMailEndpoints = true;
        store->refresh();
        QTRY_COMPARE(store->state(), QStringLiteral("ready"));
        QVERIFY(store->selectedMessageUuid().isEmpty());
        QVERIFY(visible(page, "mailSampleNotice"));
        QCOMPARE(textOf(page, "mailSampleNotice"),
                 russian ? QStringLiteral("Демоящик. Письма и счётчики непрочитанных показаны для примера.")
                         : QStringLiteral("Sample mailbox. These messages and unread counts are examples."));
        auto *sent = item(page, "mailFolderRow-sent");
        QVERIFY(sent);
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                          sent->mapToScene(sent->boundingRect().center()).toPoint());
        QTRY_COMPARE(store->folderId(), QStringLiteral("sent"));
        QTRY_COMPARE(store->state(), QStringLiteral("ready"));
        QVERIFY(visible(page, "mailSampleNotice"));
        // If the messages endpoint is enabled first, its real rows must not
        // be labelled as sample messages alongside the built-in folder counts.
        missingMailEndpoints = false;
        missingFoldersOnly = true;
        store->refresh();
        QTRY_COMPARE(store->state(), QStringLiteral("ready"));
        QCOMPARE(textOf(page, "mailSampleNotice"),
                 russian ? QStringLiteral("Демопапки. Счётчики непрочитанных показаны для примера.")
                         : QStringLiteral("Sample folders. The unread counts are examples."));
        missingFoldersOnly = false;
        store->refresh();
        QTRY_COMPARE(store->state(), QStringLiteral("ready"));
        QVERIFY(!visible(page, "mailSampleNotice"));

        brokenFoldersOnce = true;
        store->refresh();
        QTRY_VERIFY(visible(page, "mailRetryButton"));
        QVERIFY(!visible(page, "mailSampleNotice"));
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
    QQuickItem *showMailWindow(QQmlEngine &engine, QQuickWindow &window, bool dark)
    {
        QQmlComponent palette(&engine, QUrl::fromLocalFile(QStringLiteral(
            WORKSPACE_NATIVE_MAIL_QML_SOURCE_DIR "/NativeTheme.qml")));
        QScopedPointer<QObject> theme(palette.createWithInitialProperties(
            {{"paletteId", QStringLiteral("orange-warm")}, {"dark", dark}}));
        if (!theme)
            return nullptr;
        const auto colors = theme->property("colors").value<QJSValue>().toVariant().toMap();
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(
            WORKSPACE_NATIVE_MAIL_QML_SOURCE_DIR "/NativeMailPage.qml")));
        const QVariantMap properties{{"colors", colors}, {"dark", dark}, {"mail", QVariant::fromValue(store)}};
        auto *page = qobject_cast<QQuickItem *>(component.createWithInitialProperties(properties));
        if (page) {
            window.setColor(colors.value("bg").value<QColor>());
            page->setParent(&window);
            page->setParentItem(window.contentItem());
            page->setSize(window.size());
            window.show();
        }
        return page;
    }

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
        if ((missingFoldersOnly && path == "/mail/folders")
            || (missingMessagesOnly && path == "/mail/messages")) {
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
    bool missingFoldersOnly = false;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    NativeMailTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "native-mail-test.moc"
