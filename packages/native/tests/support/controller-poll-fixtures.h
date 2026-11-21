#pragma once

#include "native-test-harness.h"

// ---- Issue #214 poll fixtures (docs/NATIVE_DESKTOP_POLLS_FOUNDATION.md) ----
inline const QString pollQuestionOne = "a1a1a1a1-1111-4111-8111-111111111111";
inline const QString pollQuestionTwo = "b2b2b2b2-2222-4222-8222-222222222222";
inline const QString pollOptionOne = "c3c3c3c3-3333-4333-8333-333333333333";
inline const QString pollOptionTwo = "d4d4d4d4-4444-4444-8444-444444444444";
inline const QString pollOptionThree = "e5e5e5e5-5555-4555-8555-555555555555";
inline const QString pollOptionFour = "f6f6f6f6-6666-4666-8666-666666666666";

inline QString pollFixtureUuid(int number)
{
    return QStringLiteral("a5a5a5a5-%1-4a5a-8a5a-a5a5a5a5a5a5").arg(number, 4, 16, QLatin1Char('0'));
}

inline QJsonObject pollPayloadFixture(const QString &questionText = QStringLiteral("Which day?"))
{
    return {{QString("kind"), QStringLiteral("poll")},
        {QString("questions"), QJsonArray{
            QJsonObject{{QString("uuid"), pollQuestionOne}, {QString("text"), questionText},
                {QString("options"), QJsonArray{
                    QJsonObject{{QString("uuid"), pollOptionOne}, {QString("text"), QStringLiteral("Monday")}},
                    QJsonObject{{QString("uuid"), pollOptionTwo}, {QString("text"), QStringLiteral("Tuesday")}}}}},
            QJsonObject{{QString("uuid"), pollQuestionTwo}, {QString("text"), QStringLiteral("Where?")},
                {QString("options"), QJsonArray{
                    QJsonObject{{QString("uuid"), pollOptionThree}, {QString("text"), QStringLiteral("Office")}},
                    QJsonObject{{QString("uuid"), pollOptionFour}, {QString("text"), QStringLiteral("Online")}}}}}}},
        {QString("multiple_answers"), true}, {QString("is_anonymous"), false},
        {QString("allow_revote"), false}, {QString("results_visibility"), QStringLiteral("after_vote")},
        {QString("allow_adding_options"), true}, {QString("closes_at"), QStringLiteral("2026-12-12T09:00:00Z")},
        {QString("closed_at"), QJsonValue::Null}};
}

inline QJsonObject pollStateFixture(qint64 revision, bool closed, bool voted)
{
    QJsonArray votes;
    if (voted) votes.append(QJsonObject{{QString("question_uuid"), pollQuestionOne},
        {QString("option_uuids"), QJsonArray{pollOptionOne}}});
    return {{QString("revision"), revision}, {QString("is_closed"), closed}, {QString("voter_count"), 3},
        {QString("questions"), QJsonArray{
            QJsonObject{{QString("question_uuid"), pollQuestionOne}, {QString("voter_count"), 3},
                {QString("option_counts"), QJsonObject{{pollOptionOne, 1}, {pollOptionTwo, 2}}}},
            QJsonObject{{QString("question_uuid"), pollQuestionTwo}, {QString("voter_count"), 3},
                {QString("option_counts"), QJsonObject{{pollOptionThree, 3}, {pollOptionFour, 0}}}}}},
        {QString("my_votes"), votes}};
}

inline QJsonObject pollMessageDto(const QString &id, qint64 revision, bool closed,
    const QString &questionText = QStringLiteral("Which day?"), bool voted = false)
{
    return {{QString("uuid"), id}, {QString("project_id"), projectId},
        {QString("stream_uuid"), streamId}, {QString("topic_uuid"), topicId},
        {QString("author_uuid"), userId}, {QString("is_own"), true}, {QString("read"), true},
        {QString("starred"), false}, {QString("created_at"), QStringLiteral("2026-01-02T10:00:30.000Z")},
        {QString("payload"), pollPayloadFixture(questionText)},
        {QString("poll_state"), pollStateFixture(revision, closed, voted)}};
}

inline bool pollStateHasMyVote(const QVariantMap &state, const QString &optionUuid)
{
    for (const auto &vote : state.value("myVotes").toList())
        if (vote.toMap().value("optionUuids").toStringList().contains(optionUuid)) return true;
    return false;
}
