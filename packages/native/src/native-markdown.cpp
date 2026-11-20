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
#include <QSet>
#include <utility>

namespace workspace {
namespace {
class MessageDocument final : public QTextDocument {
protected:
    QVariant loadResource(int, const QUrl &) override { return {}; }
};

bool markdownHtml(const QString &markdown, QByteArray &html)
{
    const auto input = markdown.toUtf8();
    const auto append = [](const MD_CHAR *text, MD_SIZE size, void *output) {
        static_cast<QByteArray *>(output)->append(text, size);
    };
    return md_html(input.constData(), MD_SIZE(input.size()), append, &html,
        MD_DIALECT_GITHUB | MD_FLAG_NOHTML | MD_FLAG_HARD_SOFT_BREAKS, 0) == 0;
}

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

QVariantMap textPart(MessageDocument &document, int first, int last,
    const QVariantList &codeBlocks, const QVector<int> &codeTails)
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
    QVariantMap result{{"kind", "text"}, {"start", first}, {"html", htmlBody(part.toHtml())}, {"plain", part.toPlainText()}};
    // Issue #110 review r3 (finding 4639247928): the copy targets a media-split
    // body renders per text part. The part document re-imports the exported
    // fragment 1:1 (one element per block), so a block entirely inside this
    // range keeps its shape at (start - first) in the part document.
    QVariantList partBlocks;
    for (qsizetype i = 0; i < codeBlocks.size(); ++i)
    {
        const auto blockMap = codeBlocks.at(i).toMap();
        const auto blockStart = blockMap.value("start").toInt();
        if (blockStart < first || blockStart > last || codeTails.at(i) > last) continue;
        partBlocks.append(QVariantMap{{"start", blockStart - first},
            {"text", blockMap.value("text").toString()}});
    }
    if (!partBlocks.isEmpty()) result.insert("codeBlocks", partBlocks);
    return result;
}

QVariantList orderedImageParts(MessageDocument &document,
    const QVariantList &codeBlocks, const QVector<int> &codeTails)
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
        const auto before = textPart(document, first, block.position() - 1, codeBlocks, codeTails);
        if (!before.isEmpty()) parts.append(before);
        parts.append(image); hasImage = true;
        first = block.position() + block.length();
    }
    if (!hasImage) return {};
    const auto after = textPart(document, first, document.characterCount() - 1, codeBlocks, codeTails);
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

