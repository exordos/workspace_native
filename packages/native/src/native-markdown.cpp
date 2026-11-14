#include "native-markdown.h"

#include "workspace-reference.h"

#include <QDesktopServices>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFrame>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QUrl>
#include <md4c-html.h>

namespace workspace {
namespace {
class MessageDocument final : public QTextDocument {
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

QString resourceTarget(const QTextCharFormat &format)
{
    return format.isImageFormat() ? format.toImageFormat().name() : format.anchorHref();
}

// Private document metadata never becomes HTML or a resource request.
constexpr int InlineImageReference = QTextFormat::UserProperty + 101;

// Qt rich text marks every <pre> block non-breakable, so one long code or
// exception-trace line paints past the bubble's right edge (issue #131).
// Qt's own exporter writes each exported code line as a <pre> block whose
// style carries the block margins and whose text span carries the monospace
// face, so re-tagging that exported block as a <div> keeps the presentation
// while letting the message view wrap the line. Only Qt-generated markup is
// rewritten: user text is escaped in the export and cannot contain a tag.
QString containedCodeBlocks(const QString &exportedBody)
{
    QString body = exportedBody;
    body.replace(QLatin1String("<pre "), QLatin1String("<div "));
    body.replace(QLatin1String("<pre>"), QLatin1String("<div>"));
    body.replace(QLatin1String("</pre>"), QLatin1String("</div>"));
    return body;
}

QString htmlBody(const QString &html)
{
    const auto body = html.indexOf("<body");
    const auto start = html.indexOf('>', body);
    const auto end = html.lastIndexOf("</body>");
    return start >= 0 && end > start ? containedCodeBlocks(html.mid(start + 1, end - start - 1)) : QString{};
}

QVariantMap textPart(MessageDocument &document, int first, int last)
{
    if (last <= first) return {};
    QTextCursor cursor(&document);
    cursor.setPosition(first); cursor.setPosition(last, QTextCursor::KeepAnchor);
    MessageDocument part;
    part.setResourceProvider([](const QUrl &) { return QVariant{}; });
    part.setHtml(QTextDocumentFragment(cursor).toHtml());
    // The enclosing native Column supplies the inter-part paragraph gap.
    const auto tail = part.lastBlock();
    auto format = tail.blockFormat(); format.setBottomMargin(0);
    QTextCursor(tail).setBlockFormat(format);
    return {{"kind", "text"}, {"html", htmlBody(part.toHtml())}, {"plain", part.toPlainText()}};
}

QVariantList orderedImageParts(MessageDocument &document)
{
    QVariantList parts;
    int first = 0;
    bool hasImage = false;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        QVariantMap image;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (fragment.isValid()) image = fragment.charFormat().property(InlineImageReference).toMap();
            if (!image.isEmpty()) break;
        }
        if (image.isEmpty()) continue;
        const auto before = textPart(document, first, block.position() - 1);
        if (!before.isEmpty()) parts.append(before);
        parts.append(image); hasImage = true;
        first = block.position() + block.length();
    }
    if (!hasImage) return {};
    const auto after = textPart(document, first, document.characterCount() - 1);
    if (!after.isEmpty()) parts.append(after);
    return parts;
}

bool secretFragment(const QTextCharFormat &format)
{
    const auto target = resourceTarget(format);
    return (format.isAnchor() || format.isImageFormat())
        && (isWorkspaceSecretReference(target)
            || isWorkspaceSecretReference(QUrl::fromPercentEncoding(target.toUtf8())));
}
// The renderer refuses bodies above 40000 Unicode code points (and bodies
// with unpaired surrogates): the message contract counts code points, while
// QString::size() counts UTF-16 units, so the unit bound is only a
// pre-filter for the scan. Gallery collection must apply the exact same
// guard, or references hidden inside a body render() would refuse could
// enter the gallery and trigger downloads (issue #203, exo-review finding B).
bool withinMessageDisplayLimit(const QString &markdown)
{
    bool bounded = markdown.size() <= 80000;
    int points = 0;
    for (qsizetype i = 0; bounded && i < markdown.size(); ++i) {
        if (markdown[i].isHighSurrogate()) {
            if (++i == markdown.size() || !markdown[i].isLowSurrogate()) bounded = false;
        } else if (markdown[i].isLowSurrogate()) bounded = false;
        if (++points > 40000) bounded = false;
    }
    return bounded;
}
}

QVariantMap NativeMarkdown::reference(const QString &target) const
{
    if (target.isEmpty() || target.size() > 65536 || target != target.trimmed()
        || target.contains(QRegularExpression("[\\x00-\\x20\\x7f]")))
        return {};
    const auto parsed = parseWorkspaceReference(target);
    return parsed ? parsed->toVariantMap() : QVariantMap{};
}

