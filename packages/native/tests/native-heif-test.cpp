// Contract tests for the HEIC/HEIF preview detector (issue #638).
// Reference oracle: workspace_ui e0194ff, shared/lib/heic-preview-format.lib.ts
// (isHeicAttachment + hasHeicFileType) and heic-preview.lib.ts caps.
#include "heif-preview.h"
#include "support/heif-test-fixtures.h"

#include <QColor>
#include <QSize>
#include <QStringList>
#include <QTest>

using namespace workspace;

namespace {

void appendBe32(QByteArray &out, quint32 value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.append(char((value >> shift) & 0xff));
}

void appendBe64(QByteArray &out, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.append(char((value >> shift) & 0xff));
}

// Builds an ftyp box: size, 'ftyp', major, 4-byte minor version, compat brands.
// With wideSize set the short size field is 1 and the real box size follows as
// a 64-bit field (brands move to offset 16, matching the reference).
// declaredSize overrides the computed size so malformed boxes can be produced;
// byteLength truncates (or pads with 'z') the physical header.
QByteArray ftypBox(const QByteArray &major, const QList<QByteArray> &compat = {},
    bool wideSize = false, qint64 declaredSize = -1, qint64 byteLength = -1)
{
    const int brandOffset = wideSize ? 16 : 8;
    // 12 bytes = size + 'ftyp' + major; 4 more = minor version. The reference
    // rejects any box shorter than brandOffset + 8, i.e. without room for
    // major + minor version.
    const qint64 size = declaredSize >= 0 ? declaredSize
                                          : qint64(brandOffset + 8 + 4 * compat.size());
    QByteArray header;
    appendBe32(header, wideSize ? 1 : quint32(size));
    header.append("ftyp");
    if (wideSize)
        appendBe64(header, quint64(size));
    header.append(major.left(4));
    appendBe32(header, 0); // minor version
    for (const QByteArray &brand : compat)
        header.append(brand.left(4));
    if (byteLength >= 0) {
        while (header.size() < byteLength)
            header.append('z');
        header.truncate(int(byteLength));
    }
    return header;
}

} // namespace

