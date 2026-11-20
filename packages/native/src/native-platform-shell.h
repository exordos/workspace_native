#pragma once

#include <QString>

class QWindow;

namespace workspace {
void setPlatformUnread(QWindow *window, bool unread, bool inCall, const QString &label);
void setPlatformTaskbarProgress(QWindow *window, qreal progress);
void setPlatformAttention(quintptr windowId, bool requested);
}
