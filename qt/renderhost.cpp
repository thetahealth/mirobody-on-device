#include "renderhost.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QPair>
#include <QRegularExpression>
#include <QSize>
#include <QStandardPaths>
#include <QTextDocument>
#include <QTimer>
#include <QUrl>

#include <algorithm>

// Off unless asked for: QT_LOGGING_RULES="mirobody.render.debug=true".
//
// Worth having wired in rather than added when something breaks. Everything here is
// asynchronous and silent by design -- a failed render is indistinguishable from a
// slow one from the outside, because BOTH show the block's source -- so without a way
// to see the page come up and each job land, the only symptom of any fault in the
// chain is "it still looks like code".
//
// On Windows the app is a GUI subsystem binary with no console, so pair it with
// QT_FORCE_STDERR_LOGGING=1 and redirect stderr to a file.
Q_LOGGING_CATEGORY(lcRender, "mirobody.render")

namespace {

/**
 * Files kept on disk. Above the memory cap because the whole point of the disk tier
 * is to outlive both the session and an eviction; below anything that would make the
 * prune scan (which stats every file) worth worrying about.
 */
constexpr int kDiskCap  = 600;
constexpr int kCacheCap = 200;

QString pad8(quint32 n) {
    return QStringLiteral("%1").arg(n, 8, 16, QLatin1Char('0'));
}

/**
 * djb2 and sdbm concatenated — 64 bits of filename.
 *
 * One 32-bit hash would be a live hazard rather than a theoretical one: two keys that
 * collide name the SAME file, the second render clobbers the first, and the first
 * message then silently draws the second's formula. Two independent mixes put that
 * back under the noise floor, and the disk index raises the stakes — a wrong file now
 * survives the launch that wrote it.
 */
QString hashOf(const QString& s) {
    quint32 a = 5381;   // djb2
    quint32 b = 0;      // sdbm
    const QList<uint> ucs = s.toUcs4();
    for (uint c : ucs) {
        a = (a << 5) + a + c;
        b = c + (b << 6) + (b << 16) - b;
    }
    return pad8(a) + pad8(b);
}

/** Whitespace, for the "is this the start of a real attribute" tests below. */
bool isSpace(QChar c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r'; }

/** An `attr=` whose value is not a same-document `#fragment`. */
bool hasExternalRef(const QString& lower, const QString& attr) {
    const QString needle = attr + QLatin1Char('=');
    int from = 0;
    while (true) {
        const int at = lower.indexOf(needle, from);
        if (at < 0) return false;
        int j = at + needle.size();
        while (j < lower.size() && (lower.at(j) == u'"' || lower.at(j) == u'\'')) j++;
        if (j < lower.size() && lower.at(j) != u'#') return true;
        from = at + needle.size();
    }
}

/**
 * Is this SVG source one we are willing to hand to the rasterizer?
 *
 * VALIDATE AND REFUSE, never rewrite. The source is model-authored and untrusted, and
 * stripping dangerous constructs out of untrusted markup by text surgery is a game of
 * whack-a-mole against an adversary who writes the input. Refusing is total,
 * auditable, and costs a model nothing: a diagram needs none of this.
 *
 * Qt rasterizes SVG itself (QtSvg, a static renderer) rather than through a browser,
 * so there is no script engine to attack. What is left is what this checks for:
 *
 *   external references  <image href="https://…"> fetches when the reply is read,
 *                        which is the read-receipt problem markdown images are
 *                        refused for. Fragment refs (#gradient) are fine and common,
 *                        so they stay allowed.
 *   DOCTYPE / ENTITY     entity expansion is a parser-level denial of service that no
 *                        renderer feature depends on.
 *   script, foreignObject, event handlers
 *                        inert here by construction, refused anyway — the cost is
 *                        three string searches and it stops this from silently
 *                        becoming unsafe if the SVG ever reaches the render page.
 */
bool svgIsSafe(const QString& source) {
    const QString s = source.toLower();
    if (s.contains(QLatin1String("<script")) || s.contains(QLatin1String("<foreignobject"))
        || s.contains(QLatin1String("<!doctype")) || s.contains(QLatin1String("<!entity"))
        || s.contains(QLatin1String("<use"))) {
        return false;
    }
    // Any on*= handler attribute. Tested as ' on…=' so "button-on=" and prose
    // containing "on" do not trip it.
    for (int i = 1; i + 3 < s.size(); ++i) {
        if (!isSpace(s.at(i - 1)) || s.at(i) != u'o' || s.at(i + 1) != u'n') continue;
        int j = i + 2;
        while (j < s.size() && s.at(j) != u'=' && s.at(j) != u' ' && s.at(j) != u'>') j++;
        if (j < s.size() && s.at(j) == u'=') return false;
    }
    return !hasExternalRef(s, QStringLiteral("href")) && !hasExternalRef(s, QStringLiteral("src"));
}

/**
 * The quoted value of an attribute, or "".
 *
 * The name must start a real attribute — preceded by whitespace or `<` — because a
 * bare indexOf("width=") also matches `stroke-width="2"`, and a figure sized off a
 * stroke would be shaped by whichever rectangle happened to be drawn first.
 */
QString attrValue(const QString& lower, const QString& source, const QString& attr) {
    const QString needle = attr + QLatin1Char('=');
    int from = 0;
    while (true) {
        const int at = lower.indexOf(needle, from);
        if (at < 0) return {};
        const QChar before = at == 0 ? QLatin1Char('<') : lower.at(at - 1);
        if (isSpace(before) || before == u'<') {
            const int q = at + needle.size();
            if (q < source.size() && (source.at(q) == u'"' || source.at(q) == u'\'')) {
                const int end = source.indexOf(source.at(q), q + 1);
                if (end > 0) return source.mid(q + 1, end - q - 1);
            }
            return {};
        }
        from = at + needle.size();
    }
}

/** An absolute CSS length -> px. A percentage (or anything unusable) -> 0. */
int plainPixels(const QString& v) {
    const QString t = v.trimmed();
    if (t.isEmpty() || t.endsWith(QLatin1Char('%'))) return 0;
    bool ok = false;
    const double n = t.toDouble(&ok);
    if (!ok) {
        // "420px" and friends: take the leading number.
        int i = 0;
        while (i < t.size() && (t.at(i).isDigit() || t.at(i) == u'.' || t.at(i) == u'-')) i++;
        if (i == 0) return 0;
        const double lead = t.left(i).toDouble(&ok);
        if (!ok || lead <= 0) return 0;
        return qRound(lead);
    }
    return n <= 0 ? 0 : qRound(n);
}

/**
 * The figure's intrinsic size, for an aspect ratio. {0,0} means unknown.
 *
 * viewBox first, and width/height only as a fallback: a hand-written SVG reliably
 * carries a viewBox, while its width/height are as often absent or a percentage — and
 * a percentage says nothing about shape, so it counts as unknown rather than as the
 * number in front of the sign.
 */
QSize svgSize(const QString& source) {
    const QString lower = source.toLower();

    const QString vb = attrValue(lower, source, QStringLiteral("viewbox"));
    if (!vb.isEmpty()) {
        const QStringList parts = vb.split(QRegularExpression(QStringLiteral("[\\s,]+")),
                                           Qt::SkipEmptyParts);
        if (parts.size() == 4) {
            bool okW = false, okH = false;
            const double w = parts.at(2).toDouble(&okW);
            const double h = parts.at(3).toDouble(&okH);
            if (okW && okH && w > 0 && h > 0) return QSize(qRound(w), qRound(h));
        }
    }

    const int w = plainPixels(attrValue(lower, source, QStringLiteral("width")));
    const int h = plainPixels(attrValue(lower, source, QStringLiteral("height")));
    if (w > 0 && h > 0) return QSize(w, h);
    return QSize(0, 0);
}

}  // namespace

//------------------------------------------------------------------------------

RenderHost::RenderHost(QObject* parent) : QObject(parent) {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!base.isEmpty()) {
        dir_ = base + QStringLiteral("/render-cache");
        if (!QDir().exists(dir_)) {
            // First run: nothing to index. A failure here is not fatal -- renders
            // fail their file write, cache as failed, and the blocks show source.
            QDir().mkpath(dir_);
        } else {
            scan();
        }
    }