class NativeHeifTest : public QObject
{
    Q_OBJECT

private slots:
    void mimeFastPath_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<QString>("contentType");
        QTest::addColumn<bool>("expectsHeic");
        // The four reference MIME values hit the fast path regardless of name.
        QTest::newRow("mime-heic") << QString("photo.jpg") << QString("image/heic") << true;
        QTest::newRow("mime-heif") << QString("photo.jpg") << QString("image/heif") << true;
        QTest::newRow("mime-heic-sequence")
            << QString("photo.jpg") << QString("image/heic-sequence") << true;
        QTest::newRow("mime-heif-sequence")
            << QString("photo.jpg") << QString("image/heif-sequence") << true;
        // Parameters are stripped and case folded before the set lookup.
        QTest::newRow("mime-with-parameters")
            << QString("photo.bin") << QString("image/heic; charset=binary") << true;
        QTest::newRow("mime-uppercase") << QString("photo.bin") << QString("IMAGE/HEIF") << true;
        // A present, different MIME vetoes the filename heuristic outright.
        QTest::newRow("mime-png-wins-over-name") << QString("photo.heic") << QString("image/png") << false;
        QTest::newRow("mime-json-wins-over-name") << QString("photo.heif") << QString("application/json") << false;
        // The two binary placeholders fall through to the filename regex.
        QTest::newRow("octet-stream-heic-name") << QString("photo.heic") << QString("application/octet-stream") << true;
        QTest::newRow("binary-octet-stream-heif-name") << QString("photo.heif") << QString("binary/octet-stream") << true;
        QTest::newRow("octet-stream-png-name") << QString("photo.png") << QString("application/octet-stream") << false;
    }

    void mimeFastPath()
    {
        QFETCH(QString, fileName);
        QFETCH(QString, contentType);
        QFETCH(bool, expectsHeic);
        QCOMPARE(heifLooksLikeHeic(fileName, contentType), expectsHeic);
    }

    void fileNameFallback_data()
    {
        QTest::addColumn<QString>("fileName");
        QTest::addColumn<bool>("expectsHeic");
        QTest::newRow("heic-lower") << QString("photo.heic") << true;
        QTest::newRow("heif-lower") << QString("photo.heif") << true;
        QTest::newRow("uppercase") << QString("PHOTO.HEIC") << true;
        QTest::newRow("surrounding-spaces-trimmed") << QString("  photo.HEIC ") << true;
        QTest::newRow("no-dot") << QString("heic") << false;
        QTest::newRow("not-a-suffix") << QString("heic.png") << false;
        QTest::newRow("too-far") << QString("photo.heicx") << false;
        QTest::newRow("empty-name") << QString() << false;
    }

    void fileNameFallback()
    {
        QFETCH(QString, fileName);
        QFETCH(bool, expectsHeic);
        // No MIME at all: only the name regex decides (reference parity).
        QCOMPARE(heifLooksLikeHeic(fileName, QString()), expectsHeic);
    }

    void ftypBrandTable_data()
    {
        QTest::addColumn<QByteArray>("header");
        QTest::addColumn<bool>("expectsHeic");
        // Every brand of the reference HEIC_BRANDS table as major brand.
        for (const QByteArray &brand :
            QList<QByteArray>{ "heic", "heix", "heim", "heis", "hevc", "hevx", "hevm", "hevs" }) {
            QTest::newRow(QString::fromLatin1("major-%1").arg(QString::fromLatin1(brand)).toUtf8().constData())
                << ftypBox(brand) << true;
        }
        // Generic HEIF majors are HEIC unless a compatible brand vetoes them.
        QTest::newRow("major-mif1-alone") << ftypBox("mif1") << true;
        QTest::newRow("major-msf1-alone") << ftypBox("msf1") << true;
        QTest::newRow("major-mif1-compat-heix") << ftypBox("mif1", { "heix" }) << true;
        QTest::newRow("major-mif1-compat-mjpg") << ftypBox("mif1", { "mjpg" }) << true;
        QTest::newRow("major-mif1-compat-jpeg") << ftypBox("mif1", { "jpeg" }) << false;
        QTest::newRow("major-mif1-compat-avif") << ftypBox("mif1", { "avif" }) << false;
        QTest::newRow("major-msf1-compat-avis") << ftypBox("msf1", { "avis" }) << false;
        // Other image formats are never HEIC, whatever their compatible brands.
        QTest::newRow("major-avif") << ftypBox("avif", { "mif1" }) << false;
        QTest::newRow("major-avis") << ftypBox("avis", { "heic" }) << false;
        QTest::newRow("major-jpeg") << ftypBox("jpeg", { "heic" }) << false;
        // A HEIC brand only counts when it lies inside the declared box.
        QTest::newRow("junk-beyond-box-size-ignored")
            << ftypBox("mif1", {}, false, 16, 20) << true;
        QTest::newRow("heic-compat-inside-box")
            << ftypBox("mif1", { "mjpg", "heic" }) << true;
    }

    void ftypBrandTable()
    {
        QFETCH(QByteArray, header);
        QFETCH(bool, expectsHeic);
        QCOMPARE(heifSniffFtyp(header), expectsHeic);
    }

    void ftypHeaderBounds_data()
    {
        QTest::addColumn<QByteArray>("header");
        QTest::addColumn<bool>("expectsHeic");
        QTest::newRow("empty") << QByteArray() << false;
        QTest::newRow("under-16-bytes") << ftypBox("heix", {}, false, 16, 12) << false;
        QTest::newRow("no-ftyp-magic")
            << QByteArray("\0\0\0\x18" "meta" "heix" "heix", 16) << false;
        QTest::newRow("declared-size-past-header") << ftypBox("heix", {}, false, 100, 20) << false;
        QTest::newRow("declared-size-not-multiple-of-4")
            << ftypBox("heix", {}, false, 22, 24) << false;
        QTest::newRow("box-size-smaller-than-brands")
            << ftypBox("heix", {}, false, 8, 16) << false;
        QTest::newRow("exact-minimal-heix") << ftypBox("heix", {}, false, 16, 16) << true;
        // 64-bit size box (size == 1): brands live at offset 16.
        QTest::newRow("wide-heix") << ftypBox("heix", { "mif1" }, true) << true;
        QTest::newRow("wide-truncated") << ftypBox("heix", { "mif1" }, true, 1000, 20) << false;
        QTest::newRow("wide-size-not-multiple-of-4")
            << ftypBox("heix", {}, true, 22, 24) << false;
        // Only the first 4096 bytes are inspected; a long header keeps working.
        QTest::newRow("padded-to-limit")
            << ftypBox("heix", {}, false, 4096, 4096) << true;
        QTest::newRow("box-past-limit-ignored")
            << ftypBox("heix", {}, false, 8000, 4096) << false;
    }

    void ftypHeaderBounds()
    {
        QFETCH(QByteArray, header);
        QFETCH(bool, expectsHeic);
        QCOMPARE(heifSniffFtyp(header), expectsHeic);
    }

    void sniffIgnoresBytesBeyondHeaderLimit()
    {
        QByteArray header = ftypBox("heix");
        QVERIFY(header.size() < 4096);
        // Extra bytes after the inspected window cannot flip the verdict.
        QByteArray padded = header + QByteArray(8192, 'z');
        QCOMPARE(heifSniffFtyp(padded), true);
    }

    void capsConstants()
    {
        QCOMPARE(kHeifMaxFileBytes, qint64(25) * 1024 * 1024);
        QCOMPARE(kHeifDecodeTimeoutMs, 15000);
        QCOMPARE(kHeifFtypHeaderLimit, 4096);
        // Decode budget cap (#670 finding 3): covers 48 MP photos with margin
        // while keeping the worst-case RGBA canvas bounded (50 MP * 4 B).
        QCOMPARE(kHeifMaxDecodePixels, qint64(50) * 1000 * 1000);
    }

    // The reference app's own e2e fixture (workspace_ui e0194ff
    // e2e/mocks/fixtures/solid-color.heic, sha256 2a210a049069363a..., 425 B):
    // a solid #4682b4 48x32 image. Embedded as base64 so the group needs no
    // file I/O and the bytes are review-inspectable.
    // Also pins the sniff/DECODE split: a corrupt-payload file with a valid
    // ftyp header still SNIFFS as HEIC (libheif does not validate the payload
    // at container level), so the surfaces decide via the decode verdict, not
    // the sniff - same split as the reference isHeicAttachment vs previewFailed.
    void decodeReferenceFixture()
    {
        QCOMPARE(heifSniffFtyp(heicFixture()), true);
        QImage image;
        QString error;
        QVERIFY2(heifDecodeRgba(heicFixture(), image, error), qPrintable(error));
        QCOMPARE(image.size(), QSize(48, 32));
        QCOMPARE(image.format(), QImage::Format_RGBA8888);
        QCOMPARE(image.pixelColor(0, 0), QColor(0x46, 0x82, 0xb4));
        QCOMPARE(image.pixelColor(47, 31), QColor(0x46, 0x82, 0xb4));
    }

    // #680 fix round: the cooperative cancel hook must actually gate the
    // decoder. A pre-set cancel stops the decode before the library is even
    // entered (the deterministic half of the contract); the same hook is also
    // handed to libheif's heif_decoding_options::cancel_decoding so a cancel
    // that arrives mid-decode can abort inside the library's checkpoints.
    void decodeCancelHookStopsBeforeLibrary()
    {
        int hookCalls = 0;
        const auto alwaysCancelled = [](void *user) {
            ++(*static_cast<int *>(user));
            return true;
        };
        QImage image;
        QString error;
        QCOMPARE(heifDecodeRgba(heicFixture(), image, error,
            kHeifMaxDecodePixels, HeifCancelHook{alwaysCancelled, &hookCalls}), false);
        QVERIFY(image.isNull());
        QVERIFY2(error.contains(QStringLiteral("cancelled")), qPrintable(error));
        QVERIFY(hookCalls >= 1);
        // Control: without the hook the same fixture still decodes.
        QVERIFY2(heifDecodeRgba(heicFixture(), image, error), qPrintable(error));
        QVERIFY(!image.isNull());
    }

    // The flipped bytes sit in the ancillary 'ispe' box, not in mdat: libheif
    // (like the reference's bundled decoder) ignores that and still finds a
    // HEIC container, so the surfaces must route on the decode verdict and the
    // sniff result is only the metadata fast path.
    void corruptAncillaryBytesStillSniffAsHeic()
    {
        QCOMPARE(heifSniffFtyp(heicCorruptFixture()), true);
    }

    void decodeRejectsCorruptInput_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::newRow("truncated") << heicTruncatedFixture();
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not-an-image") << QByteArray(4096, 'z');
        QTest::newRow("non-heic-valid-utf8")
            << QByteArray("a valid utf-8 text payload but no ISO-BMFF container anywhere in sight.");
    }

    void decodeRejectsCorruptInput()
    {
        QFETCH(QByteArray, bytes);
        QImage image;
        QString error;
        QVERIFY(!heifDecodeRgba(bytes, image, error));
        QVERIFY(image.isNull());
        QVERIFY(!error.isEmpty());
    }

    void decodeRejectsOversizeWithoutLibrary()
    {
        // A byte blob above the 25 MiB cap must be refused up front (reference
        // MAX_FILE_BYTES); this never reaches libheif, so the blob content is
        // irrelevant and cannot leak into the digest.
        const QByteArray oversize = heicFixture() + QByteArray(int(kHeifMaxFileBytes), 'z');
        QImage image;
        QString error;
        QVERIFY(!heifDecodeRgba(oversize, image, error));
        QVERIFY(image.isNull());
    }

    // #670 review finding 3: ordinary high-resolution HEIC photos (a 48 MP
    // iPhone Pro frame is 8064x6048 = 48.8 MP) must stay inside the decode
    // budget. The budget predicate is the exact gate heifDecodeRgba applies
    // to the handle dimensions before materialising the canvas (a host with
    // no HEVC encoder cannot produce a real 48 MP fixture, and libheif takes
    // the grid from the coded SPS, not from patchable container metadata -
    // probed on libheif 1.21.2 - so the contract pins predicate + path).
    void budgetAcceptsFortyEightMegapixelPhoto()
    {
        QString error;
        QVERIFY(!heifPixelBudgetRejects(8064, 6048, kHeifMaxDecodePixels, error));
        QVERIFY(error.isEmpty());
    }

    void budgetRejectsAboveCap()
    {
        QString error;
        QVERIFY(heifPixelBudgetRejects(9000, 8000, kHeifMaxDecodePixels, error)); // 72 MP
        QVERIFY(error.contains(QLatin1String("above pixel budget")));
    }

    // The live decode path honours the budget: forcing a tiny budget on the
    // real fixture must refuse BEFORE any canvas is materialised.
    void decodePathAppliesBudgetBeforeDecode()
    {
        QImage image;
        QString error;
        QVERIFY(!heifDecodeRgba(heicFixture(), image, error, 1000)); // 48x32 = 1536 px
        QVERIFY(error.contains(QLatin1String("above pixel budget")));
        QVERIFY(image.isNull());
    }
};

QTEST_GUILESS_MAIN(NativeHeifTest)

#include "native-heif-test.moc"
