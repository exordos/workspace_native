#include "support/native-test-harness.h"

class ControllerFeedTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    // ---- Project Feed page (#38, docs/NATIVE_DESKTOP_FEED.md) ----

    static QJsonObject feedMessage(int number)
    {
        const auto id = QStringLiteral("78787878-7878-4878-8878-")
            + QString("%1").arg(number, 12, 16, QLatin1Char('0'));
        auto row = message(QStringLiteral("Feed message %1").arg(number), id);
        row.insert("is_own", false);
        row.insert("read", true);
        row.insert("created_at", QDateTime::fromString("2026-01-02T10:00:00Z", Qt::ISODate)
                         .addSecs(number).toString(Qt::ISODateWithMs));
        return row;
    }

    // Serves GET /messages/ like the Workspace feed query: newest items first
    // inside a page, page_limit caps the page and the continuation cursor
    // rides X-Pagination-Marker (feed.api.ts:43-78, messenger-client.ts:202-225).
    static Response feedPageResponse(const QMap<int, QJsonObject> &rows, const Request &request)
    {
        const auto query = QUrlQuery(request.target);
        const int limit = qMax(1, query.queryItemValue("page_limit").toInt());
        const auto marker = query.queryItemValue("page_marker");
        const int boundary = marker.isEmpty() ? std::numeric_limits<int>::max()
                                             : marker.right(12).toInt(nullptr, 16);
        QJsonArray items;
        bool more = false;
        for (auto it = rows.cend(); it != rows.cbegin();) {
            --it;
            if (it.key() >= boundary)
                continue;
            if (items.size() == limit) {
                more = true;
                break;
            }
            items.append(it.value());
        }
        Response response{items, 200, 0, {}};
        if (more)
            response.headers.insert("X-Pagination-Marker",
                items.last().toObject().value("uuid").toString().toUtf8());
        return response;
    }

    // The feed query is the only project-scoped page_limit=50 GET without
    // mentioned/stream/topic/uuid narrowing (projectScopedParams,
    // messenger-client.ts:96-119).
    static bool isFeedQuery(const QUrlQuery &query)
    {
        return query.queryItemValue("page_limit") == "50"
            && !query.hasQueryItem("mentioned") && !query.hasQueryItem("stream_uuid")
            && !query.hasQueryItem("topic_uuid") && !query.hasQueryItem("uuid");
    }

    void feedLoadsProjectScopedFirstPage()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, feedMessage(1)}, {2, feedMessage(2)}, {3, feedMessage(3)}});
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.messages = [rows, queries](const Request &request) {
            const QUrlQuery query(request.target);
            queries->append(query);
            if (isFeedQuery(query))
                return feedPageResponse(*rows, request);
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QVERIFY(fixture.controller.feedVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.feedPage().value("loading").toBool(), 4000);
        const auto list = fixture.controller.feedPage().value("rows").toList();
        QCOMPARE(list.size(), 3);
        // The store order is oldest-first with the newest at the list bottom
        // (feed.model.ts:39-68 sortUniqueMessages createdAt asc, uuid asc).
        QCOMPARE(list.first().toMap().value("id").toString(),
            feedMessage(1).value("uuid").toString());
        QCOMPARE(list.last().toMap().value("id").toString(),
            feedMessage(3).value("uuid").toString());
        bool sawFeedQuery = false;
        for (const auto &query : *queries) {
            if (!isFeedQuery(query))
                continue;
            sawFeedQuery = true;
            QCOMPARE(query.queryItemValue("project_id"), projectId);
            QVERIFY(!query.hasQueryItem("page_marker"));
            QVERIFY(!query.hasQueryItem("sort_key")); // the feed sends no sort params
        }
        QVERIFY(sawFeedQuery);
        // Row projection: time pill, "stream · topic" context pill, sender and
        // the clamped summary (feed-page.ui.tsx:363-424).
        const auto row = list.last().toMap();
        QVERIFY(!row.value("time").toString().isEmpty());
        QCOMPARE(row.value("context").toString(), QString("Native channel · Native topic"));
        QCOMPARE(row.value("preview").toString(), QString("Feed message 3"));
        // Author names resolve from authorUuid only (feed-page.ui.test.tsx:251).
        QVERIFY(!row.value("sender").toString().isEmpty());
        QVERIFY(!fixture.controller.feedPage().value("canLoadMore").toBool());
    }

    void feedPaginationFollowsMarkerOldestFirst()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>();
        for (int n = 1; n <= 60; ++n)
            rows->insert(n, feedMessage(n));
        auto queries = std::make_shared<QVector<QUrlQuery>>();
        fixture.messages = [rows, queries](const Request &request) {
            const QUrlQuery query(request.target);
            queries->append(query);
            if (isFeedQuery(query))
                return feedPageResponse(*rows, request);
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.feedPage().value("rows").toList().size() == 50, 4000);
        QVERIFY(fixture.controller.feedPage().value("canLoadMore").toBool());
        fixture.controller.loadMoreFeed();
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.feedPage().value("rows").toList().size() == 60, 4000);
        // The continuation rode X-Pagination-Marker into page_marker
        // (feed-page.ui.test.tsx:363, api-client.cpp:497).
        const bool marked = std::any_of(queries->cbegin(), queries->cend(),
            [](const QUrlQuery &query) { return isFeedQuery(query) && query.hasQueryItem("page_marker"); });
        QVERIFY(marked);
        const auto list = fixture.controller.feedPage().value("rows").toList();
        QCOMPARE(list.size(), 60);
        QSet<QString> ids;
        for (const auto &value : list)
            ids.insert(value.toMap().value("id").toString());
        QCOMPARE(ids.size(), 60); // dedupe by uuid (feed.model.ts sortUniqueMessages)
        // The older page merged in front: oldest first, newest last.
        QCOMPARE(list.first().toMap().value("id").toString(),
            feedMessage(1).value("uuid").toString());
        QCOMPARE(list.last().toMap().value("id").toString(),
            feedMessage(60).value("uuid").toString());
        QVERIFY(!fixture.controller.feedPage().value("canLoadMore").toBool());
    }

    void feedInFlightOlderPageIsNotDuplicated()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>();
        for (int n = 1; n <= 60; ++n)
            rows->insert(n, feedMessage(n));
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto markerRequests = std::make_shared<int>(0);
        fixture.messages = [rows, gates, markerRequests](const Request &request) {
            const QUrlQuery query(request.target);
            if (isFeedQuery(query) && query.hasQueryItem("page_marker")) {
                ++*markerRequests;
                Response response{feedPageResponse(*rows, request)};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            if (isFeedQuery(query))
                return feedPageResponse(*rows, request);
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.feedPage().value("rows").toList().size() == 50, 4000);
        fixture.controller.loadMoreFeed();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the older page is in flight
        // Never a second request while one is in flight
        // (feed-scroll.lib.ts shouldRequestOlderFeedPage isLoadingMore guard).
        fixture.controller.loadMoreFeed();
        fixture.controller.loadMoreFeed();
        for (auto &release : *gates)
            release();
        gates->clear();
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.feedPage().value("rows").toList().size() == 60, 4000);
        QCOMPARE(*markerRequests, 1);
    }

    void feedStaleOwnerSwitchDiscardsInFlightPage()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, feedMessage(1)}, {2, feedMessage(2)}});
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        fixture.messages = [rows, gates](const Request &request) {
            const QUrlQuery query(request.target);
            if (isFeedQuery(query)) {
                Response response{feedPageResponse(*rows, request)};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first page is in flight
        // An owner switch during the in-flight page clears the feed
        // (feed-page.ui.test.tsx:337) and the late response must not repopulate
        // it (feed-page.ui.test.tsx:407).
        fixture.controller.logout();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.authenticated(), 4000);
        for (auto &release : *gates)
            release();
        gates->clear();
        QTest::qWait(250); // give the late response a chance to misfire
        QVERIFY(!fixture.controller.feedVisible());
        QVERIFY(fixture.controller.feedPage().value("rows").toList().isEmpty());
    }

    // Issue #356 AC1/AC2/AC5: a normal same-owner token renewal replaces the
    // access token, which bumps the API generation and aborts the in-flight
    // first page; the shared request wrapper then drops its callback, so
    // nothing ever cleared feed inflight/loading and every later entry point
    // (showFeed, loadMoreFeed, retryFeed) refused to load for the rest of the
    // controller session. The renewal must settle the interrupted page, keep
    // the accepted rows and marker, let a reopen dispatch a fresh GET, and the
    // retired response must not settle or replace the newer request.
    void feedSameOwnerRenewalSettlesFirstPageAndReopenReloads()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, feedMessage(1)}, {2, feedMessage(2)}});
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto feedGets = std::make_shared<int>(0);
        fixture.messages = [rows, gates, feedGets](const Request &request) {
            const QUrlQuery query(request.target);
            if (isFeedQuery(query)) {
                ++*feedGets;
                Response response{feedPageResponse(*rows, request)};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first page is in flight
        QVERIFY(fixture.controller.feedPage().value("loading").toBool());
        const int firstPageGets = *feedGets;
        fixture.api.setAccessToken("synthetic-renewed-access-token");
        // AC1: the interrupted page settles its transient state; the page is
        // still open and shows no error card for an interrupted request.
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.feedPage().value("loading").toBool(), 4000);
        QVERIFY(!fixture.controller.feedPage().value("loadingMore").toBool());
        QVERIFY(!fixture.controller.feedPage().value("error").toBool());
        QVERIFY(fixture.controller.feedVisible());
        // AC2: reopening under the renewed session dispatches a fresh GET.
        const auto staleSends = gates->size();
        fixture.controller.closeFeed();
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(*feedGets >= firstPageGets + 1, 4000);
        QVERIFY(fixture.controller.feedPage().value("loading").toBool());
        // AC5: the retired first-page answers may not settle the newer request.
        for (size_t i = 0; i < staleSends; ++i)
            (*gates)[i]();
        QTest::qWait(250); // give the retired callbacks a chance to misfire
        QVERIFY(fixture.controller.feedPage().value("loading").toBool());
        QVERIFY(fixture.controller.feedPage().value("rows").toList().isEmpty());
        // The newer request's own answer settles it and the feed stays usable.
        for (size_t i = staleSends; i < gates->size(); ++i)
            (*gates)[i]();
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.feedPage().value("rows").toList().size() == 2, 4000);
        QVERIFY(!fixture.controller.feedPage().value("loading").toBool());
    }

    // Issue #356 AC1/AC3/AC4: the same generation bump interrupts an older-page
    // (load-more) request. The pagination overlay must settle, the accepted
    // newest page and its cursor survive, and load-more may run again.
    void feedSameOwnerRenewalSettlesInterruptedOlderPage()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>();
        for (int n = 1; n <= 60; ++n)
            rows->insert(n, feedMessage(n));
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        auto markerGets = std::make_shared<int>(0);
        fixture.messages = [rows, gates, markerGets](const Request &request) {
            const QUrlQuery query(request.target);
            if (isFeedQuery(query) && query.hasQueryItem("page_marker")) {
                ++*markerGets;
                Response response{feedPageResponse(*rows, request)};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            if (isFeedQuery(query))
                return feedPageResponse(*rows, request);
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.feedPage().value("rows").toList().size() == 50, 4000);
        fixture.controller.loadMoreFeed();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the older page is in flight
        QVERIFY(fixture.controller.feedPage().value("loadingMore").toBool());
        fixture.api.setAccessToken("synthetic-renewed-access-token");
        // AC1: the interrupted older page settles; the overlay is gone.
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.feedPage().value("loadingMore").toBool(), 4000);
        QVERIFY(!fixture.controller.feedPage().value("loading").toBool());
        // AC3: the accepted newest page and its pagination marker are retained.
        QCOMPARE(fixture.controller.feedPage().value("rows").toList().size(), 50);
        QVERIFY(fixture.controller.feedPage().value("canLoadMore").toBool());
        // AC2/AC4: a new older-page GET runs under the renewed session.
        const auto staleSends = gates->size();
        const int markerGetsBefore = *markerGets;
        fixture.controller.loadMoreFeed();
        QTRY_VERIFY_WITH_TIMEOUT(*markerGets >= markerGetsBefore + 1, 4000);
        for (size_t i = 0; i < staleSends; ++i)
            (*gates)[i]();
        QTest::qWait(250);
        QCOMPARE(fixture.controller.feedPage().value("rows").toList().size(), 50);
        for (size_t i = staleSends; i < gates->size(); ++i)
            (*gates)[i]();
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.feedPage().value("rows").toList().size() == 60, 4000);
        QVERIFY(!fixture.controller.feedPage().value("loadingMore").toBool());
    }

    // Issue #356 AC5: signing out (owner replacement) retires the Feed request
    // identity: no loading state may outlive the owner, and the late answer of
    // the retired page may not bring the previous owner's feed back.
    void feedSignOutClearsFeedRequestIdentity()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, feedMessage(1)}, {2, feedMessage(2)}});
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        fixture.messages = [rows, gates](const Request &request) {
            const QUrlQuery query(request.target);
            if (isFeedQuery(query)) {
                Response response{feedPageResponse(*rows, request)};
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000); // the first page is in flight
        QVERIFY(fixture.controller.feedPage().value("loading").toBool());
        fixture.controller.logout();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.authenticated(), 4000);
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.feedPage().value("loading").toBool(), 4000);
        QVERIFY(!fixture.controller.feedPage().value("loadingMore").toBool());
        for (auto &release : *gates)
            release();
        gates->clear();
        QTest::qWait(250); // give the late response a chance to misfire
        QVERIFY(!fixture.controller.feedVisible());
        QVERIFY(fixture.controller.feedPage().value("rows").toList().isEmpty());
        QVERIFY(!fixture.controller.feedPage().value("loading").toBool());
    }

    void feedErrorAndRetry()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, feedMessage(1)}});
        auto failing = std::make_shared<bool>(true);
        fixture.messages = [rows, failing](const Request &request) {
            const QUrlQuery query(request.target);
            if (isFeedQuery(query)) {
                if (*failing)
                    return Response{QJsonObject{{"detail", "synthetic outage"}}, 500, 0, {}};
                return feedPageResponse(*rows, request);
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(fixture.controller.feedPage().value("error").toBool(), 4000);
        QVERIFY(fixture.controller.feedPage().value("rows").toList().isEmpty());
        *failing = false;
        fixture.controller.retryFeed();
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.feedPage().value("rows").toList().size() == 1, 4000);
        QVERIFY(!fixture.controller.feedPage().value("error").toBool());
    }

    // The preview reproduces the reference summarizeWorkspaceMessageMarkdown
    // contract for inline syntax: FEED_MESSAGE_SUMMARY_OPTIONS maxLength 80
    // with media/attachment labels and the quote prefix (feed-page.ui.tsx:64-69);
    // summarizeFileInline, summarizeLinkInline and summarizeParagraph in
    // workspace-message-summary.lib.ts replace link targets and attachment
    // urns with safe labels BEFORE the clamp truncates, so no raw urn or link
    // target survives into the preview (feed-page.ui.test.tsx:279-297 pins the
    // media case as "Изображение: Вот скрин" with no urn and no file uuid).
    void feedPreviewSummarizesMarkdownWithoutRawUrls()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto withContent = [](int number, const QString &content) {
            auto row = feedMessage(number);
            auto payload = row.value("payload").toObject();
            payload.insert("content", content);
            row.insert("payload", payload);
            return row;
        };
        const QString longText(78, QLatin1Char('a'));
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, withContent(1, QStringLiteral(
                "![screen.png](urn:image:11111111-1111-4111-8111-111111111111?name=screen.png) Вот скрин"))},
            {2, withContent(2, QStringLiteral(
                "Docs at [the handbook](https://example.com/docs/secret) daily"))},
            {3, withContent(3, QStringLiteral(
                "![report.pdf](urn:file:22222222-2222-4222-8222-222222222222?name=report.pdf&content_type=application/pdf&size=1024) attached"))},
            {4, withContent(4, QStringLiteral(
                "Hi [Alice](urn:user:33333333-3333-4333-8333-333333333333)"))},
            {5, withContent(5, QStringLiteral("> follow up please"))},
            {6, withContent(6, QStringLiteral("<img src=\"http://evil.invalid/track.png\"> ping"))},
            {7, withContent(7, longText + QStringLiteral(
                " [the handbook](https://example.com/very/long/path?with=query&more=segments#anchor) end"))},
        });
        fixture.messages = [rows](const Request &request) {
            const QUrlQuery query(request.target);
            if (isFeedQuery(query))
                return feedPageResponse(*rows, request);
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.feedPage().value("rows").toList().size() == 7, 4000);
        // Rows project oldest-first (feed.model.ts sortUniqueMessages), so
        // feed message N sits at index N-1.
        const auto preview = [&](int number) {
            const auto list = fixture.controller.feedPage().value("rows").toList();
            return list.at(number - 1).toMap().value("preview").toString();
        };
        QCOMPARE(preview(1), QStringLiteral("Image: Вот скрин"));
        QCOMPARE(preview(2), QStringLiteral("Docs at the handbook daily"));
        QCOMPARE(preview(3), QStringLiteral("File: report.pdf attached"));
        QCOMPARE(preview(4), QStringLiteral("Hi @Alice"));
        QCOMPARE(preview(5), QStringLiteral("Quote: follow up please"));
        // Attacker-supplied HTML stays literal text; the QML side must keep
        // rendering it plain (never rich text). The url carried in the src
        // attribute is still a link destination, so the URL-token invariant
        // collapses it to the neutral `Link` label (exo-review thread
        // 4581966233 round 3; docs/NATIVE_DESKTOP_FEED.md deviation note).
        QCOMPARE(preview(6), QStringLiteral("<img Link ping"));
        QVERIFY(!preview(6).contains(QStringLiteral("evil")));
        // The clamp runs on the summarized text: the link target is gone
        // before truncation, so the summary keeps its tail label.
        QVERIFY(!preview(7).contains(QStringLiteral("https://")));
        QVERIFY(preview(7).endsWith(QStringLiteral("...")));
        QCOMPARE(preview(7).size(), 80);
        QVERIFY(!preview(1).contains(QStringLiteral("urn:")));
        QVERIFY(!preview(1).contains(QStringLiteral("11111111")));
        QVERIFY(!preview(3).contains(QStringLiteral("22222222")));
    }

    // exo-review round 2 (thread 4581966233): the summarize pass only matched
    // the plain `[label](target)` shape, so every other Markdown link form kept
    // its raw source - and with it its link target - in the preview. The
    // reference parses the full AST (workspace-message-summary.lib.ts
    // summarizeLinkInline/summarizeFileInline, with isReadableLinkLabel at
    // lines 61-68) and prints only the readable label for all of these forms;
    // reference-style definitions never render at all, and truncatePreview must
    // never leave a target fragment behind.
    void feedPreviewHidesUncommonMarkdownLinkTargets()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto withContent = [](int number, const QString &content) {
            auto row = feedMessage(number);
            auto payload = row.value("payload").toObject();
            payload.insert("content", content);
            row.insert("payload", payload);
            return row;
        };
        const QString head(60, QLatin1Char('a'));
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            {1, withContent(1, QStringLiteral(
                "[handbook][1]\n\n[1]: https://intranet.example/private/doc"))},
            {2, withContent(2, QStringLiteral(
                "Read [report](https://intranet.example/x 'internal') now"))},
            {3, withContent(3, QStringLiteral(
                "see [a\\]b](https://intranet.example/leak) ok"))},
            {4, withContent(4, QStringLiteral(
                "[![shot](urn:image:44444444-4444-4444-9444-444444444444)]"
                "(https://intranet.example/leak)"))},
            {5, withContent(5, QStringLiteral(
                "open [x](<https://intranet.example/a b>) tail"))},
            {6, withContent(6, head + QStringLiteral(
                " https://intranet.example/secret/token=abcdefghij"))},
            {7, withContent(7, QStringLiteral(
                "note [a][u]\n\n[u]: urn:file:55555555-5555-4555-8555-555555555555?name=secret.pdf"))},
            {8, withContent(8, QStringLiteral(
                "plain [docs](https://example.com/handbook) tail"))},
            // Round 3 sweep (thread 4581966233): hostile rows that survive the
            // markdown passes above because the reference itself prints their
            // text verbatim - autolinks whose label IS the destination, URLs
            // typed as plain text, raw <...> autolinks, URL-shaped labels,
            // scheme-case variants - plus a destination whose first characters
            // cross the 80-character clamp.
            {9, withContent(9, QStringLiteral(
                "[https://intranet.example/secret?k=1](https://intranet.example/secret?k=1)"))},
            {10, withContent(10, QStringLiteral(
                "Read the doc at https://intranet.example/secret/a?k=1 ok"))},
            {11, withContent(11, QStringLiteral(
                "see [www.example.com/leak](https://intranet.example/secret) now"))},
            {12, withContent(12, QStringLiteral(
                "<https://intranet.example/raw>"))},
            {13, withContent(13, QStringLiteral(
                "see doc https://leak.example/x now"))},
            {14, withContent(14, QStringLiteral(
                "[HTTP://EXAMPLE.EXAMPLE/SECRET](HTTP://EXAMPLE.EXAMPLE/SECRET)"))},
            {15, withContent(15, QStringLiteral(
                "[urn:uuid:secret-id](urn:uuid:secret-id)"))},
            {16, withContent(16, QStringLiteral(
                "[https://public.example/label](https://intranet.example/target)"))},
            {17, withContent(17, QStringLiteral(
                "note the url https://intranet.example/x?sig=deadbeefcafe123"))},
            {18, withContent(18, QStringLiteral("x").repeated(74) + QStringLiteral(
                " https://leak.example/p?token=supersecretvalue"))},
            // Round 3 addendum (exo-review re-review at 75c589b): destinations
            // with balanced parentheses - CommonMark keeps them inside the
            // destination, and the old destination scan stopped at the first
            // `(`, stranding the rest of the target as plain text.
            {19, withContent(19, QStringLiteral(
                "note [a](https://intranet.example/f(bar)) tail"))},
            {20, withContent(20, QStringLiteral(
                "[a](https://intranet.example/(x)?q=SECRET) tail"))},
            {21, withContent(21, QStringLiteral(
                "[x](https://intranet.example/y (note)) end"))},
            {22, withContent(22, QStringLiteral(
                "[v](https://intranet.example/((a)(b))/top?secret) end"))},
        });
        fixture.messages = [rows](const Request &request) {
            const QUrlQuery query(request.target);
            if (isFeedQuery(query))
                return feedPageResponse(*rows, request);
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.feedPage().value("rows").toList().size() == 22, 4000);
        const auto preview = [&](int number) {
            const auto list = fixture.controller.feedPage().value("rows").toList();
            return list.at(number - 1).toMap().value("preview").toString();
        };
        // Invariant (exo-review thread 4581966233): after summarization and
        // clamping a preview never carries a scheme marker (`://` in any case,
        // `urn:`, `mailto:`), a known destination host or an urn payload -
        // not in full and not as a fragment cut by the clamp.
        QStringList leaked;
        const QString xTail(74, QLatin1Char('x'));
        // Every row has an exact green value (row 15 excepted: its urn is
        // dropped by the residue pass today and collapsing it to `Link` must
        // not make it re-appear, so only the markers below pin it). The
        // witness loop flags marker leaks AND deviations from the safe
        // summary, so one run names every leak even when it ends in the
        // clamp-cut `ht` fragment of row 18 that carries no marker.
        QMap<int, QString> exact;
        exact[1] = QStringLiteral("handbook");
        exact[2] = QStringLiteral("Read report now");
        exact[3] = QStringLiteral("see a]b ok");
        exact[4] = QStringLiteral("Image");
        exact[5] = QStringLiteral("open x tail");
        exact[6] = head + QStringLiteral("...");
        exact[7] = QStringLiteral("note a");
        exact[8] = QStringLiteral("plain docs tail");
        exact[9] = QStringLiteral("Link");
        exact[10] = QStringLiteral("Read the doc at Link ok");
        exact[11] = QStringLiteral("see www.example.com/leak now");
        exact[12] = QStringLiteral("Link");
        exact[13] = QStringLiteral("see doc Link now");
        exact[14] = QStringLiteral("Link");
        exact[16] = QStringLiteral("Link");
        exact[17] = QStringLiteral("note the url Link");
        exact[18] = xTail + QStringLiteral("...");
        exact[19] = QStringLiteral("note a tail");
        exact[20] = QStringLiteral("a tail");
        exact[21] = QStringLiteral("x end");
        exact[22] = QStringLiteral("v end");
        for (int number = 1; number <= 22; ++number) {
            const auto value = preview(number);
            const auto lowered = value.toLower();
            bool suspect = lowered.contains(QStringLiteral("://"))
                || lowered.contains(QStringLiteral("urn:"))
                || lowered.contains(QStringLiteral("mailto:"))
                || lowered.contains(QStringLiteral("intranet"))
                || lowered.contains(QStringLiteral("leak.example"))
                || lowered.contains(QStringLiteral("evil.invalid"))
                || lowered.contains(QStringLiteral("secret"))
                || lowered.contains(QStringLiteral("public.example"))
                || value.contains(QStringLiteral("EXAMPLE.EXAMPLE"))
                || value.contains(QStringLiteral("!(")) || value.contains(QStringLiteral("]("));
            if (exact.contains(number) && value != exact.value(number))
                suspect = true;
            if (suspect)
                leaked << QStringLiteral("row %1: actual [%2] expected [%3]")
                              .arg(number).arg(value, exact.value(number));
        }
        for (const auto &row : leaked)
            qWarning().noquote() << QStringLiteral("LEAK-WITNESS %1").arg(row);
        const QStringList expected;
        QCOMPARE(leaked, expected);
        QCOMPARE(preview(1), QStringLiteral("handbook"));
        QCOMPARE(preview(2), QStringLiteral("Read report now"));
        QCOMPARE(preview(3), QStringLiteral("see a]b ok"));
        QCOMPARE(preview(4), QStringLiteral("Image"));
        QCOMPARE(preview(5), QStringLiteral("open x tail"));
        QCOMPARE(preview(7), QStringLiteral("note a"));
        // The reference autolink/label contract is untouched for ordinary links.
        QCOMPARE(preview(8), QStringLiteral("plain docs tail"));
        // The clamp may never cut inside a target: dropping the trailing token
        // is the only way to keep the "no raw target" invariant (truncatePreview,
        // workspace-message-summary.lib.ts:38-52).
        QVERIFY(!preview(6).contains(QStringLiteral("://")));
        QVERIFY(preview(6).endsWith(QStringLiteral("...")));
        QVERIFY(preview(6).startsWith(head));
        // Round 3: every URL-shaped row collapses to the neutral `Link` label.
        // The reference would print rows 9-17 as their URL text verbatim
        // (summarizeLinkInline fallback, workspace_ui 61dacef78dab:97-112);
        // the feed privacy invariant wins (docs/NATIVE_DESKTOP_FEED.md).
        QCOMPARE(preview(9), QStringLiteral("Link"));
        QCOMPARE(preview(10), QStringLiteral("Read the doc at Link ok"));
        // A plain `www.` host label is not a destination and stays visible.
        QCOMPARE(preview(11), QStringLiteral("see www.example.com/leak now"));
        QCOMPARE(preview(12), QStringLiteral("Link"));
        QCOMPARE(preview(13), QStringLiteral("see doc Link now"));
        QCOMPARE(preview(14), QStringLiteral("Link"));
        QVERIFY(!preview(15).contains(QStringLiteral("secret-id")));
        QVERIFY(!preview(15).contains(QStringLiteral("urn:")));
        QCOMPARE(preview(16), QStringLiteral("Link"));
        QCOMPARE(preview(17), QStringLiteral("note the url Link"));
        // The clamp cuts at whole tokens even when the destination crosses
        // the limit inside its first characters (`...ht` of `https` is already
        // a partial destination): the entire token is dropped, never trimmed
        // down to a fragment. (The witness loop above asserts the same.)
        QCOMPARE(preview(18), xTail + QStringLiteral("..."));
    }

    // exo-review round 4 (thread 4586950726): the bare destination was a
    // greedy non-space run that backtracked to the LAST closing parenthesis,
    // so a link immediately followed by more non-space text (an adjacent
    // second link, with or without separators inside the run) was swallowed
    // into one match and every label after the first disappeared. The
    // destination must be parsed as a CommonMark balanced-parenthesis run
    // that stops at its own matching `)`.
    void feedPreviewKeepsAllAdjacentLinkLabels()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        const auto withContent = [](int number, const QString &content) {
            auto row = feedMessage(number);
            auto payload = row.value("payload").toObject();
            payload.insert("content", content);
            row.insert("payload", payload);
            return row;
        };
        const auto rows = std::make_shared<QMap<int, QJsonObject>>(QMap<int, QJsonObject>{
            // RED witnesses: nothing separates the two links' inline runs,
            // so the old greedy capture ran from the first `(` to the last
            // `)` and the second link's label was eaten.
            {1, withContent(1, QStringLiteral(
                "see [docs](https://example.com/a)and[handbook](https://example.com/b) now"))},
            {2, withContent(2, QStringLiteral(
                "see [docs](https://example.com/a)[handbook](https://example.com/b) now"))},
            {3, withContent(3, QStringLiteral(
                "[one](https://example.com/x)[two](https://example.com/z) end"))},
            // Regression guards: the space-separated pair and the balanced
            // destinations fixed in round 3 must keep their exact output,
            // including a deeply nested destination on the SECOND link.
            {4, withContent(4, QStringLiteral(
                "open [x](https://example.com/a(b)) and [y](https://example.com/c(d(e))) tail"))},
            {5, withContent(5, QStringLiteral(
                "see [docs](https://example.com/a) and [handbook](https://example.com/b) now"))},
            {6, withContent(6, QStringLiteral(
                "open [x](https://example.com/a(b)) tail"))},
        });
        fixture.messages = [rows](const Request &request) {
            const QUrlQuery query(request.target);
            if (isFeedQuery(query))
                return feedPageResponse(*rows, request);
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(
            fixture.controller.feedPage().value("rows").toList().size() == 6, 4000);
        const auto preview = [&](int number) {
            const auto list = fixture.controller.feedPage().value("rows").toList();
            return list.at(number - 1).toMap().value("preview").toString();
        };
        // Each link contributes exactly its own label; CommonMark keeps the
        // separator text that sits inside the old greedy run (`and`, `y`).
        QCOMPARE(preview(1), QStringLiteral("see docs and handbook now"));
        QCOMPARE(preview(2), QStringLiteral("see docs handbook now"));
        QCOMPARE(preview(3), QStringLiteral("one two end"));
        QCOMPARE(preview(4), QStringLiteral("open x and y tail"));
        QCOMPARE(preview(5), QStringLiteral("see docs and handbook now"));
        QCOMPARE(preview(6), QStringLiteral("open x tail"));
        for (int number = 1; number <= 6; ++number)
            QVERIFY2(!preview(number).contains(QStringLiteral("://")),
                qPrintable(preview(number)));
    }

    void feedLoadingAndEmptyStates()
    {
        Fixture fixture;
        fixture.aggregateMessages = true;
        auto gates = std::make_shared<std::vector<std::function<void()>>>();
        fixture.messages = [gates](const Request &request) {
            const QUrlQuery query(request.target);
            if (isFeedQuery(query)) {
                Response response{QJsonArray{}, 200, 0, {}}; // empty project feed
                response.defer = [gates](std::function<void()> send) { gates->push_back(std::move(send)); };
                return response;
            }
            return Response{QJsonArray{}};
        };
        QVERIFY(fixture.start());
        fixture.controller.showFeed();
        QTRY_VERIFY_WITH_TIMEOUT(!gates->empty(), 2000);
        QVERIFY(fixture.controller.feedPage().value("loading").toBool()); // app.loading card
        for (auto &release : *gates)
            release();
        gates->clear();
        QTRY_VERIFY_WITH_TIMEOUT(!fixture.controller.feedPage().value("loading").toBool(), 4000);
        // Empty without error: the reference renders the feed.noMessages card.
        QVERIFY(fixture.controller.feedPage().value("rows").toList().isEmpty());
        QVERIFY(!fixture.controller.feedPage().value("error").toBool());
        QVERIFY(!fixture.controller.feedPage().value("canLoadMore").toBool());
        fixture.controller.closeFeed();
        QVERIFY(!fixture.controller.feedVisible());
    }

    void feedIsExclusiveContentPage()
    {
        Fixture fixture;
        QVERIFY(fixture.start());
        fixture.controller.showInbox();
        QVERIFY(fixture.controller.inboxVisible());
        fixture.controller.showFeed();
        QVERIFY(fixture.controller.feedVisible());
        QVERIFY(!fixture.controller.inboxVisible());
        fixture.controller.closeFeed();
        QVERIFY(!fixture.controller.feedVisible());
    }
};

QTEST_MAIN(ControllerFeedTest)
#include "native-controller-feed-test.moc"
