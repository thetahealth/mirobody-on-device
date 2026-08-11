#pragma once

// RenderHost — LaTeX and ECharts rendering through ONE hidden web page
// (render/render.html), plus the Markdown→HTML conversion the reply body is drawn
// from. A port of harmony's core/RenderHost.ets, and it keeps that design: the
// message list itself never holds a web view. Formulas render to SVG files, charts
// to PNG files, and the UI draws plain QML Images from file:// paths — so the list
// scrolls native and the files double as a disk cache across launches.
//
// Why serialized payloads rather than screenshots: the host page hands SVG/PNG back
// as STRINGS (window.renderTex / window.renderChart -> JSON), so nothing depends on
// grabbing a web view's surface, and math stays resolution-independent.
//
// Flow: the QML delegate asks math()/chart()/svg(); a miss queues the job and
// returns {state:"pending"} (the block shows its source meanwhile). When the page
// finishes, the result lands in the cache and `epoch` bumps; the delegate's bindings
// read `epoch`, so the affected messages re-evaluate and find their image. Failures
// are cached too — a formula MathJax rejects would otherwise re-queue on every
// rebuild — but in MEMORY only, so the next launch retries rather than inheriting a
// bad render forever.
//
// Two cache tiers, and the disk one is load-bearing. `cache_` is this session's map,
// capped; `disk_` indexes what previous launches already rendered, built from ONE
// directory listing at construction. Without that index the files would be
// write-only: every cold start would re-render the whole history through the page,
// one formula at a time (the queue is serialized on purpose). The dimensions the
// layout needs cannot be recovered from an SVG/PNG cheaply, so they ride in the FILE
// NAME (<hash>-<w>x<h>.svg|png) and the listing alone rebuilds the index.
//
// Built WITHOUT Qt WebEngine (the MIROBODY_WEBENGINE option is off, or the module is
// not installed), math() and chart() answer "failed" immediately and those blocks
// show their source. svg() and markdownToHtml() are unaffected — neither needs a
// browser.

#include <QHash>
#include <QList>
#include <QLoggingCategory>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

Q_DECLARE_LOGGING_CATEGORY(lcRender)

class RenderHost : public QObject {
    Q_OBJECT
    // Bumped every time a job lands. QML bindings that draw a rendered block read
    // this, which is what makes a pending block become an image without the model
    // having to know a render was in flight.
    Q_PROPERTY(int epoch READ epoch NOTIFY epochChanged)
    // Whether formulas and charts can be drawn at all (i.e. built with WebEngine).
    // QML uses it to decide whether a `pending` state is worth waiting for.
    Q_PROPERTY(bool canRender READ canRender CONSTANT)
public:
    explicit RenderHost(QObject* parent = nullptr);
    ~RenderHost() override;

    int  epoch() const { return epoch_; }
    bool canRender() const;

    // --- lookups (all return {state, url, w, h}) --------------------------
    // `state` is "ready" (draw `url` at w×h), "pending" (queued — show the source
    // and wait for an `epoch` bump) or "failed" (show the source, for good).

    /** A formula. `display` distinguishes a `$$` block from inline `$` math. */
    Q_INVOKABLE QVariantMap math(const QString& tex, bool display);
    /** An Apache ECharts `option`, as JSON text. */
    Q_INVOKABLE QVariantMap chart(const QString& optionJson);
    /** A ```svg block's body. Needs no renderer — the source IS the file. */
    Q_INVOKABLE QVariantMap svg(const QString& source);

    /**
     * The renderer could not draw a figure we accepted and wrote. Demote it so the
     * block falls back to showing its source.
     *
     * Validation only proves the source holds nothing we refuse to draw — it cannot
     * prove Qt's SVG rasterizer will succeed. Without this, a file that writes fine
     * and draws blank leaves an empty hole in the reply, which is worse than the XML
     * it replaced. Wire it to Image.onStatusChanged == Image.Error.
     */
    Q_INVOKABLE void svgFailed(const QString& source);

