#pragma once

#include "native-test-harness.h"

// Issue #83: the reference fixed system folder UUIDs
// (workspace_ui 61dacef7 messenger-folder-system-type.lib.ts).
inline const QString allChatsFolder = QStringLiteral("00000000-0000-0000-0000-000000000000");
inline const QString personalFolder = QStringLiteral("00000000-0000-0000-0000-000000000001");
inline const QString channelsFolder = QStringLiteral("00000000-0000-0000-0000-000000000002");

// ---- Issue #83: folders rail model (docs/NATIVE_DESKTOP_FOLDERS_RAIL.md) ----
inline QJsonObject folderRailStream(const QString &id, const QString &name)
{
    return QJsonObject{{"uuid", id}, {"project_id", projectId}, {"name", name}, {"encryption", false}};
}
inline QJsonObject folderRailFolder(const QString &uuid, const QString &title, const QString &systemType,
    int unread, int color, const QJsonArray &items = {}, bool camelCase = false)
{
    if (camelCase)
        return QJsonObject{{"uuid", uuid}, {"title", title}, {"systemType", systemType},
            {"unreadCount", unread}, {"backgroundColorValue", color}, {"folderItems", items}};
    return QJsonObject{{"uuid", uuid}, {"title", title}, {"system_type", systemType},
        {"unread_count", unread}, {"background_color_value", color},
        {"folder_items", items}};
}
inline QJsonObject folderRailItem(const QString &stream)
{
    return QJsonObject{{"uuid", "fi-" + stream}, {"folder", allChatsFolder}, {"stream_uuid", stream},
        {"chat_type", "channel"}};
}
inline QStringList folderKeys(const QVariantList &folders)
{
    QStringList keys;
    for (const auto &folder : folders) keys.append(folder.toMap().value("key").toString());
    return keys;
}
inline QStringList sidebarRowIds(const QVariantList &rows)
{
    QStringList ids;
    for (const auto &row : rows) ids.append(row.toMap().value("id").toString());
    return ids;
}
// Boots a controller whose mock serves two streams and a swappable /folders/ answer.
struct FolderRailFixture
{
    MockServer server;
    ApiClient api{nullptr, true};
    Session session{&api};
    NativeController controller{&api, &session};
    Response folders{QJsonArray{}};
    QString customUuid = QStringLiteral("9f9f9f9f-9f9f-4f9f-8f9f-9f9f9f9f9f9f");
    QString streamB = QStringLiteral("8e8e8e8e-8e8e-4e8e-8e8e-8e8e8e8e8e8e");
    FolderRailFixture()
    {
        server.handler = [this](const Request &request) -> Response {
            const auto path = request.target.path();
            if (path.endsWith("/messenger/folders/")) return folders;
            if (path.endsWith("/messenger/streams/"))
                return Response{QJsonArray{folderRailStream(streamId, "Alpha"), folderRailStream(streamB, "Beta")}};
            if (path.endsWith("/messenger/streams/" + streamId)) return Response{folderRailStream(streamId, "Alpha")};
            if (path.endsWith("/messenger/streams/" + streamB)) return Response{folderRailStream(streamB, "Beta")};
            if (path.endsWith("/messenger/stream_topics/")) return Response{QJsonArray{}};
            if (path.endsWith("/server_settings")) return Response{organizationSettingsDto()};
            if (path.endsWith("/users/")) return Response{QJsonArray{}};
            if (path.contains("/messages/")) return Response{QJsonArray{}};
            return sessionResponse(request);
        };
    }
    bool start()
    {
        controller.login(server.origin().toString(), "synthetic-user", "synthetic-password", "");
        if (!QTest::qWaitFor([this] { return controller.projects().size() == 1; }, 2000)) return false;
        controller.openProject(projectId);
        if (!QTest::qWaitFor([this] { return controller.streams().size() == 2; }, 2000)) return false;
        return true;
    }
};
