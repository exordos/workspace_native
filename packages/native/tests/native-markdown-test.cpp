#include "native-markdown.h"
#include "mock-server.h"

#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextOption>

using namespace workspace;
using namespace workspace::test;

class NativeMarkdownTest final : public QObject {
    Q_OBJECT
private slots:
    void messageDisplayLimitCountsUnicodeCodePoints_data()
    {
        QTest::addColumn<QString>("markdown");
        QTest::addColumn<int>("points");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("ascii-40000") << QString(40000, QLatin1Char('a')) << 40000 << true;
        const char32_t scalar = 0x1f642;
        QTest::newRow("astral-39988-fits-encrypted-wire")
            << QString::fromUcs4(&scalar, 1).repeated(39988) << 39988 << true;
        QTest::newRow("ascii-40001-rejected") << QString(40001, QLatin1Char('a')) << 40001 << false;
    }

    void messageDisplayLimitCountsUnicodeCodePoints()
    {
        QFETCH(QString, markdown); QFETCH(int, points); QFETCH(bool, accepted);
        QCOMPARE(markdown.toUcs4().size(), points);
        if (points == 39988) {
            QCOMPARE(markdown.size(), 79976);
            // Source envelope: 32-byte salt + UTF-8 text + 16-byte tag.
            // This is exactly the existing 213336-character HTTP wire cap.
            QCOMPARE(4 * ((markdown.toUtf8().size() + 48 + 2) / 3), 213336);
        }
        NativeMarkdown renderer;
        const auto rendered = renderer.render(markdown);
        if (accepted) {
            QVERIFY2(rendered.value("plain").toString() == markdown,
                "Valid codepoint-bounded text must remain visible/copyable, rather than diagnostic text");
            QTextDocument document; document.setHtml(rendered.value("html").toString());
            QVERIFY2(document.toPlainText() == markdown, "The native rich-text document must retain the complete valid text");
        } else {
            QVERIFY(rendered.value("plain").toString() != markdown);
            QVERIFY(rendered.value("plain").toString().size() <= 40000);
            QVERIFY(rendered.value("html").toString().size() <= 40000);
        }
    }

    void softAndHardLineBreaksStayInsideTheirParagraph_data()
    {
        QTest::addColumn<QString>("markdown");
        QTest::newRow("soft") << QString("**First line**\nSecond _line_");
        QTest::newRow("hard") << QString("**First line**  \nSecond _line_");
        QTest::newRow("crlf") << QString("**First line**\r\nSecond _line_");
    }
    void softAndHardLineBreaksStayInsideTheirParagraph()
    {
        QFETCH(QString, markdown);
        NativeMarkdown renderer;
        const auto html = renderer.render(markdown).value("html").toString();
        QTextDocument document;
        document.setHtml(html);
        QCOMPARE(document.blockCount(), 1);
        QCOMPARE(document.toPlainText(), QString("First line\nSecond line"));
        QCOMPARE(document.begin().blockFormat().bottomMargin(), 0);
        QVERIFY(html.contains("font-weight:700"));
        QVERIFY(html.contains("font-style:italic"));
    }

    void bubbleParagraphSpacingMatchesExistingDesktop()
    {
        NativeMarkdown renderer;
        QTextDocument document;
        document.setHtml(renderer.render("First paragraph\n\nSecond paragraph").value("html").toString());
        const auto first = document.begin();
        const auto second = first.next();
        QVERIFY(second.isValid());
        QCOMPARE(first.blockFormat().topMargin(), 0);
        QCOMPARE(first.blockFormat().bottomMargin(), 4);
        QCOMPARE(second.blockFormat().topMargin(), 0);
        QCOMPARE(second.blockFormat().bottomMargin(), 0);
    }

    void lineBreakCorrectionPreservesTaskMarkersAndCodeLines()
    {
        NativeMarkdown renderer;
        const auto rendered = renderer.render("- [x] done\n- [ ] todo\n\n```cpp\nint x=1;\nreturn x;\n```\n\nFinal\nparagraph");
        QTextDocument document;
        document.setHtml(rendered.value("html").toString());
        const auto checked = document.begin(), unchecked = checked.next();
        QVERIFY(checked.textList());
        QVERIFY(unchecked.textList());
        QCOMPARE(checked.blockFormat().marker(), QTextBlockFormat::MarkerType::Checked);
        QCOMPARE(unchecked.blockFormat().marker(), QTextBlockFormat::MarkerType::Unchecked);
        const auto code = unchecked.next();
        // #131: exported code lines must stay breakable inside the bubble;
        // each source line remains its own wrappable block.
        QVERIFY(!code.blockFormat().nonBreakableLines());
        QCOMPARE(code.text(), QString("int x=1;"));
        QCOMPARE(code.next().text(), QString("return x;"));
        QCOMPARE(code.next().next().text(), QString("Final\u2028paragraph"));
        QCOMPARE(rendered.value("plain").toString(), QString("done\ntodo\nint x=1;\nreturn x;\nFinal\nparagraph"));
    }