    qCDebug(lcRender) << "cache dir" << dir_ << "indexed" << disk_.size()
                      << "file(s); webengine =" << canRender();
}

RenderHost::~RenderHost() = default;

bool RenderHost::canRender() const {
#ifdef MIROBODY_WEBENGINE
    return true;
#else
    return false;
#endif
}

//------------------------------------------------------------------------------
// The disk index
//------------------------------------------------------------------------------

void RenderHost::scan() {
    QDir d(dir_);
    const QStringList names = d.entryList(QDir::Files);
    QStringList kept;
    for (const QString& name : names) {
        // `<hash>-<w>x<h>.svg|png` -> the image it holds. Everything the lookup
        // needs is in the name, so this is one listing and no file reads. Anything
        // that does not parse is deleted rather than kept: it is either an older
        // naming scheme (whose dimensions are unrecoverable, so it can never be
        // served) or not ours, and this directory belongs to nothing else.
        const int dash = name.indexOf(QLatin1Char('-'));
        const int dot  = name.lastIndexOf(QLatin1Char('.'));
        bool okW = false, okH = false;
        int w = 0, h = 0;
        if (dash > 0 && dot > dash) {
            const QString dims = name.mid(dash + 1, dot - dash - 1);
            const int x = dims.indexOf(QLatin1Char('x'));
            if (x > 0) {
                w = dims.left(x).toInt(&okW);
                h = dims.mid(x + 1).toInt(&okH);
            }
        }
        if (!okW || !okH) {
            QFile::remove(d.filePath(name));
            continue;
        }
        Image img;
        img.ok  = true;
        img.url = QUrl::fromLocalFile(d.filePath(name)).toString();
        img.w   = w;
        img.h   = h;
        disk_.insert(name.left(dash), img);
        kept.push_back(name);
    }
    prune(kept);
}

