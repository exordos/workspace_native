import QtQuick

// Half-opacity hover surface icon button used by the call window header
// (reference hover:bg-bg/50 rounded-lg p-1.5 sm:p-2). Glyphs are drawn in
// Canvas so no icon font or missing SVG asset can change the geometry.
Rectangle {
    id: button
    required property var colors
    required property string glyph // fullscreen | fullscreen-exit | chevron-up | chevron-down | close
    property string label: ""
    property string tooltip: ""
    property int glyphSize: 18
    readonly property int pad: 6
    signal clicked

    Accessible.role: Accessible.Button
    Accessible.name: label
    width: glyphSize + pad * 2
    height: glyphSize + pad * 2
    radius: 8
    color: area.containsMouse ? Qt.alpha(colors["bg"], 0.5) : "transparent"

    Canvas {
        anchors.centerIn: parent
        width: button.glyphSize
        height: button.glyphSize
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = Qt.rgba(0, 0, 0, 0) // recolored below via palette
            ctx.strokeStyle = Qt.alpha(button.colors["text-muted"], 1).toString()
            ctx.lineWidth = 2
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            const s = width
            const m = 4 // outer margin of the drawn glyph
            ctx.beginPath()
            if (button.glyph === "close") {
                ctx.moveTo(m, m)
                ctx.lineTo(s - m, s - m)
                ctx.moveTo(s - m, m)
                ctx.lineTo(m, s - m)
            } else if (button.glyph === "chevron-up" || button.glyph === "chevron-down") {
                const up = button.glyph === "chevron-up"
                const y = up ? s * 0.38 : s * 0.62
                const t = s * 0.22
                ctx.moveTo(s / 2 - t, up ? y + t : y - t)
                ctx.lineTo(s / 2, y)
                ctx.lineTo(s / 2 + t, up ? y + t : y - t)
            } else {
                // fullscreen: corner brackets pointing out; fullscreen-exit
                // uses the same brackets pointing in.
                const out = button.glyph !== "fullscreen-exit"
                const a = out ? m : s * 0.30
                const b = out ? s * 0.38 : s - m
                const arms = 6
                // top-left
                ctx.moveTo(a, a + arms)
                ctx.lineTo(a, a)
                ctx.lineTo(a + arms, a)
                // top-right
                ctx.moveTo(b - arms, a)
                ctx.lineTo(b, a)
                ctx.lineTo(b, a + arms)
                // bottom-right
                ctx.moveTo(b, b - arms)
                ctx.lineTo(b, b)
                ctx.lineTo(b - arms, b)
                // bottom-left
                ctx.moveTo(a + arms, b)
                ctx.lineTo(a, b)
                ctx.lineTo(a, b - arms)
            }
            ctx.stroke()
        }
        property string watchedGlyph: button.glyph
        onWatchedGlyphChanged: requestPaint()
        property int watchedGlyphSize: button.glyphSize
        onWatchedGlyphSizeChanged: requestPaint()
        property var watchedColors: button.colors
        onWatchedColorsChanged: requestPaint()
        id: canvas
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
