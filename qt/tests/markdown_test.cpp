// Tests for qml/markdown.js — the reply splitter and the math scanner.
//
// The file is plain JavaScript with no QML dependency, so it is exercised through a
// QJSEngine rather than by standing up a scene: the questions worth asking are all
// about strings in and structures out.
//
// Worth testing at all because both halves are heuristics over model-authored text
// whose failures are silent and expensive. A fence misjudged mid-stream flickers a
// figure in and out; a `$` misjudged does something worse — it takes everything up to
// the next `$` with it, which during development ate two sections of the help
// document and left a formula where the prose had been.

#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QString>
#include <QtTest>

class MarkdownTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void looksLikeTex_data();
    void looksLikeTex();

    void splitsOurFencesOnly();
    void unclosedFenceStaysProse();
    void nestedFenceIsNotLiftedOut();

    void findsMathInProse();
    void skipsCodeSpans();
    void skipsFencedCode();
    void stopsAtAParagraphBreak();
    void halfStreamedMathIsLeftAsWritten();
    void placeholdersAreInert();

    void helpDocumentSplitsAsExpected();

private:
    QJSEngine engine_;
    QJSValue  md_;                       // the module's exported functions

    QJSValue call(const QString& fn, const QJSValueList& args);
    QStringList kindsOf(const QString& text);
    QJSValue    scan(const QString& text);
    static QString helpPath();
};

//------------------------------------------------------------------------------

void MarkdownTest::initTestCase() {
    // The module lives beside the QML that imports it. `.pragma library` is a QML-JS
    // directive QJSEngine does not know, so it is dropped; nothing else changes.
    const QString path = QStringLiteral(MIROBODY_QT_SOURCE_DIR "/qml/markdown.js");
    QFile f(path);
    QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(QStringLiteral("cannot open ") + path));
    QString source = QString::fromUtf8(f.readAll());
    source.replace(QLatin1String(".pragma library"), QLatin1String(""));

    // The file declares plain functions, so evaluating it in a fresh scope and then
    // collecting them by name is enough -- no module system involved.
    const QJSValue wrapped = engine_.evaluate(
        QStringLiteral("(function () {\n%1\nreturn { splitMessage: splitMessage,"
                       " scanMath: scanMath, looksLikeTex: looksLikeTex,"
                       " prettyJson: prettyJson, escapeHtml: escapeHtml };\n})")
            .arg(source));
    QVERIFY2(!wrapped.isError(), qPrintable(wrapped.toString()));
    md_ = wrapped.call();
    QVERIFY2(!md_.isError(), qPrintable(md_.toString()));
}

QJSValue MarkdownTest::call(const QString& fn, const QJSValueList& args) {
    QJSValue f = md_.property(fn);
    const QJSValue r = f.call(args);
    if (r.isError()) qWarning("%s threw: %s", qPrintable(fn), qPrintable(r.toString()));
    return r;
}

QStringList MarkdownTest::kindsOf(const QString& text) {
    const QJSValue segs = call(QStringLiteral("splitMessage"), {QJSValue(text)});
    QStringList out;
    const int n = segs.property(QStringLiteral("length")).toInt();
    for (int i = 0; i < n; ++i) {
        out << segs.property(i).property(QStringLiteral("kind")).toString();
    }
    return out;
}

QJSValue MarkdownTest::scan(const QString& text) {
    return call(QStringLiteral("scanMath"), {QJSValue(text)});
}

//------------------------------------------------------------------------------
// looksLikeTex -- the one judgement call the clients share, so it is also the one
// place a change here would silently disagree with android and harmony.
//------------------------------------------------------------------------------

void MarkdownTest::looksLikeTex_data() {
    QTest::addColumn<QString>("body");
    QTest::addColumn<bool>("isMath");

    QTest::newRow("a TeX metacharacter decides it")      << "\\alpha"        << true;
    QTest::newRow("superscript")                         << "BMI = w / h^2"  << true;
    QTest::newRow("an ion")                              << "Na^+"           << true;
    QTest::newRow("an operator licenses the space")      << "x + y"          << true;
    QTest::newRow("a price opens with a digit")          << "100 and "       << false;
    QTest::newRow("CJK means the pair spans prose")      << QString::fromUtf8("\xe8\x8a\xb1\xe4\xba\x86 5") << false;
    QTest::newRow("words with no operator are prose")    << "five or "       << false;
    QTest::newRow("a newline is not expression-shaped")  << "a\nb"           << false;
    QTest::newRow("empty")                               << ""               << false;
}

