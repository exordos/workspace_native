#pragma once

#include <QtGlobal>

namespace workspace {

// Bounds one clipboard confirmation's private snapshots. The defaults are
// separate from the existing per-file upload limit.
class ClipboardSnapshotBudget {
public:
    static constexpr qsizetype MaxFiles = 100;
    static constexpr qint64 MaxBytes = 100 * 1024 * 1024;
    explicit ClipboardSnapshotBudget(qsizetype maxFiles = MaxFiles, qint64 maxBytes = MaxBytes)
        : m_maxFiles(maxFiles), m_maxBytes(maxBytes) {}
    bool add(qint64 bytes)
    {
        if (bytes < 0 || m_count >= m_maxFiles || bytes > m_maxBytes - m_bytes) return false;
        ++m_count; m_bytes += bytes;
        return true;
    }
private:
    qsizetype m_maxFiles, m_count = 0;
    qint64 m_maxBytes, m_bytes = 0;
};

} // namespace workspace
