#include "native-hotkeys.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QKeySequence>
#include <QRegularExpression>
#include <QSet>
#include <optional>

namespace workspace {
namespace {
    struct Definition {
        const char* id;
        const char* binding;
    };
    constexpr Definition catalog[] = { { "navigation.back", "Alt+ArrowLeft" },
        { "navigation.forward", "Alt+ArrowRight" }, { "navigation.messenger", "" }, { "navigation.calendar", "" },
        { "navigation.mail", "" }, { "navigation.calls", "" }, { "navigation.activity", "" },
        { "navigation.folderList", "" }, { "interface.theme", "" }, { "interface.help", "Mod+/" },
        { "interface.sidebar", "Mod+\\" }, { "interface.infoPanel", "Mod+." },
        { "interface.quoteMessage", "Ctrl+L" }, { "composer.editLastMessage", "ArrowUp" },
        { "composer.bold", "Mod+B" }, { "composer.italic", "Mod+I" }, { "composer.strike", "Mod+Shift+X" },
        { "composer.code", "Mod+E" } };
    constexpr Definition fixedKeys[] = { { "fixed.cancel", "Escape" }, { "fixed.focus", "Tab" },
        { "fixed.send", "Enter" }, { "fixed.editing", "Mod+C" }, { "fixed.focusZones", "F6" } };
    const Definition* definition(const QString& id)
    {
        for (const auto& item : catalog)
            if (id == QLatin1String(item.id))
                return &item;
        return nullptr;
    }
    const QString unshifted = QStringLiteral("`1234567890-=[]\\;',./");
    const QString shifted = QStringLiteral("~!@#$%^&*()_+{}|:\"<>?");