    void longCodeLinesAndUnbrokenTokensStayInsideTheBubbleWidth()
    {
        // #131 regression: a long monospace exception trace painted past the
        // bubble's right edge. Qt rich text never breaks a non-breakable line
        // (Text.Wrap already maps to the word-boundary-or-anywhere mode), so
        // every rendered block must be breakable and must lay out inside the
        // width the message view provides.
        NativeMarkdown renderer;
        const QString trace = QStringLiteral(
            "Unknown storage exception: LockNotAvailable('canceling statement due to lock timeout') "
            "CONTEXT: process_00_lockwait AAAAAAAAAAAAAAAAA______________________________________LONGTOKEN______________________________________END");
        struct Case { const char *name; QString markdown; };
        const QList<Case> cases{
            {"fenced-trace", QStringLiteral("```text\n") + trace + QStringLiteral("\nCONTEXT: retry deadline exceeded\n```")},
            {"indented-trace", QStringLiteral("    ") + trace},
            {"paragraph-token", QStringLiteral("See ") + trace + QStringLiteral(" above.")},
            {"mixed-prose", QStringLiteral("Startup failed:\n\n```text\n") + trace
                + QStringLiteral("\n```\n\nPlease retry.")}
        };
        const qreal bubbleTextWidth = 320;
        for (const auto &entry : cases) {
            const auto rendered = renderer.render(entry.markdown);
            QVERIFY2(rendered.value("plain").toString().contains("LockNotAvailable"), entry.name);
            QTextDocument document;
            document.setDocumentMargin(0);
            document.setHtml(rendered.value("html").toString());
            for (auto block = document.begin(); block.isValid(); block = block.next())
                QVERIFY2(!block.blockFormat().nonBreakableLines(),
                    QStringLiteral("%1: exported line must stay breakable: %2")
                        .arg(entry.name, block.text()).toUtf8().constData());
            QTextOption option;
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            document.setDefaultTextOption(option);
            document.setTextWidth(bubbleTextWidth);
            QVERIFY2(document.size().width() <= bubbleTextWidth + 2.0,
                QStringLiteral("%1: wrapped layout width %2 exceeds %3: %4")
                    .arg(entry.name).arg(document.size().width()).arg(bubbleTextWidth)
                    .arg(rendered.value("html").toString()).toUtf8().constData());
        }
    }

    void secretReferenceLabelsAndEnvelopesNeverEnterRenderedOutput_data()
    {
        QTest::addColumn<QString>("markdown");
        QTest::newRow("unknown-version") << "[DO NOT DISPLAY](urn:secret:v99:synthetic-envelope)";
        QTest::newRow("malformed") << "[DO NOT DISPLAY](urn:secret:malformed)";
        QTest::newRow("formatted-label") << "[**DO NOT** DISPLAY](urn:secret:v1:synthetic-envelope)";
        QTest::newRow("image-label") << "![DO NOT DISPLAY](urn:secret:v99:synthetic-envelope)";
        QTest::newRow("bom-prefixed-target") << QString("[DO NOT DISPLAY](<\ufeffurn:secret:v99:synthetic-envelope>)");
    }

    void secretReferenceLabelsAndEnvelopesNeverEnterRenderedOutput()
    {
        QFETCH(QString, markdown);
        NativeMarkdown renderer;
        const auto output = renderer.render(markdown);
        for (const auto &key : {"html", "plain"}) {
            const auto text = output.value(key).toString();
            QVERIFY(!text.contains("DO NOT"));
            QVERIFY(!text.contains("DISPLAY"));
            QVERIFY(!text.contains("synthetic-envelope"));
            QVERIFY(!text.contains("urn:secret:"));
            QCOMPARE(text.count("Encrypted fragment"), 1);
        }
    }

    void preservesWorkspaceSoftLineBreaksAndFrontMatterText()
    {
        NativeMarkdown renderer;
        QCOMPARE(renderer.render("one\ntwo").value("plain").toString(), "one\ntwo");
        QVERIFY(renderer.render("---\ntitle: keep this message\n---\nBody").value("plain").toString().contains("title: keep this message"));
    }

