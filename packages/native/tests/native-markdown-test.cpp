#include "native-markdown.h"
#include "mock-server.h"

#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

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
        QVERIFY(code.blockFormat().nonBreakableLines());
        QCOMPARE(code.text(), QString("int x=1;"));
        QCOMPARE(code.next().text(), QString("return x;"));
        QCOMPARE(code.next().next().text(), QString("Final\u2028paragraph"));
        QCOMPARE(rendered.value("plain").toString(), QString("done\ntodo\nint x=1;\nreturn x;\nFinal\nparagraph"));
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
        QVERIFY(html.contains("<pre"));
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