void RenderHost::prune(const QStringList& names) {
    if (names.size() <= kDiskCap) return;
    // Nothing else ever deletes: a memory eviction drops the map entry but KEEPS the
    // file, which is exactly what makes the disk tier worth having. So this is the
    // only thing standing between a long-lived install and unbounded growth. The
    // mtime stat costs one syscall per file, and only on the launches actually over.
    QDir d(dir_);
    QList<QPair<qint64, QString>> aged;
    aged.reserve(names.size());
    for (const QString& name : names) {
        const QFileInfo fi(d.filePath(name));
        // Unreadable sorts oldest, so it goes first.
        aged.push_back({fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0, name});
    }
    std::sort(aged.begin(), aged.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    const int drop = aged.size() - kDiskCap;
    for (int i = 0; i < drop; ++i) {
        const QString& name = aged.at(i).second;
        QFile::remove(d.filePath(name));
        disk_.remove(name.left(name.indexOf(QLatin1Char('-'))));
    }
}

//------------------------------------------------------------------------------
// Lookups
//------------------------------------------------------------------------------

QString RenderHost::keyOf(char kind) {
    return QString(QLatin1Char(kind)) + QStringLiteral("0:");
}

QVariantMap RenderHost::math(const QString& tex, bool display) {
#ifndef MIROBODY_WEBENGINE
    Q_UNUSED(tex);
    Q_UNUSED(display);
    return {{QStringLiteral("state"), QStringLiteral("failed")}};
#else
    return lookup(keyOf(display ? 'D' : 'I') + tex);
#endif
}

QVariantMap RenderHost::chart(const QString& optionJson) {
#ifndef MIROBODY_WEBENGINE
    Q_UNUSED(optionJson);
    return {{QStringLiteral("state"), QStringLiteral("failed")}};
#else
    return lookup(keyOf('C') + optionJson);
#endif
}

QVariantMap RenderHost::svg(const QString& source) {
    return lookup(keyOf('S') + source);
}

QVariantMap RenderHost::stateOf(const Image& img) const {
    QVariantMap m;
    m.insert(QStringLiteral("state"),
             img.ok ? QStringLiteral("ready") : QStringLiteral("failed"));
    m.insert(QStringLiteral("url"), img.url);
    m.insert(QStringLiteral("w"), img.w);
    m.insert(QStringLiteral("h"), img.h);
    return m;
}

QVariantMap RenderHost::lookup(const QString& key) {
    const auto hit = cache_.constFind(key);
    if (hit != cache_.constEnd()) return stateOf(*hit);

    // Rendered by an earlier launch, or evicted from memory but never from disk.
    // Promoting it into `cache_` keeps the hot path a single hash lookup.
    const auto onDisk = disk_.constFind(hashOf(key));
    if (onDisk != disk_.constEnd()) {
        const Image img = *onDisk;
        remember(key, img);
        return stateOf(img);
    }

    if (!queued_.contains(key)) {
        queued_.insert(key);
        waiting_.push_back(key);
        pump();
    }
    return {{QStringLiteral("state"), QStringLiteral("pending")}};
}

void RenderHost::remember(const QString& key, const Image& img) {
    if (!cache_.contains(key)) {
        // Oldest out at the cap. The FILE stays -- see prune().
        if (cacheOrder_.size() >= kCacheCap) {
            cache_.remove(cacheOrder_.takeFirst());
        }
        cacheOrder_.push_back(key);
    }
    cache_.insert(key, img);
}

void RenderHost::svgFailed(const QString& source) {
    const QString key = keyOf('S') + source;
    const auto hit = cache_.constFind(key);
    if (hit != cache_.constEnd() && !hit->ok) {
        return;   // already demoted; do not loop on a re-render
    }
    disk_.remove(hashOf(key));
    remember(key, Image{});
    bumpEpoch();
}

void RenderHost::bumpEpoch() {
    ++epoch_;
    emit epochChanged();
}

//------------------------------------------------------------------------------
// The queue. One render at a time: the host page's document state is shared.
//------------------------------------------------------------------------------

/**
 * `s` as a JavaScript string literal, escaped so a formula full of backslashes and
 * quotes crosses into the page as DATA rather than as syntax.
 *
 * Via a one-element JSON array, `["…"]`, indexed at [0]: JSON's string escaping is a
 * subset of JavaScript's, so the text QJsonDocument produces is already a valid JS
 * literal, and an array is the smallest thing QJsonDocument will serialise a bare
 * string inside.
 *
 * (The version this replaces built `JSON.parse({"v":"…"})` -- a JSON *object literal*
 * where JSON.parse wants a *string*. JS stringified the object to "[object Object]",
 * JSON.parse threw on it, and every formula and chart in the app came back a failure.)
 */
static QString jsLiteral(const QString& s) {
    const QByteArray arr = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(arr) + QStringLiteral("[0]");
}

bool RenderHost::pageReady() const {
#ifdef MIROBODY_WEBENGINE
    return ready_;
#else
    return false;
#endif
}

void RenderHost::pump() {
    if (rendering_) return;

    // FIFO with ONE exception: a ```svg figure needs no page, so it is not made to
    // wait behind a formula while MathJax is still starting up. Strict FIFO meant a
    // reply whose chart came before its diagram (the built-in guide is exactly that)
    // showed BOTH as source whenever the page was slow or broken -- one head-of-line
    // job held back work that had nothing to do with it.
    int at = -1;
    for (int i = 0; i < waiting_.size(); ++i) {
        if (waiting_.at(i).at(0) == u'S' || pageReady()) { at = i; break; }
    }
    if (at < 0) {
        qCDebug(lcRender) << "pump: waiting on the page," << waiting_.size() << "job(s) queued";
        return;   // only page work left, and the page is not up yet
    }

    const QString key = waiting_.takeAt(at);
    const char kind = key.at(0).toLatin1();
    const QString payload = key.mid(3);
    rendering_ = true;

    // The source IS the file for an SVG, but it still goes through the queue rather
    // than straight into lookup(), because lookup() runs inside a binding evaluation
    // and a synchronous write there would put disk I/O on the frame. The zero timer
    // gets it off the binding the way runJavaScript does for the other two.
    if (kind == 'S') {
        QTimer::singleShot(0, this, [this, key, payload]() { storeSvg(key, payload); });
        return;
    }

#ifdef MIROBODY_WEBENGINE
    const QString script = kind == 'C'
        ? QStringLiteral("window.renderChart(%1)").arg(jsLiteral(payload))
        : QStringLiteral("window.renderTex(%1, %2)")
              .arg(jsLiteral(payload),
                   kind == 'D' ? QStringLiteral("true") : QStringLiteral("false"));

    // Handed to the view, which runs it and calls scriptResult(id, …) back.
    const int id = ++nextJobId_;
    inFlight_.insert(id, key);
    qCDebug(lcRender) << "render" << kind << "job" << id << payload.size() << "chars";
    emit runScript(id, script);
#else
    // Unreachable: without WebEngine these never enter the queue (math()/chart()
    // answer "failed" outright). Settle the slot rather than wedge it if they ever do.
    store(key, QString());
#endif
}

QUrl RenderHost::pageUrl() const {
    return QUrl(QStringLiteral("qrc:/render/render.html"));
}

#ifdef MIROBODY_WEBENGINE
void RenderHost::hostLoaded() {
    qCDebug(lcRender) << "host page loaded";
    probeReady();
}

void RenderHost::hostFailed(const QString& reason) {
    // Reported, then probed ANYWAY. A WebEngineView can report LoadFailedStatus with an
    // empty error for a page that in fact runs -- the same thing happens to a bare
    // loadHtml() -- so a failure here is a hint, not a verdict. The real test is whether
    // the page answers, and if it does not the probe says so on its own timeout.
    qCWarning(lcRender) << "render page reported a load failure:" << reason
                        << "-- probing anyway";
    probeReady();
}

void RenderHost::probeReady() {
    // MathJax's startup is asynchronous, so "the page loaded" is not "the page can
    // typeset". render.html sets window.mbReady when both libraries are up; ask for it
    // rather than racing, or the first formula of the session renders as a failure and
    // stays cached that way for the whole run.
    emit runScript(kReadyProbeId, QStringLiteral("window.mbReady === true"));
}

void RenderHost::scriptResult(int id, const QVariant& result) {
    if (id == kReadyProbeId) {
        if (result.toBool()) {
            qCDebug(lcRender) << "host page ready after" << readyPolls_ << "probe(s)";
            ready_ = true;
            pump();
            return;
        }
        // Bounded, because "not ready yet" and "never going to be ready" look identical
        // from here. Probing forever would leave every formula and chart pending,
        // silently showing source, with nothing in the log to say why.
        if (++readyPolls_ > kReadyPollLimit) {
            qCWarning(lcRender)
                << "render page never became ready (window.mbReady still false after"
                << kReadyPollLimit * 50 << "ms) -- MathJax or ECharts did not start. "
                   "Math and charts will show their source.";
            return;
        }
        QTimer::singleShot(50, this, &RenderHost::probeReady);
        return;
    }

    const QString key = inFlight_.take(id);
    if (key.isEmpty()) {
        qCWarning(lcRender) << "script result for unknown job" << id;
        return;
    }
    store(key, result.toString());
}
#else
void RenderHost::hostLoaded() {}
void RenderHost::hostFailed(const QString&) {}
void RenderHost::scriptResult(int, const QVariant&) {}
#endif

void RenderHost::storeSvg(const QString& key, const QString& source) {
    // Refused source caches as a failure, exactly like a formula MathJax rejects, so
    // the block shows its XML and the check never runs twice on the same source.
    Image img;
    if (!svgIsSafe(source)) {
        qCWarning(lcRender) << "svg refused (external ref, DOCTYPE, script or handler)";
    }
    if (svgIsSafe(source) && !dir_.isEmpty()) {
        const QSize size = svgSize(source);
        const QString path = QStringLiteral("%1/%2-%3x%4.svg")
                                 .arg(dir_, hashOf(key))
                                 .arg(size.width())
                                 .arg(size.height());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(source.toUtf8());
            f.close();
            img.ok  = true;
            img.url = QUrl::fromLocalFile(path).toString();
            img.w   = size.width();
            img.h   = size.height();
            disk_.insert(hashOf(key), img);
        }
    }
    qCDebug(lcRender) << "svg stored" << img.ok << img.w << "x" << img.h << img.url;
    remember(key, img);
    // Deliberately still in `queued_`: a failure lives in memory only, and if it were
    // evicted an un-queued key would be attempted again on every rebuild. Left
    // queued, an evicted failure reads as "pending" forever, which shows the same
    // source and costs nothing.
    rendering_ = false;
    bumpEpoch();
    pump();
}