    void githubFormattingIsNativeRichText()
    {
        NativeMarkdown renderer;
        const auto result = renderer.render("**Strong** and _emphasis_ and ~~removed~~\n\n- first\n- second\n\n```cpp\nint x = 1;\n```\n\n| A | B |\n| --- | --- |\n| 1 | 2 |");
        const auto html = result.value("html").toString();
        QVERIFY(html.contains("font-weight:700"));
        QVERIFY2(html.contains("font-style:italic"), qPrintable(html));
        QVERIFY(html.contains("line-through"));
        QVERIFY(html.contains("<ul"));
        // #131: code blocks are exported as wrappable divs carrying the
        // monospace face, never as non-breakable <pre> blocks.
        QVERIFY(!html.contains("<pre"));
        QVERIFY(html.contains("<div"));
        QVERIFY(html.contains("font-family:'monospace'"));
        QVERIFY(html.contains("<table"));
        QVERIFY(result.value("plain").toString().contains("int x = 1;"));
    }

    void rawHtmlAndUnsafeProtocolsCannotExecuteOrReadResources()
    {
        NativeMarkdown renderer;
        const auto html = renderer.render("<script>alert(1)</script>\n<img src='file:///etc/passwd'>\n\n[local](file:///etc/passwd) [script](javascript:alert) [data](data:text/html,evil) [app](ew://evil)\n\n![secret](file:///etc/passwd) ![remote](https://example.invalid/track.png)").value("html").toString();
        QVERIFY(!html.contains("<script"));
        QVERIFY(!html.contains("<img"));
        QVERIFY(!html.contains("href=\"file:"));
        QVERIFY(!html.contains("href=\"javascript:"));
        QVERIFY(!html.contains("href=\"data:"));
        QVERIFY(!html.contains("href=\"ew:"));
        QVERIFY(html.contains("secret"));
        QVERIFY(html.contains("remote"));
        QVERIFY(html.contains("&lt;script&gt;"));
    }

    void typedWorkspaceReferencesKeepExactIdentity()
    {
        NativeMarkdown renderer;
        const auto parsed = renderer.reference("urn:topic:" + streamId + ":" + topicId);
        QCOMPARE(parsed.value("kind").toString(), "topic");
        QCOMPARE(parsed.value("stream").toString(), streamId);
        QCOMPARE(parsed.value("id").toString(), topicId);
        QCOMPARE(renderer.reference("urn:quote:" + messageId + "?text=hello%20world").value("text").toString(), "hello world");
        const auto html = renderer.render("[Alice](urn:user:" + userId + ") ![screenshot](urn:image:" + messageId + "?name=screen.png)").value("html").toString();
        QVERIFY(html.contains("urn:user:" + userId));
        QVERIFY(html.contains("urn:image:" + messageId));
        QVERIFY(!html.contains("<img"));
        QVERIFY(renderer.reference("urn:topic:" + streamId + ":bad").isEmpty());
        QVERIFY(renderer.reference("urn:user:" + userId + "?token=bad").isEmpty());
        QVERIFY(renderer.reference("urn:quote:" + messageId + "?other=x").isEmpty());
        QVERIFY(renderer.reference("urn:user:not-a-uuid").isEmpty());
    }

    void sourceUrlUrnsOnlyAllowCredentialFreeWebLinks()
    {
        NativeMarkdown renderer;
        QCOMPARE(renderer.reference("urn:url:https://example.invalid/path?q=one%20two").value("kind").toString(), "external");
        QVERIFY(renderer.reference("https://user:password@example.invalid").isEmpty());
        QVERIFY(renderer.reference("urn:url:file:///etc/passwd").isEmpty());
        QVERIFY(renderer.reference("urn:url:https://example.invalid/a b").isEmpty());
        QVERIFY(renderer.reference("https://example.invalid\n").isEmpty());
    }