    struct Chord {
        bool ctrl = false, alt = false, shift = false, meta = false;
        QString key;
        QString text() const
        {
            QStringList parts;
            if (ctrl)
                parts << "Ctrl";
            if (alt)
                parts << "Alt";
            if (shift)
                parts << "Shift";
            if (meta)
                parts << "Meta";
            parts << key;
            return parts.join('+');
        }
    };
    const QMap<QString, QString> keypad = { { "Numpad0", "0" }, { "Numpad1", "1" }, { "Numpad2", "2" },
        { "Numpad3", "3" }, { "Numpad4", "4" }, { "Numpad5", "5" }, { "Numpad6", "6" }, { "Numpad7", "7" },
        { "Numpad8", "8" }, { "Numpad9", "9" }, { "NumpadAdd", "+" }, { "NumpadSubtract", "-" },
        { "NumpadMultiply", "*" }, { "NumpadDivide", "/" }, { "NumpadDecimal", "." }, { "NumpadComma", "," },
        { "NumpadEqual", "=" }, { "NumpadEnter", "Enter" } };
    QString namedKey(QString key)
    {
        const QStringList names = { "ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown", "Home", "End", "PageUp",
            "PageDown", "Delete", "Insert", "Backspace", "Escape", "Tab", "Enter", "Space", "IntlBackslash",
            "IntlRo", "IntlYen" };
        for (const auto& name : names)
            if (name.compare(key, Qt::CaseInsensitive) == 0)
                return name;
        for (auto it = keypad.cbegin(); it != keypad.cend(); ++it)
            if (it.key().compare(key, Qt::CaseInsensitive) == 0)
                return it.key();
        static const QRegularExpression functionKey(QStringLiteral("^F([1-9]|[12][0-9]|3[0-5])$"));
        key = key.toUpper();
        if (functionKey.match(key).hasMatch())
            return key;
        if (key.size() == 1
            && ((key[0] >= u'A' && key[0] <= u'Z') || unshifted.contains(key) || shifted.contains(key)))
            return key;
        return { };
    }
    std::optional<Chord> parse(QString value, bool mac)
    {
        if (value.isEmpty() || value.size() > 64 || value.trimmed() != value)
            return { };
        QStringList parts = value.split('+');
        if (value.endsWith('+')) {
            if (parts.size() < 2 || !parts.last().isEmpty() || !parts[parts.size() - 2].isEmpty())
                return { };
            parts.removeLast();
            parts.last() = "+";
        }
        Chord chord;
        chord.key = namedKey(parts.takeLast());
        if (chord.key.isEmpty())
            return { };
        for (auto part : parts) {
            part = part.toLower();
            bool* modifier = nullptr;
            if (part == "mod")
                modifier = mac ? &chord.meta : &chord.ctrl;
            else if (part == "ctrl" || part == "control")
                modifier = &chord.ctrl;
            else if (part == "meta" || part == "cmd")
                modifier = &chord.meta;
            else if (part == "alt")
                modifier = &chord.alt;
            else if (part == "shift")
                modifier = &chord.shift;
            if (!modifier || *modifier)
                return { };
            *modifier = true;
        }
        const auto n = shifted.indexOf(chord.key);
        if (n >= 0) {
            chord.key = unshifted.mid(n, 1);
            chord.shift = true;
        }
        return chord;
    }
#ifdef Q_OS_MACOS
    constexpr bool macPlatform = true;
#else
    constexpr bool macPlatform = false;
#endif
    QString current(const QString& id, const QMap<QString, QString>& overrides)
    {
        const auto* entry = definition(id);
        return entry ? overrides.value(id, QLatin1String(entry->binding)) : QString();
    }
    QString problem(const QString& id, const QString& value, const QMap<QString, QString>& overrides)
    {
        if (!definition(id))
            return "invalid";
        if (value.isEmpty())
            return { };
        const auto native = parse(value, macPlatform);
        if (!native)
            return "invalid";
        const auto& c = *native;
        const QString key = keypad.value(c.key, c.key);
        const bool platform = c.ctrl || c.meta;
        if (QStringList { "Escape", "Tab", "Enter", "F6" }.contains(key)
            || (!platform && !c.alt && !(id == "composer.editLastMessage" && key == "ArrowUp" && !c.shift))
            || (platform && QStringList { "+", "=", "-", "0" }.contains(key))
            || (platform && c.shift && key == "R")
            || (platform && !c.alt && c.ctrl != c.meta && !c.shift && key.size() == 1
                && QStringLiteral("123456789ACFVXLNORPSTWYZQ").contains(key)
                && !(id == "interface.quoteMessage" && c.ctrl && key == "L"
                    && (value.startsWith("Ctrl+", Qt::CaseInsensitive)
                        || value.startsWith("Control+", Qt::CaseInsensitive))))
            || (platform && !c.alt && c.ctrl != c.meta && c.shift && (key == "T" || key == "Z"))
            || (!c.alt
                && ((c.shift && !platform && (key == "Insert" || key == "Delete"))
                    || (platform && !c.shift && c.ctrl != c.meta && key == "Insert")))
            || (c.alt && !platform && !c.shift && key == "F4"))
            return "reserved";
        for (bool mac : { false, true }) {
            const auto candidate = parse(value, mac);
            if (!candidate)
                return "invalid";
            for (const auto& item : catalog) {
                if (id == QLatin1String(item.id))
                    continue;
                const auto other = parse(current(QLatin1String(item.id), overrides), mac);
                if (other && candidate->text() == other->text())
                    return "conflict";
            }
        }
        return { };
    }
} // namespace

NativeHotkeys::NativeHotkeys(QSettings& settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
}
void NativeHotkeys::load(const QString& group)
{
    m_group = group + "/hotkeys";
    m_enabled = m_settings.value(m_group + "/enabled", true).toBool();
    m_overrides.clear();
    for (const auto& entry : catalog) {
        const auto id = QLatin1String(entry.id);
        const auto value = m_settings.value(m_group + "/overrides/" + id);
        if (value.metaType().id() == QMetaType::QString)
            m_overrides.insert(id, value.toString());
    }
    bool removed;
    do {
        removed = false;
        for (const auto& entry : catalog) {
            const auto id = QLatin1String(entry.id);
            if (m_overrides.contains(id) && !problem(id, m_overrides.value(id), m_overrides).isEmpty()) {
                m_overrides.remove(id);
                removed = true;
            }
        }
    } while (removed);
    rebuildBindings();
    setRecording(false);
    emit changed();
}
void NativeHotkeys::rebuildBindings()
{
    m_actionsByChord.clear();
    for (const auto& entry : catalog) {
        const QString id = QLatin1String(entry.id);
        const auto chord = parse(binding(id), macPlatform);
        if (chord)
            m_actionsByChord.insert(chord->text(), id);
    }
}
QString NativeHotkeys::binding(const QString& id) const
{
    return current(id, m_overrides);
}
QString NativeHotkeys::validate(const QString& id, const QString& value) const
{
    return problem(id, value, m_overrides);
}
QVariantList NativeHotkeys::actions() const
{
    QVariantList rows;
    for (const auto& entry : catalog) {
        const QString id = QLatin1String(entry.id);
        rows.append(QVariantMap { { "id", id }, { "category", id.section('.', 0, 0) }, { "binding", binding(id) },
            { "defaultBinding", QLatin1String(entry.binding) }, { "overridden", m_overrides.contains(id) } });
    }
    return rows;
}
QVariantList NativeHotkeys::fixed() const
{
    QVariantList rows;
    for (const auto& entry : fixedKeys)
        rows.append(
            QVariantMap { { "id", QLatin1String(entry.id) }, { "binding", QLatin1String(entry.binding) } });
    return rows;
}
QString NativeHotkeys::write(const QMap<QString, QString>& overrides, bool enabled)
{
    if (m_group.isEmpty())
        return "storageError";
    m_settings.setValue(m_group + "/enabled", enabled);
    m_settings.remove(m_group + "/overrides");
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
        m_settings.setValue(m_group + "/overrides/" + it.key(), it.value());
    m_settings.sync();
    if (m_settings.status() != QSettings::NoError)
        return "storageError";
    m_overrides = overrides;
    m_enabled = enabled;
    rebuildBindings();
    emit changed();
    return { };
}
QString NativeHotkeys::assign(const QString& id, const QString& value)
{
    const auto error = validate(id, value);
    if (!error.isEmpty())
        return error;
    auto next = m_overrides;
    next.insert(id, value);
    return write(next, m_enabled);
}
QString NativeHotkeys::reset(const QString& id)
{
    if (!definition(id))
        return "invalid";
    auto next = m_overrides;
    next.remove(id);
    const auto restored = current(id, next);
    if (!problem(id, restored, next).isEmpty())
        return "conflict";
    return write(next, m_enabled);
}
QString NativeHotkeys::resetAll()
{
    return write({ }, m_enabled);
}
QString NativeHotkeys::setEnabled(bool value)
{
    return write(m_overrides, value);
}
void NativeHotkeys::setRecording(bool value)
{
    if (m_recording != value) {
        m_recording = value;
        emit recordingChanged();
    }
}
void NativeHotkeys::attach(QWindow* window)
{
    if (m_window)
        m_window->removeEventFilter(this);
    m_window = window;
    if (window)
        window->installEventFilter(this);
}

namespace {
    QString logicalKey(int key)
    {
        switch (key) {
        case Qt::Key_Left:
            return "ArrowLeft";
        case Qt::Key_Right:
            return "ArrowRight";
        case Qt::Key_Up:
            return "ArrowUp";
        case Qt::Key_Down:
            return "ArrowDown";
        case Qt::Key_Return:
        case Qt::Key_Enter:
            return "Enter";
        case Qt::Key_Escape:
            return "Escape";
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            return "Tab";
        case Qt::Key_Space:
            return "Space";
        case Qt::Key_PageUp:
            return "PageUp";
        case Qt::Key_PageDown:
            return "PageDown";
        default:
            return namedKey(QKeySequence(key).toString(QKeySequence::PortableText));
        }
    }
    QString physicalKey(const QKeyEvent& event)
    {
        // XCB and Wayland expose XKB codes (evdev + 8). Windows supplies set-1
        // codes, whose unextended alphanumeric positions use the same numbers.
#ifdef Q_OS_MACOS
        static const QMap<int, QString> positions = { { 0, "A" }, { 1, "S" }, { 2, "D" }, { 3, "F" }, { 4, "H" },
            { 5, "G" }, { 6, "Z" }, { 7, "X" }, { 8, "C" }, { 9, "V" }, { 10, "IntlBackslash" }, { 11, "B" },
            { 12, "Q" }, { 13, "W" }, { 14, "E" }, { 15, "R" }, { 16, "Y" }, { 17, "T" }, { 18, "1" },
            { 19, "2" }, { 20, "3" }, { 21, "4" }, { 22, "6" }, { 23, "5" }, { 24, "=" }, { 25, "9" },
            { 26, "7" }, { 27, "-" }, { 28, "8" }, { 29, "0" }, { 30, "]" }, { 31, "O" }, { 32, "U" },
            { 33, "[" }, { 34, "I" }, { 35, "P" }, { 37, "L" }, { 38, "J" }, { 39, "'" }, { 40, "K" },
            { 41, ";" }, { 42, "\\" }, { 43, "," }, { 44, "/" }, { 45, "N" }, { 46, "M" }, { 47, "." },
            { 50, "`" }, { 93, "IntlYen" }, { 94, "IntlRo" } };
        if (event.nativeVirtualKey() || event.nativeModifiers() || event.nativeScanCode()) {
            const auto key = positions.value(event.nativeVirtualKey());
            if (!key.isEmpty())
                return key;
        }
#else
        const auto scan = event.nativeScanCode();
#ifdef Q_OS_WIN
        const int code = int(scan);
#else
        const int code = scan >= 8 ? int(scan) - 8 : -1;
#endif
        if (code >= 2 && code <= 13)
            return QStringLiteral("1234567890-=").mid(code - 2, 1);
        if (code >= 16 && code <= 27)
            return QStringLiteral("QWERTYUIOP[]").mid(code - 16, 1);
        if (code >= 30 && code <= 41)
            return QStringLiteral("ASDFGHJKL;'`").mid(code - 30, 1);
        if (code >= 44 && code <= 53)
            return QStringLiteral("ZXCVBNM,./").mid(code - 44, 1);
        if (code == 43)
            return "\\";
        if (code == 86)
            return "IntlBackslash";
        if (code == 89)
            return "IntlRo";
        if (code == 124)
            return "IntlYen";
        if (code >= 71 && code <= 83 && (event.modifiers() & Qt::KeypadModifier)) {
            const QStringList keys = { "Numpad7", "Numpad8", "Numpad9", "NumpadSubtract", "Numpad4", "Numpad5",
                "Numpad6", "NumpadAdd", "Numpad1", "Numpad2", "Numpad3", "Numpad0", "NumpadDecimal" };
            return keys[code - 71];
        }
#endif
        const auto key = logicalKey(event.key());
        if (event.modifiers() & Qt::KeypadModifier) {
            for (auto it = keypad.cbegin(); it != keypad.cend(); ++it)
                if (it.value() == key)
                    return it.key();
        }
        return key;
    }
}
QString NativeHotkeys::fromEvent(const QKeyEvent& event)
{
    const auto modifiers = event.modifiers();
    if (event.isAutoRepeat() || (modifiers & Qt::GroupSwitchModifier) || event.key() == Qt::Key_AltGr
        || event.key() == Qt::Key_unknown || event.key() == Qt::Key_Shift || event.key() == Qt::Key_Alt
        || event.key() == Qt::Key_Control || event.key() == Qt::Key_Meta || event.key() == Qt::Key_Multi_key
        || (event.key() >= Qt::Key_Dead_Grave && event.key() <= Qt::Key_Dead_Longsolidusoverlay))
        return { };
    auto key = physicalKey(event);
    if (key.isEmpty())
        return { };
    bool ctrl = modifiers.testFlag(Qt::ControlModifier), meta = modifiers.testFlag(Qt::MetaModifier);
#ifdef Q_OS_MACOS
    if (!QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta))
        std::swap(ctrl, meta);
#endif
    Chord chord { ctrl, modifiers.testFlag(Qt::AltModifier), modifiers.testFlag(Qt::ShiftModifier), meta, key };
    const auto parsed = parse(chord.text(), macPlatform);
    return parsed ? parsed->text() : QString();
}
bool NativeHotkeys::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_window || (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress))
        return false;
    const auto& key = static_cast<const QKeyEvent&>(*event);
    if (!m_recording && (!m_enabled || (key.modifiers() == Qt::NoModifier && key.key() != Qt::Key_Up)))
        return false;
    const auto value = fromEvent(key);
    if (value.isEmpty())
        return false;
    if (m_recording) {
        event->accept();
        if (event->type() == QEvent::KeyPress) {
            if (key.key() == Qt::Key_Escape)
                emit recordingCancelled();
            else
                emit recorded(value);
        }
        return true;
    }
    const auto match = m_actionsByChord.constFind(value);
    if (match == m_actionsByChord.cend())
        return false;
    QVariant accepted;
    const bool activate = event->type() == QEvent::KeyPress;
    const bool invoked = QMetaObject::invokeMethod(m_window, "handleAppShortcut", Qt::DirectConnection,
        Q_RETURN_ARG(QVariant, accepted), Q_ARG(QVariant, QVariant(match.value())),
        Q_ARG(QVariant, QVariant(activate)));
    if (!invoked || !accepted.toBool())
        return false;
    event->accept();
    return true;
}
} // namespace workspace
