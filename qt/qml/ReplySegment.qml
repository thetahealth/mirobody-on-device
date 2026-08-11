import QtQuick
import Mirobody
import "markdown.js" as Md

// One piece of a reply: prose, a ```svg figure, or a ```echarts chart.
//
// A figure is a plain Image over a file the render host already produced — the list
// never holds a web view (see renderhost.cpp). Until that file exists, and forever if
// it cannot be made, the block shows its own source instead: an unrendered formula or
// option is still information, an empty hole is not.
Item {
    id: seg

    // { kind: "md" | "svg" | "chart", text: <the segment's source> }
    property var segment: ({ kind: "md", text: "" })

    readonly property bool isProse: segment.kind === "md"

    // Both renderers are instantiated and one is hidden, rather than a Loader swapping
    // between them.
    //
    // A Loader ASSIGNS its own size to whatever it loads, and a Text that has been given
    // an explicit height re-lays-out when a rich-text <img> finishes loading, which
    // changes the implicit height the Loader took its size from, which resizes the Text
    // again -- a formula spliced into a paragraph pegged a core forever and the window
    // stopped answering. (Only INLINE math showed it: display math and charts are their
    // own segment, and a fence's fallback is plain text with no image in it.) Nothing
    // assigns a height here; the two children publish theirs and this reads it.
    implicitHeight: isProse ? prose.implicitHeight : figure.implicitHeight
    height: implicitHeight

    /**
     * A prose segment as rich text, with each formula replaced by its rendered image.
     *
     * Markdown cannot express "an image here", so the conversion goes through HTML:
     * scanMath() swaps every formula for an inert placeholder word, QTextDocument
     * turns the rest into the rich text QML draws (the same importer Text.MarkdownText
     * uses, so tables and code blocks are unchanged), and each placeholder is then
     * swapped for an <img> — or, if that formula has not rendered, for its own source.
     *
     * `epoch` is a parameter rather than something this reads, so the binding on it is
     * visible at the call site: a formula that was pending when the reply first drew
     * lands later, and the epoch bump is what brings this back to pick it up.
     */
    function html(markdown, family, epoch) {
        var scan = Md.scanMath(markdown);
        var out = render.markdownToHtml(scan.text, family, Theme.baseSize);
        for (var i = 0; i < scan.spans.length; ++i) {
            var span = scan.spans[i];
            // A formula is put back as its own SOURCE, not as an <img>.
            //
            // Splicing the rendered SVG in here is what this was built to do, and it
            // cannot be done this way: an <img> inside Text.RichText sends QQuickText
            // into an endless relayout -- one formula in one paragraph pegs a core and
            // the window stops answering, with no binding-loop warning and nothing in
            // any log. Measured on every kind of block: prose, tables, code, ```svg and
            // ```echarts all idle at ~0.1 core; a single inline formula sits at 1.00.
            // Removing the Loader that wrapped this Text (a plausible cause, since it
            // assigned a height back) changed nothing.
            //
            // So math reads as `$BMI = w/h^2$` for now, which is at least what the
            // model wrote. Fixing it properly needs a mechanism that is not a rich-text
            // image -- see qt/README.md.
            var replacement = Md.escapeHtml(span.raw);
            // The FUNCTION form of replace, never the string one: a replacement
            // containing "$$" or "$&" is a substitution pattern to String.replace, and
            // an unrendered `$$x$$` would come out as `$x$`.
            out = out.replace(span.token, function () { return replacement; });
        }
        return out;
    }

    // --- prose ------------------------------------------------------------
    Text {
        id: prose
        // WIDTH ONLY. Text re-declares implicitHeight read-only (it comes from
        // QQuickImplicitSizeItem, unlike Item's or Rectangle's) and already tracks the
        // laid-out content height at this width, which is what the root reads.
        width: seg.width
        visible: seg.isProse
        // Not evaluated for a figure segment: html() runs a markdown conversion and a
        // math scan, and neither is free on a long reply.
        text: seg.isProse ? seg.html(seg.segment.text, font.family, render.epoch) : ""
        textFormat: Text.RichText
        wrapMode: Text.Wrap
        color: Theme.surfaceFg
        font.pointSize: Theme.baseSize
        onLinkActivated: function (link) { Qt.openUrlExternally(link); }
    }

    // --- a figure: an SVG block or a chart ---------------------------------
    Column {
        id: figure
        width: seg.width
        visible: !seg.isProse
        spacing: 0

        // Reading `render.epoch` is what re-runs this when the render lands; the
        // lookup itself queues the job on the first miss.
        readonly property var img: {
            var _ = render.epoch;
            // A hidden item still evaluates its bindings, so without this a prose
            // segment would ask for a CHART of its own paragraph -- queueing a job that
            // can only fail, ahead of the ones that matter.
            if (seg.isProse) return ({ state: "failed" });
            return seg.segment.kind === "svg" ? render.svg(seg.segment.text)
                                              : render.chart(seg.segment.text);
        }
        readonly property bool drawable: img.state === "ready"
        // Whether the render host knew the figure's shape. A chart always has one
        // (it is a fixed canvas) and so does a formula; a hand-written SVG with
        // neither a viewBox nor an absolute width/height does not, and its size is
        // then left to the Image, which reads the intrinsic size out of the file
        // itself. Showing the XML instead would be a worse answer to "this diagram
        // omitted an attribute".
        readonly property bool sized: drawable && img.w > 0 && img.h > 0
        // Never upscale past the figure's own size: a 420px diagram blown up to a
        // 700px window is soft for no reason.
        readonly property real drawWidth: !drawable ? 0
                                        : (sized ? Math.min(figure.width, img.w) : figure.width)

        Image {
            visible: figure.drawable
            width: figure.drawWidth
            height: figure.sized ? figure.drawWidth * figure.img.h / figure.img.w
                              : (implicitWidth > 0 ? width * implicitHeight / implicitWidth : 0)
            source: figure.drawable ? figure.img.url : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            // SVG rasterizes at sourceSize, so without this a diagram is drawn at
            // its intrinsic size and then scaled — visibly soft on a high-DPI
            // screen. A chart is already a 2x PNG and wants no resize.
            //
            // WIDTH ONLY, never both: with one dimension set Qt keeps the aspect
            // ratio, which is what we want anyway — and constraining the height
            // here closes a loop, because an unsized figure derives its height
            // from implicitHeight, which is itself a function of sourceSize.
            sourceSize.width: seg.segment.kind === "svg" ? Math.round(width) : 0
            // Validation proves the source holds nothing we refuse to draw; it
            // cannot prove the rasterizer will succeed. A file that writes fine and
            // draws blank would leave a hole where the XML was, so demote it and
            // let the fallback below take over.
            onStatusChanged: {
                if (status === Image.Error && seg.segment.kind === "svg") {
                    render.svgFailed(seg.segment.text);
                }
            }
        }

        // The fallback, and the whole block while a render is queued: the source,
        // as the code block it would have been.
        Rectangle {
            visible: !figure.drawable
            width: figure.width
            implicitHeight: visible ? sourceText.implicitHeight + 16 : 0
            height: implicitHeight
            radius: 6
            color: Theme.surfaceLow
            border.color: Theme.outlineVar
            border.width: 1

            Text {
                id: sourceText
                x: 8; y: 8
                width: parent.width - 16
                text: seg.segment.text
                wrapMode: Text.Wrap
                font.family: "monospace"
                font.pointSize: Theme.baseSize - 2
                color: Theme.surfaceVarFg
            }
        }
    }
}