    void standaloneImagePartsPreserveSafeTextAndCaption()
    {
        NativeMarkdown renderer;
        const auto urn = "urn:image:" + messageId + "?name=sample.png&content_type=image%2Fpng&w=96&h=64&size=230";
        const auto output = renderer.render("**Before** image\n\n![sample](" + urn + ")\n\n_After_ image");
        const auto parts = output.value("parts").toList();
        QCOMPARE(parts.size(), 3);
        QCOMPARE(parts[0].toMap().value("kind").toString(), QString("text"));
        QCOMPARE(parts[0].toMap().value("plain").toString(), QString("Before image"));
        QVERIFY(parts[0].toMap().value("html").toString().contains("font-weight:700"));
        const auto image = parts[1].toMap();
        QCOMPARE(image.value("kind").toString(), QString("image"));
        QCOMPARE(image.value("id").toString(), messageId);
        QCOMPARE(image.value("reference").toString(), urn);
        QCOMPARE(image.value("label").toString(), QString("sample"));
        QCOMPARE(image.value("width").toInt(), 96); QCOMPARE(image.value("height").toInt(), 64);
        QCOMPARE(parts[2].toMap().value("kind").toString(), QString("text"));
        QCOMPARE(parts[2].toMap().value("plain").toString(), QString("After image"));
        QVERIFY(parts[2].toMap().value("html").toString().contains("font-style:italic"));
        for (int i : {0, 2}) {
            QTextDocument document; document.setHtml(parts[i].toMap().value("html").toString());
            QCOMPARE(document.blockCount(), 1);
            QCOMPARE(document.lastBlock().blockFormat().bottomMargin(), qreal(0));
            QVERIFY(!parts[i].toMap().value("html").toString().contains("<img"));
        }
        // Legacy copy/link presentation remains the same safe document.
        QCOMPARE(output.value("plain").toString(), QString("Before image\nsample\nAfter image"));
        QVERIFY(!output.value("html").toString().contains("<img"));
        QVERIFY(output.value("html").toString().contains("urn:image:" + messageId));
        QCOMPARE(renderer.render("**Before** image\n\n![sample](" + urn + ")\n\n_After_ image"), output);
    }

    void imageOnlyPartsHaveNoInventedCaptionOrEmptyTextRun()
    {
        NativeMarkdown renderer;
        const auto parts = renderer.render("![filename.png](urn:image:" + messageId + ")").value("parts").toList();
        QCOMPARE(parts.size(), 1);
        QCOMPARE(parts[0].toMap().value("kind").toString(), QString("image"));
        QCOMPARE(parts[0].toMap().value("label").toString(), QString("filename.png"));
        QVERIFY(!parts[0].toMap().contains("html"));
    }

    void nestedMixedAndUntrustedImagesKeepSafeFallback_data()
    {
        QTest::addColumn<QString>("markdown");
        const auto image = "![image](urn:image:" + messageId + ")";
        QTest::newRow("mixed-inline") << QString("before ") + image + " after";
        QTest::newRow("adjacent-composition") << image + " " + image;
        QTest::newRow("quoted") << QString("> ") + image;
        QTest::newRow("list") << QString("- ") + image;
        QTest::newRow("table") << QString("| A |\n|---|\n| ") + image + " |";
        QTest::newRow("code") << QString("```\n") + image + "\n```";
        QTest::newRow("remote") << QString("![remote](https://example.invalid/track.png)");
        QTest::newRow("local") << QString("![local](file:///does-not-exist.png)");
        QTest::newRow("raw-html") << QString("<img src='urn:image:") + messageId + "'>";
        QTest::newRow("malformed") << QString("![bad](urn:image:not-a-uuid)");
    }
    void nestedMixedAndUntrustedImagesKeepSafeFallback()
    {
        QFETCH(QString, markdown);
        NativeMarkdown renderer;
        const auto result = renderer.render(markdown);
        QVERIFY2(result.value("parts").toList().isEmpty(), qPrintable(result.value("html").toString()));
        QVERIFY(!result.value("html").toString().contains("<img"));
    }

    void secretFragmentsCannotLeakThroughNewImageParts()
    {
        NativeMarkdown renderer;
        const auto output = renderer.render("[**HIDDEN LABEL**](urn:secret:v99:synthetic-envelope)\n\n![image](urn:image:" + messageId + ")\n\nAfter");
        QCOMPARE(output.value("parts").toList().size(), 3);
        const auto serialized = QJsonDocument::fromVariant(output).toJson(QJsonDocument::Compact);
        QVERIFY(!serialized.contains("HIDDEN")); QVERIFY(!serialized.contains("LABEL"));
        QVERIFY(!serialized.contains("urn:secret:")); QVERIFY(!serialized.contains("synthetic-envelope"));
        QVERIFY(serialized.contains("Encrypted fragment"));
    }

    void boundedCacheKeepsFormattingConsistent()
    {
        NativeMarkdown renderer;
        const auto first = renderer.render("**first**");
        for (int i = 0; i < 150; ++i)
            renderer.render(QString::number(i));
        QCOMPARE(renderer.render("**first**"), first);
        QVERIFY(renderer.render(QString(50000, 'a')).value("plain").toString().size() <= 40000);
    }
};

QTEST_MAIN(NativeMarkdownTest)
#include "native-markdown-test.moc"
