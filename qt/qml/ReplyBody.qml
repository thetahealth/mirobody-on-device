import QtQuick
import Mirobody
import "markdown.js" as Md

// An assistant reply, split into the pieces that need different renderers: prose (as
// rich text, with formulas spliced in as images), a ```svg figure, and a ```echarts
// chart. Mirrors android's MessageBubble + splitMessage and harmony's RichMessage.
//
// The split runs on EVERY token while a reply streams, which is what android does
// too. It is also what makes an unclosed fence stay prose: half an SVG is not a
// figure, so a block becomes one the moment its closing fence lands and not before —
// the reader watches source arrive rather than a figure flickering in and out.
Column {
    id: replyBody

    // The reply's markdown, as it stands (mid-stream or settled).
    property string source: ""

    spacing: 8

    Repeater {
        // A plain JS array, rebuilt whenever `source` changes.
        model: Md.splitMessage(replyBody.source)
        ReplySegment {
            required property var modelData
            width: replyBody.width
            segment: modelData
        }
    }
}
