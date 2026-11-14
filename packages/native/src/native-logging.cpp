#include "native-logging.h"

namespace workspace {

NativeLogging::NativeLogging(QString rootDir, QObject *parent)
    : QObject(parent), m_rootDir(std::move(rootDir))
{
}

QString NativeLogging::logDirectoryPath() const { return {}; }
QString NativeLogging::filePath() const { return {}; }
QString NativeLogging::rotatedFilePath() const { return {}; }
void NativeLogging::setProcRoot(QString procRoot) { m_procRoot = std::move(procRoot); }
QString NativeLogging::getFilePath() { return {}; }
bool NativeLogging::appendLogLine(const QString &) { return false; }
QVariantMap NativeLogging::memorySnapshot() const { return {}; }
QString NativeLogging::formatMessageLine(QtMsgType, const QString &message) { return message; }
bool NativeLogging::ensureLogDirectory() { return false; }
void NativeLogging::rotateLogsIfNeeded() {}

void installNativeLogHandler(NativeLogging &) {}
void removeNativeLogHandler() {}

} // namespace workspace
