#include "support/native-test-harness.h"
#include "support/controller-mention-fixtures.h"

class ControllerMentionsTest final : public NativeControllerTestHarness {
    Q_OBJECT
private slots:
    // ---- Issue #123 composer @mention suggestions ----
    // Ranking constants and ordering are pinned by the design doc
    // docs/NATIVE_DESKTOP_COMPOSER_MENTION.md, measured from workspace_ui
    // mention-suggest.lib.ts and message-composer-mention-context.lib.ts.

    static QVariantMap mentionRow(const QString &uuid, const QString &display,
        const QString &username, const QString &email = {}, const QString &uuidValue = {})
    {
        return {{"userUuid", uuidValue.isEmpty() ? uuid : uuidValue}, {"displayName", display},
            {"username", username}, {"email", email}, {"status", QString{}}, {"avatarUrl", QString{}}};
    }

    static QStringList mentionOrder(const QVariantList &rows)
    {
        QStringList order;
        for (const auto &row : rows) order << row.toMap().value("userUuid").toString();
        return order;
    }

    void mentionRankOrdersConversationContext()
    {
        const auto alice = mentionUuid("0a1c"), bob = mentionUuid("0b0b"), carl = mentionUuid("0c11");
        const auto dana = mentionUuid("0d1a"), eve = mentionUuid("0e1e"), self = mentionUuid("0f");
        const QVariantList users{mentionRow(alice, "Alice", "alice"), mentionRow(bob, "Bob", "bob"),
            mentionRow(carl, "Carl", "carl"), mentionRow(dana, "Dana", "dana"),
            mentionRow(eve, "Eve", "eve"), mentionRow(self, "Self", "self")};
        NativeController::MentionRankingContext context;
        context.membersKnown = true;
        context.channelMembers = {alice, carl};
        context.recentAuthorUuids = {carl, bob};
        context.dmPartnerUuids = {eve};
        context.selfUserUuid = self;
        // Scores (mention-suggest.lib.ts): carl 4000+1500, bob 3900, alice 1500,
        // eve 1200, dana 0, self -5000. The empty query keeps everyone at tier 0.
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QString{}, users, context)),
            (QStringList{carl, bob, alice, eve, dana, self}));
        // Membership marking (dropdown contract): only known non-members carry the
        // outsideChannel fact; members never do.
        const auto rows = NativeController::rankMentionSuggestions(QString{}, users, context);
        for (const auto &rowValue : rows)
        {
            const auto row = rowValue.toMap();
            const auto uuid = row.value("userUuid").toString();
            if (uuid == alice || uuid == carl) QVERIFY(!row.contains("outsideChannel"));
            else QCOMPARE(row.value("outsideChannel").toBool(), true);
        }
        // Unknown membership (unloaded-membership state) marks nothing.
        context.membersKnown = false;
        for (const auto &rowValue : NativeController::rankMentionSuggestions(QString{}, users, context))
            QVERIFY(!rowValue.toMap().contains("outsideChannel"));
        // Frecency lifts one equal-match candidate: min(score, 10) * 60 (mention-suggest.lib.ts).
        NativeController::MentionRankingContext bare;
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QStringLiteral("b"), users, bare)),
            (QStringList{bob}));
        bare.frecencyByUserUuid = {{dana, 5.0}};
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QStringLiteral("bo"), users, bare)),
            (QStringList{bob}));
    }

    void mentionRankTriggerRequiresBoundaryBeforeAt()
    {
        // ComposerMentionTrigger.REMINDER in the reference: a mention token only
        // starts after whitespace/line start/punctuation ([ \([{,.:;!?]), not after
        // a word character ("mail@domain" or "user@bob" must not open the popup).
        Fixture f;
        f.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "cassi"}},
            QJsonObject{{"uuid", mentionUuid("0b0b")}, {"username", "bob"},
                {"first_name", "Bob"}, {"last_name", "Ray"}}}}; };
        QVERIFY(f.start());
        QVERIFY(f.controller.composerMentionsEnabled());
        // Empty query opens the full suggestion list (empty query = no filter);
        // The composer can mention the viewer, while the creation picker offers only peers.
        QCOMPARE(mentionOrder(f.controller.mentionSuggestions(QString{})),
            (QStringList{mentionUuid("0b0b"), userId}));
        QCOMPARE(f.controller.channelMemberChoices().size(), 1);
        QCOMPARE(f.controller.channelMemberChoices().first().toMap().value("id").toString(), mentionUuid("0b0b"));
        QCOMPARE(mentionOrder(f.controller.mentionSuggestions(QStringLiteral("bob"))),
            (QStringList{mentionUuid("0b0b")}));
    }

    void mentionRankEmptyQueryOrdersMembersFrecencySelf()
    {
        NativeController::MentionRankingContext context;
        context.selfUserUuid = QStringLiteral("self");
        context.membersKnown = true;
        context.channelMembers = {QStringLiteral("in"), QStringLiteral("self")};
        context.frecencyByUserUuid = {{"in", 1.0}}; // FRECENCY_SCORE_WEIGHT 60
        const QVariantList users{
            mentionRow("self", "Sally Self", "sally"),
            mentionRow("in", "Ivan In", "ivan"),
            mentionRow("out", "Ola Out", "ola")};
        // Tier 0 order: Ivan (member +1500 and frecency 60), then Ola (zero),
        // then Sally last: member +1500 but self penalty -5000 (mention-suggest.lib.ts).
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QString{}, users, context)),
            (QStringList{"in", "out", "self"}));
        // Members marked: only the outsider carries the fact (applyChannelMembership).
        const auto rows = NativeController::rankMentionSuggestions(QString{}, users, context);
        QVERIFY(rows.at(1).toMap().value("outsideChannel").toBool());
        QVERIFY(!rows.at(0).toMap().contains("outsideChannel"));
    }

    void mentionRankContextScoresTieNameLengthAndOrder()
    {
        NativeController::MentionRankingContext context;
        // Recent author order scores 4000 - 100*i (i < 20); DM partner 1200 - 40*i.
        context.recentAuthorUuids = {"late", "early"};
        context.dmPartnerUuids = {"partner"};
        const QVariantList users{
            mentionRow("early", "Early Author", "ea"),
            mentionRow("late", "Late Author", "la"),
            mentionRow("partner", "Ada Partner", "ap"),
            mentionRow("zero", "Zoe Z", "zz")};
        // "late" is recent-author index 0 (+4000), "early" index 1 (+3900),
        // "partner" is DM partner index 0 (+1200), "zero" carries no context.
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QString{}, users, context)),
            (QStringList{"late", "early", "partner", "zero"}));
        // Same tier + same score with no query: locale order of the display name
        // decides (name length is only a tie-breaker while a query is typed).
        const QVariantList tied{
            mentionRow("u3", "Sam Ray", "s3"), mentionRow("u1", "Sam", "s1"),
            mentionRow("u4", "Bob Ray", "b1"), mentionRow("u2", "Bix", "b2")};
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QString{}, tied, {})),
            (QStringList{"u2", "u4", "u1", "u3"}));
        // A query tie prefers the shorter display name too; without a query the
        // name-length term is skipped (mention-suggest.lib.ts sortMentionCandidates).
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QStringLiteral("sa"), tied, {})),
            (QStringList{"u1", "u3"}));
    }

    void mentionSerializeUrnsBeforeReplyQuote()
    {
        // Visible composer text stores "@DisplayName"; only the wire body carries
        // the canonical "[Name](urn:user:uuid)" (message-composer-body.lib.ts
        // buildOutgoingMessageBody: mentions serialize before the quote join).
        Fixture f;
        const auto bob = mentionUuid("0b0b"), ann = mentionUuid("0a55");
        f.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "cassi"}},
            QJsonObject{{"uuid", bob}, {"username", "bob"}, {"first_name", "Bob"}, {"last_name", "Ray"}},
            QJsonObject{{"uuid", ann}, {"username", "ann"}, {"first_name", "Ann"}}}}; };
        const auto quotedId = QStringLiteral("89000000-0000-4000-8000-000000000001");
        auto note = message(QStringLiteral("Peer note"), quotedId);
        note["author_uuid"] = ann; note["is_own"] = false;
        QJsonObject posted, saved;
        f.messages = [&](const Request &request) -> Response {
            if (request.method == "POST") {
                posted = request.body;
                saved = message(request.body.value("payload").toObject()
                    .value("content").toString(), QStringLiteral("89000000-0000-4000-8000-000000000002"));
                saved["user_uuid"] = userId;
                return Response{saved, 201};
            }
            if (request.method == "GET" && request.target.path()
                .endsWith(QStringLiteral("89000000-0000-4000-8000-000000000002")) && !saved.isEmpty())
                return Response{saved};
            return Response{QJsonArray{note}};
        };
        QVERIFY(f.start());
        QVERIFY(f.controller.beginReply(quotedId));
        QTRY_COMPARE(f.controller.replyAuthor(), QStringLiteral("Ann"));
        f.controller.setDraft(QStringLiteral("Hi @Bob Ray!"));
        f.controller.send();
        QTRY_VERIFY_WITH_TIMEOUT(!posted.isEmpty(), 4000);
        const auto content = posted.value("payload").toObject().value("content").toString();
        // Ann's quote line stays first, the mention URN lands in the answer part,
        // and the draft/visible path never contains the URN.
        QCOMPARE(content, QStringLiteral("[Ann](urn:quote:%1)\n\nHi [Bob Ray](urn:user:%2)!")
            .arg(quotedId, bob));
        QTRY_VERIFY_WITH_TIMEOUT(f.controller.draft().isEmpty(), 4000);
    }

    void mentionRankMatchTiersAndLimit()
    {
        // Tiers from mention-suggest.lib.ts resolveMatchTier: exact 0, prefix 1,
        // word prefix 2, substring 3, email 4, uuid 5 (uuid only for queries >= 4).
        const auto exact = mentionUuid("10"), prefix = mentionUuid("11"), word = mentionUuid("12");
        const auto substr = mentionUuid("13"), mail = mentionUuid("14");
        const QString hexUuid = QStringLiteral("deadbeef-1111-4111-8111-111111111111");
        const auto hexRow = mentionRow("uu", "Hex Person", "hp", {}, hexUuid);
        const QVariantList users{
            mentionRow(substr, "Di", "xivanx"), mentionRow(prefix, "Ivanov", "ivanov"),
            mentionRow(word, "Pavel Ivanov", "pavel"), mentionRow(exact, "Ivan", "ivan"),
            mentionRow(mail, "Hit", "hit", "hit@iv.example"), hexRow};
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QStringLiteral("ivan"), users, {})),
            (QStringList{exact, prefix, word, substr}));
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QStringLiteral("iv"), users, {})),
            (QStringList{exact, prefix, word, substr, mail}));
        // A deliberate >= 4-character UUID fragment matches; a 3-character one does not.
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QStringLiteral("dead"), users, {})),
            (QStringList{hexUuid}));
        QVERIFY(NativeController::rankMentionSuggestions(QStringLiteral("dea"), users, {}).isEmpty());
        // MENTION_SUGGESTION_LIMIT = 8 from mention-suggest.lib.ts.
        QVariantList many;
        for (int index = 0; index < 10; ++index)
            many.append(mentionRow(QStringLiteral("u%1").arg(index, 2, 10, QLatin1Char('0')),
                QStringLiteral("U%1 Substring").arg(index, 2, 10, QLatin1Char('0')),
                QStringLiteral("sub%1u").arg(index)));
        QCOMPARE(NativeController::rankMentionSuggestions(QStringLiteral("su"), many, {}).size(), 8);
        // Multi-token queries pair each token with a distinct word (word-prefix tier 2).
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QStringLiteral("pa iv"), users, {})),
            (QStringList{word}));
    }

    void mentionRankFoldsCyrillicYeAndAccentedLatin()
    {
        // normalizeMentionText (mention-suggest.lib.ts) NFD-decomposes and drops
        // combining marks before matching: a plain "елена" query must find the
        // display name spelled with "ё" (U+0451 -> е U+0435), and "jose" must
        // find "José" through the stripped U+0301.
        const auto elena = mentionUuid("1a"), jose = mentionUuid("1b");
        const QVariantList users{
            mentionRow(elena, QStringLiteral("Елёна Иванова"), "el"),
            mentionRow(jose, QStringLiteral("José"), "jr7")};
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QStringLiteral("елена"), users, {})),
            (QStringList{elena}));
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QStringLiteral("jose"), users, {})),
            (QStringList{jose}));
        // The accented query spelling folds onto the same key (case-fold kept).
        QCOMPARE(mentionOrder(NativeController::rankMentionSuggestions(QStringLiteral("ЕЛЁНА"), users, {})),
            (QStringList{elena}));
    }

    void mentionRankRowsCarryDisambiguatedVisibleText()
    {
        // message-composer.ui.tsx displayNameCounts: a shared display name gets
        // the "Name (username)" visible form on the ranked row itself, the exact
        // string the send serializer matches; unique names keep the bare name.
        Fixture f;
        const auto bobOne = mentionUuid("0b0b"), bobTwo = mentionUuid("18"), ann = mentionUuid("0a55");
        f.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "cassi"}},
            QJsonObject{{"uuid", bobOne}, {"username", "bob"}, {"first_name", "Bob"}, {"last_name", "Ray"}},
            QJsonObject{{"uuid", bobTwo}, {"username", "bob2"}, {"first_name", "Bob"}, {"last_name", "Ray"}},
            QJsonObject{{"uuid", ann}, {"username", "ann"}, {"first_name", "Ann"}}}}; };
        QVERIFY(f.start());
        QMap<QString, QString> visibleByUuid;
        for (const auto &value : f.controller.mentionSuggestions(QString{}))
        {
            const auto row = value.toMap();
            visibleByUuid.insert(row.value("userUuid").toString(), row.value("visibleText").toString());
        }
        QCOMPARE(visibleByUuid.value(bobOne), QStringLiteral("Bob Ray (bob)"));
        QCOMPARE(visibleByUuid.value(bobTwo), QStringLiteral("Bob Ray (bob2)"));
        QCOMPARE(visibleByUuid.value(ann), QStringLiteral("Ann"));
    }

    void mentionUrnsSkipEmailsAndAmbiguousNames()
    {
        const auto bob = mentionUuid("0b0b");
        const auto ivanOne = mentionUuid("16"), ivanTwo = mentionUuid("17");
        const QVector<ComposerMention> single{{bob, QStringLiteral("Bob Reed"), QStringLiteral("Bob Reed")}};
        // message-composer-body.lib.test.ts converts visible names to canonical URNs.
        QCOMPARE(serializeComposerMentions(QStringLiteral("Hi @Bob Reed!"), single),
            QStringLiteral("Hi [Bob Reed](urn:user:%1)!").arg(bob));
        // Emails keep their literal text: the boundary class rejects a word-character prefix.
        QCOMPARE(serializeComposerMentions(QStringLiteral("mail bob@example.com please"), single),
            QStringLiteral("mail bob@example.com please"));
        QCOMPARE(serializeComposerMentions(QStringLiteral("contact me@Bob Reed now"), single),
            QStringLiteral("contact me@Bob Reed now"));
        // Punctuation boundaries convert, longer names do not absorb a prefix match.
        QCOMPARE(serializeComposerMentions(QStringLiteral("(@Bob Reed) and @Bob Reedson"), single),
            QStringLiteral("([Bob Reed](urn:user:%1)) and @Bob Reedson").arg(bob));
        // Ambiguous display names (message-composer.ui.tsx displayNameCounts) never
        // carry the bare visibleText, so a bare mention stays literal.
        const QVector<ComposerMention> ivans{{ivanOne, QStringLiteral("Ivan Ivanov"), QStringLiteral("Ivan Ivanov (ivan.one)")},
            {ivanTwo, QStringLiteral("Ivan Ivanov"), QStringLiteral("Ivan Ivanov (ivan.two)")}};
        QCOMPARE(serializeComposerMentions(QStringLiteral("Hi @Ivan Ivanov!"), ivans),
            QStringLiteral("Hi @Ivan Ivanov!"));
        QCOMPARE(serializeComposerMentions(QStringLiteral("Hi @Ivan Ivanov (ivan.two)!"), ivans),
            QStringLiteral("Hi [Ivan Ivanov](urn:user:%1)!").arg(ivanTwo));
        // Markdown-sensitive display names are escaped inside the label.
        const QVector<ComposerMention> sensitive{{bob, QStringLiteral("A]ice * Smith"), QStringLiteral("A]ice * Smith")}};
        QCOMPARE(serializeComposerMentions(QStringLiteral("@A]ice * Smith hi"), sensitive),
            QStringLiteral("[A\\]ice \\* Smith](urn:user:%1) hi").arg(bob));
    }

    void mentionFrecencyDecaysWithinOwnerAndSurvivesReopen()
    {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        // setOwner keeps only the opaque 64-hex identity scope, like every owner group.
        const QString ownerA = QString(64, QLatin1Char('a'));
        const QString ownerB = QString(64, QLatin1Char('b'));
        const auto picked = mentionUuid("f1");
        const auto foreign = mentionUuid("f2");
        qint64 now = 1'700'000'000'000LL;
        NativePreferences preferences(directory.filePath("mention-frecency.ini"));
        preferences.setNowProvider([&] { return now; });
        QVERIFY(preferences.mentionFrecencyFor(ownerA).isEmpty());
        preferences.recordMentionPick(picked, ownerA);
        preferences.recordMentionPick(picked, ownerA);
        preferences.recordMentionPick(foreign, ownerB);
        // Stores are partitioned per owner scope: B never sees A's picks.
        QCOMPARE(preferences.mentionFrecencyFor(ownerA).size(), 1);
        QVERIFY(!preferences.mentionFrecencyFor(ownerA).contains(foreign));
        QVERIFY(preferences.mentionFrecencyFor(ownerB).contains(foreign));
        QVERIFY(qAbs(preferences.mentionFrecencyFor(ownerA).value(picked).toDouble() - 2.0) < 0.01);
        // Half-life 14 days (mention-frecency.lib.ts): after one half-life the count of 2 is 1.
        now += 14LL * 24 * 60 * 60 * 1000;
        QVERIFY(qAbs(preferences.mentionFrecencyFor(ownerA).value(picked).toDouble() - 1.0) < 0.01);
        NativePreferences reopened(directory.filePath("mention-frecency.ini"));
        reopened.setNowProvider([&] { return now; });
        QVERIFY(reopened.mentionFrecencyFor(ownerA).value(picked).toDouble() > 0.9);
        QVERIFY(!reopened.mentionFrecencyFor(ownerA).contains(foreign));
        // Entries decayed below MIN_KEPT_SCORE 0.05 drop out of the ranking store.
        now += 400LL * 24 * 60 * 60 * 1000;
        QVERIFY(reopened.mentionFrecencyFor(ownerA).isEmpty());
    }

    void mentionSuggestionsFollowLiveDirectoryAndFrecency()
    {
        Fixture f;
        const auto alice = mentionUuid("0a1c"), bob = mentionUuid("0b0b"), boba = mentionUuid("18");
        const auto bea = mentionUuid("0b3a");
        f.users = [&](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "cassi"}},
            QJsonObject{{"uuid", alice}, {"username", "alice"}, {"first_name", "Alice"}},
            QJsonObject{{"uuid", bob}, {"username", "bob"}, {"first_name", "Bob"}, {"last_name", "Ray"}},
            QJsonObject{{"uuid", boba}, {"username", "boba"}, {"first_name", "Boba"}, {"last_name", "Tea"}},
            QJsonObject{{"uuid", bea}, {"username", "bea"}, {"first_name", "Bea"}}}}; };
        f.messages = [&](const Request &request) {
            QMap<int, QJsonObject> rows;
            auto row = message(QStringLiteral("from bob"), QStringLiteral("8a000000-0000-4000-8000-000000000001"));
            row["author_uuid"] = bob; row["is_own"] = false;
            row["created_at"] = QStringLiteral("2026-01-02T10:01:00.000Z");
            rows.insert(1, row);
            row = message(QStringLiteral("from alice"), QStringLiteral("8a000000-0000-4000-8000-000000000002"));
            row["author_uuid"] = alice; row["is_own"] = false;
            row["created_at"] = QStringLiteral("2026-01-02T10:02:00.000Z");
            rows.insert(2, row);
            return historyResponse(rows, request);
        };
        QVERIFY(f.start());
        QVERIFY(f.controller.composerMentionsEnabled());
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NativePreferences preferences(directory.filePath("mention-live.ini"));
        // Mention frecency lives in the creation-owner scope (the loopback-safe
        // identity the controller passes to recordMentionPick).
        preferences.setOwner(f.controller.creationOwnerKey());
        f.controller.setPreferences(&preferences);
        // Alice wrote the newest message (recent author +4000), Bob the older one
        // (+3900); Bea and Boba carry no context and fall to locale order (mention-suggest.lib.ts).
        QCOMPARE(mentionOrder(f.controller.mentionSuggestions(QString{})),
            (QStringList{alice, bob, bea, boba, userId}));
        // Membership is unknown for an ordinary channel: no outsideChannel marking.
        for (const auto &rowValue : f.controller.mentionSuggestions(QString{}))
            QVERIFY(!rowValue.toMap().contains("outsideChannel"));
        // Bob leads on recent-author score; among the context-free "B" candidates a
        // recorded pick lifts Boba over Bea (FRECENCY_SCORE_WEIGHT 60), and the store
        // survives reopening the file.
        QCOMPARE(mentionOrder(f.controller.mentionSuggestions(QStringLiteral("b"))), (QStringList{bob, bea, boba}));
        f.controller.recordMentionPick(boba);
        QCOMPARE(mentionOrder(f.controller.mentionSuggestions(QStringLiteral("b"))), (QStringList{bob, boba, bea}));
        NativePreferences reopened(directory.filePath("mention-live.ini"));
        reopened.setOwner(f.controller.creationOwnerKey());
        f.controller.setPreferences(&reopened);
        QCOMPARE(mentionOrder(f.controller.mentionSuggestions(QStringLiteral("b"))), (QStringList{bob, boba, bea}));
        // The suggestion rows carry the dropdown fields: secondary line prefers
        // @username, presence and avatar are exposed for the row template.
        const auto rows = f.controller.mentionSuggestions(QStringLiteral("ali"));
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value("displayName").toString(), QStringLiteral("Alice"));
        QCOMPARE(rows.first().toMap().value("username").toString(), QStringLiteral("alice"));
        QVERIFY(rows.first().toMap().contains("status"));
        QVERIFY(rows.first().toMap().contains("avatarUrl"));
    }

    void mentionMembersFollowCompletedChannelBindings()
    {
        Fixture f;
        f.users = [](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "viewer"}},
            QJsonObject{{"uuid", channelMemberA}, {"username", "amy"}},
            QJsonObject{{"uuid", channelMemberB}, {"username", "zoe"}}}}; };
        auto binding = creationBinding(channelMemberB, 1);
        binding["stream_uuid"] = streamId;
        f.bindings = [&](const Request &) { return Response{QJsonArray{binding}}; };
        f.messages = [](const Request &) { return Response{QJsonArray{}}; };
        QVERIFY(f.start());
        for (const auto &row : f.controller.mentionSuggestions({}))
            QVERIFY(!row.toMap().contains("outsideChannel"));
        f.controller.refreshChatHeaderMembers();
        QTRY_COMPARE(f.controller.chatHeaderMemberCount(), 1);
        const auto ranked = f.controller.mentionSuggestions({});
        QCOMPARE(mentionOrder(ranked), (QStringList{channelMemberB, channelMemberA, userId}));
        QVERIFY(!ranked[0].toMap().value("outsideChannel").toBool());
        QVERIFY(ranked[1].toMap().value("outsideChannel").toBool());
    }

    void mentionRecentAuthorsUseLastTwoHundredMessages()
    {
        Fixture f;
        f.users = [](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", channelMemberA}, {"username", "anna"}},
            QJsonObject{{"uuid", channelMemberB}, {"username", "zoe"}},
            QJsonObject{{"uuid", userId}, {"username", "viewer"}}}}; };
        QMap<int, QJsonObject> rows;
        for (int i = 1; i <= 210; ++i) {
            auto row = historyMessage(i);
            row["author_uuid"] = i == 10 ? channelMemberB : userId;
            rows.insert(i, row);
        }
        f.messages = [&](const Request &request) { return historyResponse(rows, request); };
        QVERIFY(f.start());
        for (const int count : {100, 150, 200, 210}) {
            f.controller.loadOlder();
            QTRY_COMPARE(f.controller.messages().size(), count);
        }
        const auto ranked = mentionOrder(f.controller.mentionSuggestions({}));
        QVERIFY(ranked.contains(channelMemberA));
        QVERIFY(ranked.contains(channelMemberB));
        QVERIFY(ranked.indexOf(channelMemberA) < ranked.indexOf(channelMemberB));
    }

    void mentionSendPathSerializesCanonicalUrns()
    {
        Fixture f;
        const auto alice = mentionUuid("0a1c");
        f.users = [alice](const Request &) { return Response{QJsonArray{
            QJsonObject{{"uuid", userId}, {"username", "cassi"}},
            QJsonObject{{"uuid", alice}, {"username", "alice"}, {"first_name", "Alice"}}}}; };
        QJsonObject posted, saved;
        auto previousMessages = f.messages;
        f.messages = [&](const Request &request) -> Response {
            if (request.method == "POST") {
                posted = request.body;
                const auto content = request.body.value("payload").toObject().value("content").toString();
                saved = message(content, QStringLiteral("8b000000-0000-4000-8000-000000000001"));
                saved["user_uuid"] = userId;
                return Response{saved, 201};
            }
            if (request.method == "GET" && request.target.path().endsWith(saved.value("uuid").toString())
                && !saved.isEmpty()) return Response{saved};
            return Response{QJsonArray{}};
        };
        QVERIFY(f.start());
        f.controller.setDraft(QStringLiteral("Hi @Alice!"));
        f.controller.send();
        QTRY_VERIFY_WITH_TIMEOUT(!posted.isEmpty(), 4000);
        // The wire body carries the canonical URN while the visible draft kept the name.
        QCOMPARE(posted.value("payload").toObject().value("content").toString(),
            QStringLiteral("Hi [Alice](urn:user:%1)!").arg(alice));
        QTRY_VERIFY_WITH_TIMEOUT(f.controller.draft().isEmpty(), 4000);
    }
};

QTEST_MAIN(ControllerMentionsTest)
#include "native-controller-mentions-test.moc"