namespace {
// Source ranges are needed only for editing. MD4C decides whether a candidate
// actually is a root-paragraph link/image; regex alone must never admit code,
// escaped markup, nested links or raw HTML. Unrecognised syntax stays editable.
QVariantList editAttachmentTokens(const QString &markdown)
{
    struct Candidate { qsizetype first, last, labelFirst, labelLast; bool image, explicitLink; QVariantMap value; };
    struct Scan {
        QByteArray source; QList<Candidate> candidates; QVariantList result;
        QList<MD_BLOCKTYPE> blocks; int spans = 0; qsizetype cursor = 0, label = -1, after = 0;
        bool active = false, image = false; QString target;
    } scan;
    if (!withinMessageDisplayLimit(markdown)) return {};
    scan.source = markdown.toUtf8();
    static const QRegularExpression token(QStringLiteral(
        R"re((!?)\[((?:\\.|[^\]\\\n])*)\](?:\((<[^>\n]+>|[^\s()]+)(?:[ \t]+(?:"[^"\n]*"|'[^'\n]*'))?[ \t]*\)|\[(?:\\.|[^\]\\\n])*\])?)re"));
    auto matches = token.globalMatch(markdown);
    while (matches.hasNext()) {
        const auto match = matches.next();
        auto target = match.captured(3);
        if (target.startsWith('<') && target.endsWith('>')) target = target.mid(1, target.size() - 2);
        const bool explicitLink = match.capturedStart(3) >= 0;
        const bool image = !match.captured(1).isEmpty();
        QVariantMap value;
        if (explicitLink) {
            const auto parsed = parseWorkspaceReference(target, match.captured(2));
            if (!parsed || (parsed->kind != WorkspaceReferenceKind::File
                && parsed->kind != WorkspaceReferenceKind::Image && parsed->kind != WorkspaceReferenceKind::Video)) continue;
            value = parsed->toVariantMap(); value.insert("reference", target);
        } else if (!image) continue;
        // Reference-style images resolve through MD4C, but cannot be stripped as
        // standalone lines: their definitions belong to the editable body.
        value.insert("standalone", explicitLink);
        value.insert("markdown", match.captured()); value.insert("offset", match.capturedStart());
        value.insert("length", match.capturedLength()); value.insert("imageSyntax", !match.captured(1).isEmpty());
        scan.candidates.append({markdown.left(match.capturedStart()).toUtf8().size(),
            markdown.left(match.capturedEnd()).toUtf8().size(), markdown.left(match.capturedStart(2)).toUtf8().size(),
            markdown.left(match.capturedEnd(2)).toUtf8().size(), image, explicitLink, value});
    }
    MD_PARSER parser{}; parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTML | MD_FLAG_HARD_SOFT_BREAKS;
    parser.enter_block = [](MD_BLOCKTYPE type, void *, void *data) {
        static_cast<Scan *>(data)->blocks.append(type); return 0;
    };
    parser.leave_block = [](MD_BLOCKTYPE, void *, void *data) {
        static_cast<Scan *>(data)->blocks.removeLast(); return 0;
    };
    parser.enter_span = [](MD_SPANTYPE type, void *detail, void *data) {
        auto &state = *static_cast<Scan *>(data);
        if (state.spans++ == 0 && state.blocks == QList<MD_BLOCKTYPE>{MD_BLOCK_DOC, MD_BLOCK_P}
            && (type == MD_SPAN_IMG || type == MD_SPAN_A)) {
            const auto attribute = type == MD_SPAN_IMG ? static_cast<MD_SPAN_IMG_DETAIL *>(detail)->src
                : static_cast<MD_SPAN_A_DETAIL *>(detail)->href;
            state.active = true; state.image = type == MD_SPAN_IMG;
            state.target = QString::fromUtf8(attribute.text, attribute.size);
            state.after = state.cursor; state.label = -1;
        }
        return 0;
    };
    parser.text = [](MD_TEXTTYPE, const MD_CHAR *text, MD_SIZE size, void *data) {
        auto &state = *static_cast<Scan *>(data);
        // Some parser text (e.g. a synthesized soft break) is not source-backed.
        const auto address = reinterpret_cast<quintptr>(text), first = reinterpret_cast<quintptr>(state.source.constData());
        if (address >= first && address + size <= first + quintptr(state.source.size())) {
            const auto offset = qsizetype(address - first);
            if (state.active && state.label < 0) state.label = offset;
            state.cursor = offset + size;
        }
        return 0;
    };
    parser.leave_span = [](MD_SPANTYPE, void *, void *data) {
        auto &state = *static_cast<Scan *>(data);
        if (--state.spans == 0 && state.active) {
            for (const auto &candidate : std::as_const(state.candidates)) {
                if (candidate.image != state.image || (candidate.explicitLink
                    && candidate.value.value("reference").toString() != state.target)) continue;
                const bool atLabel = state.label >= candidate.labelFirst && state.label < candidate.labelLast;
                const bool emptyLabel = state.label < 0 && candidate.labelFirst == candidate.labelLast && candidate.first >= state.after;
                if (!atLabel && !emptyLabel) continue;
                auto value = candidate.value;
                if (!candidate.explicitLink) {
                    const auto label = QString::fromUtf8(state.source.mid(candidate.labelFirst, candidate.labelLast - candidate.labelFirst));
                    const auto parsed = parseWorkspaceReference(state.target, label);
                    if (!parsed || parsed->kind != WorkspaceReferenceKind::Image) continue;
                    value.insert(parsed->toVariantMap()); value.insert("reference", state.target);
                }
                state.result.append(value); state.cursor = candidate.last; break;
            }
            state.active = false;
        }
        return 0;
    };
    if (md_parse(scan.source.constData(), MD_SIZE(scan.source.size()), &parser, &scan) != 0) return {};
    return scan.result;
}
}

QVariantMap NativeMarkdown::editContent(const QString &markdown) const
{
    const auto tokens = editAttachmentTokens(markdown);
    const auto lines = markdown.split('\n');
    QMap<qsizetype, QVariantMap> attachmentLines;
    for (const auto &entry : tokens) {
        auto value = entry.toMap();
        if (!value.value("standalone").toBool()) continue;
        const auto offset = value.value("offset").toLongLong();
        const auto first = offset > 0 ? markdown.lastIndexOf('\n', offset - 1) + 1 : 0;
        auto last = markdown.indexOf('\n', offset); if (last < 0) last = markdown.size();
        const auto line = markdown.mid(first, last - first);
        if (line.trimmed() != value.value("markdown").toString()) continue;
        value.insert("markdown", line); attachmentLines.insert(markdown.left(first).count('\n'), value);
    }
    QVariantList leading, trailing; qsizetype first = 0;
    while (first < lines.size() && attachmentLines.contains(first)) leading.append(attachmentLines.value(first++));
    const auto remaining = lines.mid(first).join('\n');
    for (const auto &entry : leading) if (remaining.contains(entry.toMap().value("markdown").toString())) { first = 0; break; }
    auto last = lines.size() - 1;
    while (last >= 0 && lines[last].trimmed().isEmpty()) --last;
    auto end = lines.size();
    while (last >= first && attachmentLines.contains(last)) { trailing.prepend(attachmentLines.value(last)); end = last--; }
    auto text = lines.mid(first, end - first).join('\n');
    if (!trailing.isEmpty()) while (text.endsWith('\n')) text.chop(1);
    const bool paragraphBreak = first > 0 && text.startsWith('\n');
    if (paragraphBreak) text.remove(0, 1);
    QVariantList ordered;
    for (const auto &entry : leading) {
        auto value = entry.toMap();
        value.insert("retained", first > 0);
        if (paragraphBreak) value.insert("paragraphBreak", true);
        ordered.append(value);
    }
    for (const auto &entry : editAttachmentTokens(text)) {
        const auto value = entry.toMap();
        if (value.value("imageSyntax").toBool() && value.value("kind") == "image") ordered.append(value);
    }
    for (const auto &entry : trailing) {
        auto value = entry.toMap(); value.insert("retained", true); ordered.append(value);
    }
    QHash<QString, qsizetype> positions; QVariantList attachments;
    for (const auto &entry : ordered) {
        auto value = entry.toMap(); const auto raw = value.value("markdown").toString();
        if (positions.contains(raw)) {
            if (value.value("retained").toBool()) {
                auto card = attachments[positions.value(raw)].toMap(); card.insert("retained", true);
                attachments[positions.value(raw)] = card;
            }
            continue;
        }
        positions.insert(raw, attachments.size());
        value.remove("offset"); value.remove("length"); value.remove("imageSyntax"); value.remove("standalone");
        attachments.append(value);
    }
    return {{"markdown", text}, {"attachments", attachments}};
}

QString NativeMarkdown::removeEditAttachment(const QString &markdown, const QString &attachment) const
{
    auto result = markdown; const auto tokens = editAttachmentTokens(markdown);
    for (auto it = tokens.crbegin(); it != tokens.crend(); ++it) {
        const auto value = it->toMap();
        if (value.value("markdown").toString() == attachment.trimmed())
            result.remove(value.value("offset").toLongLong(), value.value("length").toLongLong());
    }
    return result;
}

QVariantList NativeMarkdown::updateEditAttachments(const QString &markdown, const QVariantList &attachments)
{
    QVariantList result; QSet<QString> seen;
    for (const auto &entry : attachments) {
        const auto value = entry.toMap();
        if (!value.value("retained").toBool()) continue;
        result.append(value); seen.insert(value.value("markdown").toString().trimmed());
    }
    for (const auto &entry : editAttachmentTokens(markdown)) {
        auto value = entry.toMap(); const auto raw = value.value("markdown").toString();
        if (seen.contains(raw)) continue;
        QVariantMap previous;
        for (const auto &old : attachments) {
            const auto card = old.toMap();
            if (!card.value("retained").toBool() && card.value("markdown").toString().trimmed() == raw
                && card.value("reference") == value.value("reference")) { previous = card; break; }
        }
        // Existing boundary-file cards can remain inline after the extractor's
        // duplicate fallback. Newly typed inline files stay ordinary Markdown.
        if (previous.isEmpty() && !(value.value("imageSyntax").toBool() && value.value("kind") == "image")) continue;
        value.remove("offset"); value.remove("length"); value.remove("imageSyntax"); value.remove("standalone");
        if (!previous.isEmpty()) value.insert("id", previous.value("id"));
        result.append(value); seen.insert(raw);
    }
    return result;
}

QString NativeMarkdown::prependEditAttachments(const QString &markdown, const QVariantList &attachments)
{
    QSet<QString> present;
    for (const auto &entry : editAttachmentTokens(markdown))
        present.insert(entry.toMap().value("markdown").toString());
    QStringList lines;
    bool paragraphBreak = false;
    for (const auto &entry : attachments) {
        const auto card = entry.toMap();
        if (!card.value("retained").toBool()) continue;
        const auto line = card.value("markdown").toString();
        if (line.isEmpty() || lines.contains(line) || present.contains(line.trimmed())) continue;
        lines.append(line);
        paragraphBreak |= card.value("paragraphBreak").toBool();
    }
    if (lines.isEmpty()) return markdown;
    if (markdown.isEmpty()) return lines.join('\n');
    return lines.join('\n') + (paragraphBreak ? QStringLiteral("\n\n") : QStringLiteral("\n")) + markdown;
}

QStringList NativeMarkdown::missingAttachmentLinks(const QString &markdown, const QStringList &links)
{
    QStringList missing;
    QByteArray body;
    for (const auto &link : links) {
        if (markdown.contains(link)) {
            if (body.isEmpty()) markdownHtml(markdown, body);
            QByteArray fragment;
            if (markdownHtml(link, fragment) && fragment.startsWith("<p>") && fragment.endsWith("</p>\n")) {
                // Compare the rendered anchor or image, including inside lists and
                // quotes. Code spans and escaped examples only render its text.
                const auto element = QByteArrayView(fragment).sliced(3, fragment.size() - 8);
                if (!element.isEmpty() && body.contains(element)) continue;
            }
        }
        if (!missing.contains(link)) missing.append(link);
    }
    return missing;
}

bool NativeMarkdown::startsWithParagraph(const QString &markdown, const QString &paragraph)
{
    QByteArray first, full;
    return markdownHtml(paragraph, first) && first.startsWith("<p>") &&
        markdownHtml(markdown, full) && full.startsWith(first);
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
    // The web composer emits standalone attachment images separated by single
    // newlines. Qt imports those as one paragraph, while our media renderer
    // admits image-only paragraphs. Split only verified root-level image lines
    // for display; the stored and transmitted Markdown stays unchanged.
    auto displayMarkdown = markdown;
    if (markdown.contains(QStringLiteral("urn:image:"))) {
        const auto tokens = editAttachmentTokens(markdown);
        for (auto it = tokens.crbegin(); it != tokens.crend(); ++it) {
            const auto value = it->toMap();
            if (!value.value("standalone").toBool() || !value.value("imageSyntax").toBool()
                || value.value("kind").toString() != QLatin1String("image")) continue;
            const auto offset = value.value("offset").toLongLong();
            const auto first = offset > 0 ? markdown.lastIndexOf('\n', offset - 1) + 1 : 0;
            auto last = markdown.indexOf('\n', offset); if (last < 0) last = markdown.size();
            if (markdown.mid(first, last - first).trimmed() != value.value("markdown").toString()) continue;
            if (last < markdown.size() && last + 1 < markdown.size() && markdown[last + 1] != QLatin1Char('\n'))
                displayMarkdown.insert(last, QLatin1Char('\n'));
            if (first > 0 && first > 1 && markdown[first - 2] != QLatin1Char('\n'))
                displayMarkdown.insert(first, QLatin1Char('\n'));
        }
    }
    if (!markdownHtml(displayMarkdown, generated))
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
    // Issue #109 (docs/NATIVE_DESKTOP_MENTION_CHIPS.md): the reference renders
    // [label](urn:user:uuid) as a mention chip whose text carries the "@"
    // prefix, while title="silent" suppresses it (workspace_ui 61dacef
    // workspace-message-render.lib.ts:342-352). The Qt HTML import discards
    // the link title, so the token is consumed here on the md4c output and
    // the ordered urn list is exposed to the view as "mentions".
    QStringList mentionUrns;
    QStringList mentionLabels;
    {
        // Issue #109 review F1/F5 (cid 4639284110, cid 4639284135): the
        // original anchor regex rejected HTML tags inside labels, so MD4C
        // output for formatted labels was silently dropped; its overlapping
        // single-character/entity alternatives also backtracked exponentially
        // when a repeated-entity label was followed by a nested tag (1 s of
        // the UI render path for one crafted message in measurement). Anchor
        // recognition is now a linear scan: find the anchor open, validate
        // the urn and optional silent title, then pair the closing tag while
        // tracking only nested <a> depth (non-anchor tags cannot affect it).
        static const QRegularExpression anchorStart(QStringLiteral("<a href=\"urn:user:"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression anchorHead(QStringLiteral(
            "^([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})\""),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression openAnchor(QStringLiteral("<a(?=[\\s/>])"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression closeAnchor(QStringLiteral("</a(?=[\\s/>])"),
            QRegularExpression::CaseInsensitiveOption);
        // The TextEdit shows the parsed plain text, so the "@" prefix decision
        // and the QML run search both need the label with its formatting tags
        // removed and its entities decoded back to the visible characters.
        const auto plainLabel = [](const QString &labelHtml) {
            auto text = labelHtml;
            text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
            text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
            text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
            text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
            text.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
            // Round-4 review N2 (cid 4640681111): the previous loop searched
            // the remaining label for a semicolon for every ampersand, so a
            // long label without semicolons rescanned its suffix O(n^2) on
            // the UI render path. One pass now copies every non-entity byte
            // once and decodes in place. A numeric reference is accepted only
            // when its digits are immediately terminated by a semicolon, with
            // the same full-body validation and (0, 0x10FFFF] range as before.
            QString decoded;
            decoded.reserve(text.size());
            qsizetype at = 0;
            while (at < text.size()) {
                bool handled = false;
                if (text.at(at) == QLatin1Char('&') && at + 2 < text.size()
                    && text.at(at + 1) == QLatin1Char('#')) {
                    qsizetype scan = at + 2;
                    const bool hex = text.at(scan) == QLatin1Char('x') || text.at(scan) == QLatin1Char('X');
                    if (hex) ++scan;
                    const qsizetype first = scan;
                    while (scan < text.size()) {
                        const QChar digit = text.at(scan);
                        const bool within = (digit >= QLatin1Char('0') && digit <= QLatin1Char('9'))
                            || (hex && ((digit >= QLatin1Char('a') && digit <= QLatin1Char('f'))
                                || (digit >= QLatin1Char('A') && digit <= QLatin1Char('F'))));
                        if (!within) break;
                        ++scan;
                    }
                    if (scan > first && scan < text.size() && text.at(scan) == QLatin1Char(';')) {
                        bool ok = false;
                        const uint value = text.mid(at + 2 + (hex ? 1 : 0), scan - first).toUInt(&ok, hex ? 16 : 10);
                        if (ok && value > 0 && value <= 0x10FFFF) {
                            const char32_t plain = static_cast<char32_t>(value);
                            decoded += QString::fromUcs4(&plain, 1);
                            at = scan + 1;
                            handled = true;
                        }
                    }
                }
                if (!handled) {
                    decoded += text.at(at);
                    ++at;
                }
            }
            return decoded;
        };
        QString html = QString::fromUtf8(generated);
        struct Anchor { int start; int length; QString uuid; QString label; };
        QVector<Anchor> anchors;
        int from = 0;
        for (;;) {
            const auto hit = anchorStart.match(html, from);
            if (!hit.hasMatch())
                break;
            // Match the uuid on a short window so the "^" anchor stays valid.
            const auto head = anchorHead.match(html.mid(hit.capturedEnd(), 40));
            if (!head.hasMatch() || head.capturedStart() != 0) { from = hit.capturedStart() + 4; continue; }
            int cursor = hit.capturedEnd() + head.capturedLength();
            bool silent = false;
            if (html.mid(cursor, 15).compare(QStringLiteral(" title=\"silent\""), Qt::CaseInsensitive) == 0) {
                silent = true;
                cursor += 15;
            }
            if (cursor >= html.size() || html.at(cursor) != QLatin1Char('>')) { from = hit.capturedStart() + 4; continue; }
            ++cursor;
            int closeStart = -1;
            int closeEnd = -1;
            int depth = 0;
            for (int p = cursor; p < html.size(); ) {
                const int lt = html.indexOf(QLatin1Char('<'), p);
                if (lt < 0)
                    break;
                const int gt = html.indexOf(QLatin1Char('>'), lt + 1);
                if (gt < 0)
                    break;
                if (closeAnchor.match(html, lt).capturedStart() == lt) {
                    if (depth == 0) { closeStart = lt; closeEnd = gt + 1; break; }
                    --depth;
                } else if (openAnchor.match(html, lt).capturedStart() == lt) {
                    ++depth;
                }
                p = gt + 1;
            }
            if (closeEnd < 0) { from = hit.capturedStart() + 4; continue; }
            auto label = plainLabel(html.mid(cursor, closeStart - cursor));
            if (!silent && !label.startsWith(QLatin1Char('@')))
                label.prepend(QLatin1Char('@'));
            mentionUrns.append(QStringLiteral("urn:user:") + head.captured(1).toLower());
            mentionLabels.append(label);
            anchors.append({static_cast<int>(hit.capturedStart()), static_cast<int>(closeEnd - hit.capturedStart()), head.captured(1).toLower(), label});
            from = closeEnd;
        }
        // Rewriting from the back keeps the earlier anchors' offsets valid.
        // Round-4 review N1 (cid 4640681088): the label is plain text after
        // the decoder above (escaped markup is literal again), so it must be
        // re-escaped here or literal "<b>" / "&" text would import as markup
        // and alter the visible chip text.
        for (auto it = anchors.crbegin(); it != anchors.crend(); ++it)
            html.replace(it->start, it->length,
                QStringLiteral("<a href=\"urn:user:%1\">%2</a>").arg(it->uuid, it->label.toHtmlEscaped()));
        generated = html.toUtf8();
    }
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
    // Issue #110 (docs/NATIVE_DESKTOP_BODY_COPY_AFFORDANCES.md): the bubble
    // mounts a copy button into every fenced block (reference
    // workspace-message-body-interactions.hook.ts:137-214). Fenced code arrives
    // as consecutive nonBreakableLines blocks (the same marker the containedCodeBlocks
    // re-tag targets), and the exporter reproduces each block 1:1, so the start
    // offsets carry over when the message view re-imports the exported html.
    QVariantList codeBlocks;
    QVector<int> codeBlockTails;
    {
        // Each md4c <pre><code> element is one fenced block; the importer
        // turns its N content lines into N nonBreakableLines blocks (an empty
        // fence into none), so the per-element line counts of the generated
        // html partition the collected blocks back into their fenced elements
        // (review r3: adjacent fences would otherwise merge into one target)
        // while the line flag below keeps blank lines' separators (review r3:
        // leading blank lines would otherwise vanish from the copy payload).
        static const QRegularExpression fencePattern(
            QStringLiteral("<pre><code[^>]*>(.*?)</code></pre>"),
            QRegularExpression::DotMatchesEverythingOption);
        QList<int> fenceLines;
        for (auto fence = fencePattern.globalMatch(QString::fromUtf8(generated)); fence.hasNext();)
        {
            const auto content = fence.next().captured(1);
            fenceLines.append(content.isEmpty() ? 0 : content.count(QLatin1Char('\n')) + 1);
        }
        QString pending;
        bool lineCollected = false;
        int start = -1;
        int tail = -1;
        int fenceIndex = 0;
        int fenceRemaining = 0;
        auto flushCodeBlock = [&]() {
            if (start >= 0)
            {
                codeBlocks.append(QVariantMap{{"start", start}, {"text", pending}});
                codeBlockTails.append(tail);
            }
            pending.clear(); lineCollected = false; start = -1; tail = -1;
        };
        for (auto block = document.begin(); block.isValid(); block = block.next())
        {
            if (!block.blockFormat().nonBreakableLines())
            {
                flushCodeBlock(); fenceRemaining = 0; continue;
            }
            while (fenceIndex < fenceLines.size() && fenceLines.at(fenceIndex) == 0) ++fenceIndex;
            if (fenceRemaining == 0)
            {
                flushCodeBlock();
                start = block.position();
                if (fenceIndex < fenceLines.size()) { fenceRemaining = fenceLines.at(fenceIndex); ++fenceIndex; }
                else fenceRemaining = -1; // unmatched non-breakable blocks: one trailing group
            }
            if (fenceRemaining > 0) --fenceRemaining;
            if (lineCollected) pending += QLatin1Char('\n');
            pending += block.text();
            lineCollected = true;
            tail = block.position();
        }
        flushCodeBlock();
    }
    auto html = document.toHtml();
    // Round-4 review N4 (cid 4641051847): plain-text copies of a mention
    // label can appear earlier in the same body, so the view must locate
    // each mention's own anchor span instead of the first text occurrence.
    // The rendered document already carries the anchor identity: report the
    // range of every urn:user anchor fragment in the document plain text,
    // in document order. When the ranges cannot be aligned 1:1 with the
    // ordered urn list (unusual crafted documents), the list stays empty
    // and the view keeps its text search as the fallback.
    QVariantList mentionSpans;
    {
        struct AnchorSpan { int start; int length; QString href; };
        QVector<AnchorSpan> spans;
        for (auto block = document.begin(); block.isValid(); block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (!fragment.isValid())
                    continue;
                const auto format = fragment.charFormat();
                if (!format.isAnchor())
                    continue;
                const auto href = format.anchorHref();
                if (!href.startsWith(QLatin1String("urn:user:")))
                    continue;
                if (!spans.isEmpty() && spans.last().href == href
                    && spans.last().start + spans.last().length == fragment.position()) {
                    spans.last().length += fragment.length();
                    continue;
                }
                spans.append({fragment.position(), fragment.length(), href});
            }
        }
        bool aligned = spans.size() == mentionUrns.size();
        for (qsizetype index = 0; aligned && index < spans.size(); ++index)
            aligned = spans.at(index).href == mentionUrns.at(index);
        if (aligned)
            for (const auto &span : std::as_const(spans))
                mentionSpans.append(QVariantMap{{"start", span.start}, {"length", span.length}});
    }
    const auto body = html.indexOf("<body");
    const auto start = html.indexOf('>', body);
    const auto end = html.lastIndexOf("</body>");
    html = start >= 0 && end > start ? containedCodeBlocks(html.mid(start + 1, end - start - 1)) : markdown.toHtmlEscaped();
    const QVariantMap result{{"html", html}, {"plain", document.toPlainText()},
                             {"parts", orderedImageParts(document, codeBlocks, codeBlockTails)},
                             {"codeBlocks", codeBlocks}, {"mentions", mentionUrns},
                             {"mentionSpans", mentionSpans},
                             // Issue #109 reviews F4/F6: the visible plain text of every
                             // mention occurrence (with the same "@" prefix rules the
                             // anchor got), in document order, so the view can locate the
                             // occurrence inside its laid-out text without re-parsing.
                             {"mentionLabels", mentionLabels}};
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
    // The web composer emits standalone attachment images separated by single
    // newlines. Qt imports those as one paragraph, while our media renderer
    // admits image-only paragraphs. Split only verified root-level image lines
    // for display; the stored and transmitted Markdown stays unchanged.
    auto displayMarkdown = markdown;
    if (markdown.contains(QStringLiteral("urn:image:"))) {
        const auto tokens = editAttachmentTokens(markdown);
        for (auto it = tokens.crbegin(); it != tokens.crend(); ++it) {
            const auto value = it->toMap();
            if (!value.value("standalone").toBool() || !value.value("imageSyntax").toBool()
                || value.value("kind").toString() != QLatin1String("image")) continue;
            const auto offset = value.value("offset").toLongLong();
            const auto first = offset > 0 ? markdown.lastIndexOf('\n', offset - 1) + 1 : 0;
            auto last = markdown.indexOf('\n', offset); if (last < 0) last = markdown.size();
            if (markdown.mid(first, last - first).trimmed() != value.value("markdown").toString()) continue;
            if (last < markdown.size() && last + 1 < markdown.size() && markdown[last + 1] != QLatin1Char('\n'))
                displayMarkdown.insert(last, QLatin1Char('\n'));
            if (first > 0 && first > 1 && markdown[first - 2] != QLatin1Char('\n'))
                displayMarkdown.insert(first, QLatin1Char('\n'));
        }
    }
    const auto input = displayMarkdown.toUtf8();
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
