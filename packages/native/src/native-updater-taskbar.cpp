#include "native-updater-taskbar.h"

#include "native-updater.h"

#include <QWindow>

#include <cmath>

namespace workspace {

// Stub: the forwarding is intentionally not implemented yet.
UpdaterTaskbarProgress::UpdaterTaskbarProgress(QObject *parent)
    : QObject(parent)
{
}

qreal UpdaterTaskbarProgress::hintFor(qreal progress)
{
    return progress;
}

void UpdaterTaskbarProgress::attach(QWindow *, NativeUpdater *)
{
}

void UpdaterTaskbarProgress::sync()
{
}

void UpdaterTaskbarProgress::setHint(qreal)
{
}

} // namespace workspace