    /**
     * Markdown -> the rich text QML's `Text` draws.
     *
     * QTextDocument owns both halves of this (it is the same importer `Text.
     * MarkdownText` uses), so tables, lists and code blocks render exactly as they
     * did — but going through HTML is what lets the caller splice an `<img>` in
     * where a formula was, which a markdown string cannot express.
     *
     * The family and size are the QML item's, not the document's default: toHtml()
     * bakes absolute font declarations from the document font, so passing the wrong
     * one would render the reply at a size the font-size setting does not reach.
     */
    Q_INVOKABLE QString markdownToHtml(const QString& markdown,
                                       const QString& family, qreal pointSize) const;

    // --- the host page's side of the wire (RenderHostView.qml) ------------
    //
    // The page is driven by a hidden WebEngineView in the QML tree, NOT by a
    // QWebEnginePage owned here -- the same shape harmony's hidden Web component has,
    // and not a stylistic choice: a standalone QWebEnginePage in this app killed its
    // renderer process outright (0xC0000409 inside Qt6WebEngineCore.dll) the moment a
    // page needed V8, while the Quick view runs the identical page and script fine.
    // So C++ owns the queue and the cache, and QML owns the one thing it must: the view.

    /** Where the view should navigate. */
    Q_PROPERTY(QUrl pageUrl READ pageUrl CONSTANT)
    QUrl pageUrl() const;

    /** The view finished loading the page. Starts the "can it typeset yet" probe. */
    Q_INVOKABLE void hostLoaded();
    /** The view could not load the page: math and charts stay as source. */
    Q_INVOKABLE void hostFailed(const QString& reason);
    /** A script this host asked for has returned. `id` is the one from runScript. */
    Q_INVOKABLE void scriptResult(int id, const QVariant& result);

signals:
    void epochChanged();

    /** Run `script` in the host page and hand the result back to scriptResult(id, …). */
    void runScript(int id, const QString& script);

private:
    struct Image {
        bool    ok = false;
        QString url;
        int     w = 0;
        int     h = 0;
    };

    // 3-char key prefix: kind ('D' display math, 'I' inline math, 'C' chart,
    // 'S' svg), colour mode, ':'. The whole key is hashed, so two modes would land
    // in different cache files -- this client has one (light) scheme, but the key
    // shape is harmony's so the two stay comparable.
    static QString keyOf(char kind);

    QVariantMap lookup(const QString& key);
    QVariantMap stateOf(const Image& img) const;
    void        remember(const QString& key, const Image& img);
    void        bumpEpoch();
    /** Can page-backed work (math, charts) run yet? Always false without WebEngine. */
    bool        pageReady() const;

    void scan();                       // index what earlier launches left behind
    void prune(const QStringList& names);
    void pump();                       // one render at a time
    void storeSvg(const QString& key, const QString& source);
    void store(const QString& key, const QString& rawJson);

    QString dir_;
    int     epoch_ = 0;

    QHash<QString, Image> cache_;      // this session, capped
    QList<QString>        cacheOrder_; // insertion order, for the eviction
    QHash<QString, Image> disk_;       // hash -> file already on disk
    QSet<QString>         queued_;
    QList<QString>        waiting_;
    bool                  rendering_ = false;
    bool                  ready_ = false;

#ifdef MIROBODY_WEBENGINE
    void probeReady();                 // ask the page whether MathJax is up yet
    int  readyPolls_ = 0;
    // ~5 s at 50 ms. Long enough for MathJax + ECharts on a cold start, short enough
    // that a page which will never come up says so instead of polling for the session.
    static constexpr int kReadyPollLimit = 100;

    // Job ids handed to the view. The readiness probe reserves one so its answer can be
    // told apart from a render's without a second channel.
    static constexpr int kReadyProbeId = -1;
    int nextJobId_ = 0;
    QHash<int, QString> inFlight_;      // job id -> cache key
#endif
};