void MarkdownTest::looksLikeTex() {
    QFETCH(QString, body);
    QFETCH(bool, isMath);
    QCOMPARE(call(QStringLiteral("looksLikeTex"), {QJSValue(body)}).toBool(), isMath);
}

//------------------------------------------------------------------------------
// The fence splitter
//------------------------------------------------------------------------------

void MarkdownTest::splitsOurFencesOnly() {
    // A python block is not ours: it stays part of the prose, as a code block.
    QCOMPARE(kindsOf(QStringLiteral("a\n```python\nx = 1\n```\nb")), QStringList{"md"});
    QCOMPARE(kindsOf(QStringLiteral("a\n```svg\n<svg/>\n```")),
             (QStringList{"md", "svg"}));
    QCOMPARE(kindsOf(QStringLiteral("a\n```echarts\n{}\n```\nb")),
             (QStringList{"md", "chart", "md"}));
}

void MarkdownTest::unclosedFenceStaysProse() {
    // Mid-stream: half an SVG is not a figure. It becomes one when the fence closes,
    // and not before -- otherwise every reply with a diagram flickers.
    QCOMPARE(kindsOf(QStringLiteral("before\n```svg\n<svg viewBox=\"0 0 10 10\">")),
             QStringList{"md"});
    QCOMPARE(kindsOf(QStringLiteral("before\n```svg\n<svg/>\n```")),
             (QStringList{"md", "svg"}));
}

void MarkdownTest::nestedFenceIsNotLiftedOut() {
    // A ```svg written INSIDE a ```markdown example is example text, not a figure.
    QCOMPARE(kindsOf(QStringLiteral("```markdown\n```svg\n<svg/>\n```\n```\n")),
             QStringList{"md"});
}

//------------------------------------------------------------------------------
// The math scanner
//------------------------------------------------------------------------------

void MarkdownTest::findsMathInProse() {
    const QJSValue r = scan(QStringLiteral("BMI is $w/h^2$ and sodium is $Na^+$."));
    QCOMPARE(r.property(QStringLiteral("spans")).property(QStringLiteral("length")).toInt(), 2);
    QCOMPARE(r.property(QStringLiteral("spans")).property(0).property(QStringLiteral("tex")).toString(),
             QStringLiteral("w/h^2"));
    QCOMPARE(r.property(QStringLiteral("spans")).property(1).property(QStringLiteral("tex")).toString(),
             QStringLiteral("Na^+"));

    // Display math, delimiters on their own lines -- the shape the help document uses.
    const QJSValue d = scan(QStringLiteral("text\n\n$$\ne = mc^2\n$$\n\nmore"));
    QCOMPARE(d.property(QStringLiteral("spans")).property(QStringLiteral("length")).toInt(), 1);
    QVERIFY(d.property(QStringLiteral("spans")).property(0).property(QStringLiteral("display")).toBool());
}

void MarkdownTest::skipsCodeSpans() {
    // A `$` in a code sample is a dollar sign.
    QCOMPARE(scan(QStringLiteral("run `$HOME` then `$PATH`"))
                 .property(QStringLiteral("spans")).property(QStringLiteral("length")).toInt(), 0);
}

void MarkdownTest::skipsFencedCode() {
    QCOMPARE(scan(QStringLiteral("```sh\necho $PATH and $USER\n```\n"))
                 .property(QStringLiteral("spans")).property(QStringLiteral("length")).toInt(), 0);
    // ...but math after the block is still found.
    QCOMPARE(scan(QStringLiteral("```sh\necho $PATH\n```\nthen $x^2$ ok"))
                 .property(QStringLiteral("spans")).property(QStringLiteral("length")).toInt(), 1);
}

void MarkdownTest::stopsAtAParagraphBreak() {
    // THE regression. Android's `$` handler is an inline processor, so its parser only
    // ever hands it one paragraph; scanning a whole reply as flat text has no such
    // bound. Without one, the `$` before a price searched on until it found another --
    // sections away, inside a code block -- and swallowed everything between.
    const QString prose = QStringLiteral(
        "a test that costs $100 and one that costs $200 stay as prose.\n"
        "\n"
        "### Code\n"
        "\n"
        "```python\n"
        "def bmi(weight_kg, height_m):\n"
        "    return weight_kg / height_m\n"
        "```\n");
    const QJSValue r = scan(prose);
    QCOMPARE(r.property(QStringLiteral("spans")).property(QStringLiteral("length")).toInt(), 0);
    // And the text comes out byte-for-byte, prices and all.
    QCOMPARE(r.property(QStringLiteral("text")).toString(), prose);
}