bool NativeMarkdown::openExternal(const QString &target) const
{
    const auto parsed = reference(target);
    return parsed.value("kind") == "external" && QDesktopServices::openUrl(QUrl(parsed.value("url").toString()));
}

QVariantMap NativeMarkdown::render(const QString &markdown, bool hideMedia)
{
    // The message contract counts Unicode code points, while QString::size()
    // counts UTF-16 units. Bound the scan before accepting surrogate pairs.
    // The gallery collector shares this exact guard (withinMessageDisplayLimit).
    if (!withinMessageDisplayLimit(markdown))
        return {{"html", tr("Message is too large to display.")}, {"plain", tr("Message is too large to display.")}};
    const auto cacheKey = QString::fromLatin1(QCryptographicHash::hash(markdown.toUtf8(), QCryptographicHash::Sha256).toHex())
        + (hideMedia ? QStringLiteral("|media-hidden") : QString());
    if (m_cache.contains(cacheKey))
        return m_cache.value(cacheKey);
    MessageDocument document;
    document.setResourceProvider([](const QUrl &) { return QVariant{}; });
    // Qt's Markdown importer inserts a paragraph for MD_TEXT_BR. Import escaped
    // GFM HTML instead so soft/hard breaks remain <br>, as in the source UI.
    // Both resource denials are installed before importing any generated HTML.
    QByteArray generated;
    const auto input = markdown.toUtf8();
    const auto append = [](const MD_CHAR *text, MD_SIZE size, void *output) {
        static_cast<QByteArray *>(output)->append(text, size);
    };
    if (md_html(input.constData(), MD_SIZE(input.size()), append, &generated,
                MD_DIALECT_GITHUB | MD_FLAG_NOHTML | MD_FLAG_HARD_SOFT_BREAKS, 0) != 0)
        return {{"html", tr("Could not display this message.")}, {"plain", tr("Could not display this message.")}};
    // Adapt only fixed renderer-generated tags. Raw user HTML was escaped by
    // NOHTML above; it cannot create one of these checkbox or code tags.
    generated.replace("<li class=\"task-list-item\"><input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled checked>",
                      "<li class=\"checked\">");
    generated.replace("<li class=\"task-list-item\"><input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled>",
                      "<li class=\"unchecked\">");
    generated.replace("\n</code></pre>", "</code></pre>");
    generated.replace("<del>", "<s>");
    generated.replace("</del>", "</s>");
    document.setDefaultStyleSheet("pre { margin-top: 0px; margin-bottom: 0px; } "
        "ul, ol { margin-top: 0px; margin-bottom: 0px; } "
        "li { margin-top: 6px; margin-bottom: 6px; } "
        "li.checked::marker { content: '\\2612'; } li.unchecked::marker { content: '\\2610'; }");
    document.setHtml(QString::fromUtf8(generated).trimmed());
    // The desktop's message-body classes use no paragraph top margin, 4px
    // between paragraphs, and no trailing margin. Qt's document defaults add
    // font-dependent space on both sides, inflating every native bubble.
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        auto format = block.blockFormat();
        if (format.headingLevel() || format.nonBreakableLines() || block.textList())
            continue;
        QTextCursor cursor(block);
        format.setTopMargin(0);
        format.setBottomMargin(!block.next().isValid() || cursor.currentTable() ? 0 : 4);
        cursor.setBlockFormat(format);
    }
    struct Fragment { int position; int length; QString text; QTextCharFormat format; };
    QVector<Fragment> fragments;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const auto format = fragment.charFormat();
            if (!fragments.isEmpty() && secretFragment(format) && secretFragment(fragments.last().format)
                && resourceTarget(format) == resourceTarget(fragments.last().format)
                && fragment.position() == fragments.last().position + fragments.last().length) {
                fragments.last().length += fragment.length();
            } else {
                fragments.append({fragment.position(), fragment.length(), fragment.text(), format});
            }
        }
    }
    for (auto it = fragments.crbegin(); it != fragments.crend(); ++it) {
        auto format = it->format;
        auto text = it->text;
        // Issue #217: a quote preview that already shows the media thumbnail hides
        // the media token itself (reference hiddenWorkspaceMediaFileUuids), so the
        // renderer drops the fragment instead of printing its alt/"Image" label.
        const auto hiddenTarget = hideMedia
            ? (format.isImageFormat() ? format.toImageFormat().name()
               : (format.isAnchor() ? format.anchorHref() : QString()))
            : QString();
        const auto hiddenKind = hiddenTarget.isEmpty()
            ? QString() : reference(hiddenTarget).value(QLatin1String("kind")).toString();
        if (hideMedia && (hiddenKind == QLatin1String("image") || hiddenKind == QLatin1String("video"))) {
            text = QString();
            format = QTextCharFormat{};
        } else if (secretFragment(format)) {
            text = tr("Encrypted fragment");
            format = QTextCharFormat{};
        } else if (format.isImageFormat()) {
            const auto image = format.toImageFormat();
            const auto label = image.stringProperty(QTextFormat::ImageAltText);
            text = label.isEmpty() ? tr("Image") : label;
            format = QTextCharFormat{};
            auto parsed = reference(image.name());
            if (!parsed.isEmpty()) {
                format.setAnchor(true);
                format.setAnchorHref(image.name());
                const auto block = document.findBlock(it->position);
                QTextCursor location(&document); location.setPosition(it->position);
                // The first media slice admits an image-only root paragraph.
                // Mixed inline, nested and adjacent-image composition keep the
                // existing safe linked-text fallback until their own layouts.
                if (parsed.value("kind") == "image" && it->length == 1
                    && block.text() == QString(QChar::ObjectReplacementCharacter)
                    && block.position() == it->position && !block.textList()
                    && !block.blockFormat().headingLevel() && !block.blockFormat().nonBreakableLines()
                    && block.blockFormat().indent() == 0 && block.blockFormat().leftMargin() == 0
                    && block.blockFormat().rightMargin() == 0 && !location.currentTable()
                    && location.currentFrame() == document.rootFrame()) {
                    parsed.insert("reference", image.name());
                    parsed.insert("label", text);
                    format.setProperty(InlineImageReference, parsed);
                }
            }
        }
        if (format.isAnchor()) {
            const auto parsed = reference(format.anchorHref());
            if (parsed.isEmpty()) {
                format.setAnchor(false);
                format.setAnchorHref({});
                format.setAnchorNames({});
                format.clearProperty(QTextFormat::ForegroundBrush);
                format.setFontUnderline(false);
            } else if (parsed.value("kind") == "external") {
                format.setAnchorHref(parsed.value("url").toString());
            }
        }
        QTextCursor cursor(&document);
        cursor.setPosition(it->position);
        cursor.setPosition(it->position + it->length, QTextCursor::KeepAnchor);
        cursor.insertText(text, format);
    }
    auto html = document.toHtml();
    const auto body = html.indexOf("<body");
    const auto start = html.indexOf('>', body);
    const auto end = html.lastIndexOf("</body>");
    html = start >= 0 && end > start ? containedCodeBlocks(html.mid(start + 1, end - start - 1)) : markdown.toHtmlEscaped();
    const QVariantMap result{{"html", html}, {"plain", document.toPlainText()},
                             {"parts", orderedImageParts(document)}};
    while (m_order.size() >= 128)
        m_cache.remove(m_order.dequeue());
    m_order.enqueue(cacheKey);
    m_cache.insert(cacheKey, result);
    return result;
}