void RenderHost::store(const QString& key, const QString& rawJson) {
    Image img;
    const QJsonDocument doc = QJsonDocument::fromJson(rawJson.toUtf8());
    const QJsonObject reply = doc.object();
    if (!reply.value(QStringLiteral("ok")).toBool()) {
        // The page's own error text, when it has one. An empty reply means the script
        // itself threw before returning -- which is what a malformed call looks like.
        qCWarning(lcRender) << "render failed:" << key.left(3)
                            << (rawJson.isEmpty() ? QStringLiteral("(script threw / no result)")
                                                  : reply.value(QStringLiteral("err")).toString());
    }
    if (reply.value(QStringLiteral("ok")).toBool() && !dir_.isEmpty()) {
        const int w = reply.value(QStringLiteral("w")).toInt();
        const int h = reply.value(QStringLiteral("h")).toInt();
        // Dimensions in the name, so the next launch's scan() can index this file
        // without opening it. Same key -> same name, so a re-render overwrites rather
        // than orphaning what it replaces.
        const QString stem = QStringLiteral("%1/%2-%3x%4").arg(dir_, hashOf(key))
                                 .arg(w).arg(h);
        QString path;
        QByteArray bytes;
        if (reply.contains(QStringLiteral("svg"))) {
            path  = stem + QStringLiteral(".svg");
            bytes = reply.value(QStringLiteral("svg")).toString().toUtf8();
        } else if (reply.contains(QStringLiteral("png"))) {
            path  = stem + QStringLiteral(".png");
            bytes = QByteArray::fromBase64(
                reply.value(QStringLiteral("png")).toString().toUtf8());
        }
        if (!path.isEmpty()) {
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(bytes);
                f.close();
                img.ok  = true;
                img.url = QUrl::fromLocalFile(path).toString();
                img.w   = w;
                img.h   = h;
                disk_.insert(hashOf(key), img);
            }
        }
    }

    qCDebug(lcRender) << "stored" << key.left(3) << img.w << "x" << img.h << img.url;
    remember(key, img);   // stays in `queued_` -- see storeSvg
    rendering_ = false;
    bumpEpoch();
    pump();
}

//------------------------------------------------------------------------------
// Markdown
//------------------------------------------------------------------------------

QString RenderHost::markdownToHtml(const QString& markdown, const QString& family,
                                   qreal pointSize) const {
    QTextDocument doc;
    QFont f = doc.defaultFont();
    if (!family.isEmpty()) f.setFamily(family);
    if (pointSize > 0) f.setPointSizeF(pointSize);
    doc.setDefaultFont(f);
    doc.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    return doc.toHtml();
}