void MarkdownTest::halfStreamedMathIsLeftAsWritten() {
    // A formula whose closing delimiter has not arrived yet must render as typed, not
    // eat the rest of the message.
    QJSValue r = scan(QStringLiteral("cost is $x"));
    QCOMPARE(r.property(QStringLiteral("spans")).property(QStringLiteral("length")).toInt(), 0);
    QCOMPARE(r.property(QStringLiteral("text")).toString(), QStringLiteral("cost is $x"));

    r = scan(QStringLiteral("$$e = m"));
    QCOMPARE(r.property(QStringLiteral("spans")).property(QStringLiteral("length")).toInt(), 0);
    QCOMPARE(r.property(QStringLiteral("text")).toString(), QStringLiteral("$$e = m"));
}

void MarkdownTest::placeholdersAreInert() {
    // The token stands in for a formula through the markdown -> HTML conversion, so it
    // has to be something no dialect treats as markup and nothing escapes.
    const QJSValue r = scan(QStringLiteral("BMI is $w/h^2$ today."));
    const QJSValue span = r.property(QStringLiteral("spans")).property(0);
    const QString token = span.property(QStringLiteral("token")).toString();
    QVERIFY(!token.isEmpty());
    for (const QChar c : token) {
        QVERIFY2(c.isLetterOrNumber(), qPrintable(QStringLiteral("token not inert: ") + token));
    }
    QCOMPARE(r.property(QStringLiteral("text")).toString(),
             QStringLiteral("BMI is ") + token + QStringLiteral(" today."));
    // The source is kept so an unrendered formula can fall back to it.
    QCOMPARE(span.property(QStringLiteral("raw")).toString(), QStringLiteral("$w/h^2$"));
}

//------------------------------------------------------------------------------
// The real document
//------------------------------------------------------------------------------

QString MarkdownTest::helpPath() {
    return QStringLiteral(MIROBODY_QT_SOURCE_DIR "/../htdoc/static/help/help-en.md");
}

void MarkdownTest::helpDocumentSplitsAsExpected() {
    // `/help` renders this, and it was written to exercise every construct the
    // renderer claims to support -- so it is also the most honest input available.
    QFile f(helpPath());
    QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(QStringLiteral("cannot open ") + helpPath()));
    const QString help = QString::fromUtf8(f.readAll());

    const QStringList kinds = kindsOf(help);
    QCOMPARE(kinds.count(QStringLiteral("svg")), 1);
    QCOMPARE(kinds.count(QStringLiteral("chart")), 1);

    // Every formula in it, and nothing from its Python block or its prices.
    const QJSValue segs = call(QStringLiteral("splitMessage"), {QJSValue(help)});
    QStringList formulas;
    const int n = segs.property(QStringLiteral("length")).toInt();
    for (int i = 0; i < n; ++i) {
        const QJSValue seg = segs.property(i);
        if (seg.property(QStringLiteral("kind")).toString() != QLatin1String("md")) continue;
        const QJSValue spans =
            scan(seg.property(QStringLiteral("text")).toString()).property(QStringLiteral("spans"));
        const int m = spans.property(QStringLiteral("length")).toInt();
        for (int j = 0; j < m; ++j) {
            formulas << spans.property(j).property(QStringLiteral("tex")).toString();
        }
    }

    QVERIFY(!formulas.isEmpty());
    QVERIFY(formulas.contains(QStringLiteral("BMI = w / h^2")));
    QVERIFY(formulas.contains(QStringLiteral("Na^+")));
    for (const QString& tex : formulas) {
        QVERIFY2(!tex.contains(QStringLiteral("height_m")),
                 qPrintable(QStringLiteral("a formula reached into the code block: ") + tex));
        QVERIFY2(!tex.contains(QStringLiteral("100 and")),
                 qPrintable(QStringLiteral("a price was read as math: ") + tex));
    }
}

QTEST_MAIN(MarkdownTest)
#include "markdown_test.moc"