QVariantList NativeMarkdown::mediaReferences(const QString &markdown) const
{
    // Collection runs on the same parser as rendering (md4c GFM ->
    // QTextDocument), matching the reference gallery which walks the parsed
    // message body's file references in document order
    // (workspace-message-list-media.lib.ts:44-66). Code spans and escaped
    // HTML therefore cannot smuggle references into the gallery.
    QVariantList result;
    // The renderer will refuse an over-limit body, so collecting references
    // from one could expose gallery slots (and their downloads) that the
    // message list itself cannot display. Use the renderer's own guard.
    if (markdown.isEmpty() || !withinMessageDisplayLimit(markdown))
        return result;
    QByteArray generated;
    const auto input = markdown.toUtf8();
    const auto append = [](const MD_CHAR *text, MD_SIZE size, void *output) {
        static_cast<QByteArray *>(output)->append(text, size);
    };
    if (md_html(input.constData(), MD_SIZE(input.size()), append, &generated,
                MD_DIALECT_GITHUB | MD_FLAG_NOHTML | MD_FLAG_HARD_SOFT_BREAKS, 0) != 0)
        return result;
    MessageDocument document;
    document.setResourceProvider([](const QUrl &) { return QVariant{}; });
    document.setHtml(QString::fromUtf8(generated).trimmed());
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            const auto format = fragment.charFormat();
            const auto target = format.isImageFormat()
                ? format.toImageFormat().name()
                : (format.isAnchor() ? format.anchorHref() : QString{});
            if (target.isEmpty())
                continue;
            const auto parsed = reference(target);
            const auto kind = parsed.value("kind").toString();
            if (kind == QLatin1String("image") || kind == QLatin1String("video")) {
                // Consumers that render the reference need the original urn back,
                // the parse map only keeps the file uuid (issue #217 thumbnail).
                auto entry = parsed;
                entry.insert(QStringLiteral("reference"), target);
                result.append(entry);
            }
        }
    }
    return result;
}

}
