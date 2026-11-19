#pragma once

#include <QList>
#include <QString>
#include <QUrl>

namespace workspace {

// #87: the pure native counterpart of the Electron shell gate
// isAllowedJitsiDisplayCaptureRequest (workspace_ui 61dacef7
// packages/electron/src/main.ts:996-1020). One entry models one child-frame
// entry of the reference frames subtree: its URL and whether the frame has
// been detached. No WebEngine dependency: the predicate is unit-testable on
// its own and the call surface feeds it the live frame model. See
// docs/NATIVE_DESKTOP_CALL_DISPLAY_CAPTURE.md.
struct DisplayCaptureFrame
{
    QUrl url;
    bool detached = false;
};

// WHATWG origin of a URL (the reference getUrlOrigin, main.ts:1022-1028):
// lowercase scheme and host with the scheme's default port dropped; an empty
// string when the URL is unparseable or has no authority, so a null origin
// can never match a requesting origin.
QString displayCaptureOrigin(const QUrl &url);

// The measured reference semantics, in order (main.ts:996-1020):
// 1. main-frame requests deny before any origin check;
// 2. an unparseable requesting URL denies;
// 3. a requesting scheme other than https denies;
// 4. allow only when a non-detached frame's origin equals the requesting
//    origin; anything else denies.
bool isAllowedDisplayCapture(bool isMainFrame, const QUrl &requestingUrl,
    const QList<DisplayCaptureFrame> &frames);

} // namespace workspace
