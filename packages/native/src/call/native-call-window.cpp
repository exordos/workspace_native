#include "call/native-call-window.h"

// Intentionally minimal stub: behavior lands with the implementation commit.
namespace workspace {

JitsiCallWindowController::JitsiCallWindowController(QObject *parent) : QObject(parent) {}

QString JitsiCallWindowController::allowPolicy() { return {}; }
QRect JitsiCallWindowController::expandedBounds(const QSize &) { return {}; }
QRect JitsiCallWindowController::defaultPipBounds(const QSize &) { return {}; }
QRect JitsiCallWindowController::bounds() const { return {}; }
int JitsiCallWindowController::pollIntervalMs() const { return 0; }
QVariantMap JitsiCallWindowController::sessionConfig() const { return {}; }
QString JitsiCallWindowController::displayName() const { return {}; }
QString JitsiCallWindowController::callName() const { return {}; }
QString JitsiCallWindowController::meetingUrl() const { return {}; }
QString JitsiCallWindowController::parsedDomain() const { return {}; }
QString JitsiCallWindowController::parsedRoomName() const { return {}; }
QString JitsiCallWindowController::titleText() const { return {}; }
QString JitsiCallWindowController::subtitleText() const { return {}; }
QString JitsiCallWindowController::stateText() const { return {}; }
QString JitsiCallWindowController::retryText() const { return {}; }
QString JitsiCallWindowController::fullscreenLabel() const { return {}; }
QString JitsiCallWindowController::minimizeLabel() const { return {}; }
QString JitsiCallWindowController::minimizeTooltip() const { return {}; }
QString JitsiCallWindowController::closeLabel() const { return {}; }
void JitsiCallWindowController::setViewportSize(const QSize &) {}
void JitsiCallWindowController::setFocused(bool) {}
void JitsiCallWindowController::setPipBounds(const QRect &) {}
void JitsiCallWindowController::open(const QVariantMap &) {}
void JitsiCallWindowController::close() {}
void JitsiCallWindowController::toggleMinimize() {}
void JitsiCallWindowController::toggleFullscreen() {}
void JitsiCallWindowController::requestDismiss() {}
void JitsiCallWindowController::readyToClose() {}
void JitsiCallWindowController::apiLoadSucceeded() {}
void JitsiCallWindowController::apiLoadFailed() {}
void JitsiCallWindowController::retry() {}
void JitsiCallWindowController::setParticipantCountProvider(std::function<int()>) {}
void JitsiCallWindowController::setEmbedFactory(std::function<void()>) {}
void JitsiCallWindowController::participantJoined() {}
void JitsiCallWindowController::participantLeft() {}
void JitsiCallWindowController::pollNow() {}
void JitsiCallWindowController::ensureEmbed() {}

} // namespace workspace
