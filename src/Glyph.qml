import QtQuick
import QtQuick.Shapes

// One family of line icons, drawn on a 24-unit grid with the same stroke and
// square ends, so places, toolbar and tiles all match. Strokes stay crisp at
// any size because the path is scaled, not the stroke.
Item {
    id: glyph

    property string name: "file"
    property color color: theme.foreground
    property real size: 18
    property real stroke: 1.6
    // Brand marks are solid shapes rather than line drawings.
    readonly property bool filled: name === "dropbox"

    implicitWidth: size
    implicitHeight: size

    function circle(cx, cy, r) {
        return "M" + (cx - r) + " " + cy + "a" + r + " " + r + " 0 1 0 " + (2 * r) + " 0a" + r + " " + r + " 0 1 0 " + (-2 * r) + " 0"
    }

    // On themes that round windows, the icons' boxes round too.
    readonly property real r: theme.radius > 0 ? 2 : 0

    // A rectangle as a path, its corners rounded by `rad` (quadratic curves,
    // which the curve renderer draws natively).
    function box(x, y, w, h, rad) {
        if (rad <= 0)
            return "M" + x + " " + y + "h" + w + "v" + h + "h" + (-w) + "z"
        const x2 = x + w, y2 = y + h
        return "M" + (x + rad) + " " + y + "H" + (x2 - rad) + "Q" + x2 + " " + y + " " + x2 + " " + (y + rad)
             + "V" + (y2 - rad) + "Q" + x2 + " " + y2 + " " + (x2 - rad) + " " + y2
             + "H" + (x + rad) + "Q" + x + " " + y2 + " " + x + " " + (y2 - rad)
             + "V" + (y + rad) + "Q" + x + " " + y + " " + (x + rad) + " " + y + "Z"
    }
    // A page with a folded corner, rounded like the boxes.
    function page() {
        if (r <= 0)
            return "M6 3h8l4 4v14H6zM14 3v4h4"
        return "M8 3H14L18 7V19Q18 21 16 21H8Q6 21 6 19V5Q6 3 8 3ZM14 3v4h4"
    }
    function folderBox() {
        if (r <= 0)
            return "M3 6h7l2 2h9v11H3z"
        return "M3 8Q3 6 5 6H10L12 8H19Q21 8 21 10V17Q21 19 19 19H5Q3 19 3 17Z"
    }

    // Glyphs come with every row, tile and menu entry, so only the drawing
    // for this name is built. Any other name, "file" among them, is a page.
    readonly property string path: {
        switch (name) {
        case "documents": return page() + "M9 12h6M9 16h6"
        case "downloads": return "M12 4v11M7 10l5 5 5-5M4 20h16"
        case "pictures": return box(3, 5, 18, 14, r) + "M3 16l5-5 4 4 3-3 6 6" + circle(16, 9, 1.5)
        case "music": return "M9 17V5l11-2v12" + circle(6.5, 17, 2.5) + circle(17.5, 15, 2.5)
        case "videos": return box(3, 5, 18, 14, r) + "M10 9v6l5-3z"
        // Dropbox's own mark, five diamonds: filled, as the brand draws it.
        case "dropbox": return "M7 2.5L12 5.7 7 8.9 2 5.7zM17 2.5L22 5.7 17 8.9 12 5.7zM7 8.9L12 12.1 7 15.3 2 12.1zM17 8.9L22 12.1 17 15.3 12 12.1zM12 13.3L17 16.5 12 19.7 7 16.5z"
        case "recent": return circle(12, 12, 9) + "M12 7v5l3 2"
        case "trash": return "M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13"
        case "drive": return box(3, 6, 18, 12, r) + "M7 14h2"
        case "eject": return "M12 5l7 8H5zM5 18h14"
        case "peer": return box(4, 4, 16, 11, r) + "M9 20h6M12 15v5"
        // Machines joined to one another: the whole tailnet.
        case "tailnet": return circle(12, 5, 2.5) + circle(5, 18, 2.5) + circle(19, 18, 2.5) + "M10.8 7.2L6.2 15.8M13.2 7.2l4.6 8.6M7.5 18h9"
        case "folder": return folderBox()
        case "back": return "M15 6l-6 6 6 6"
        case "forward": return "M9 6l6 6-6 6"
        case "grid": return box(4, 4, 6, 6, r / 2) + box(14, 4, 6, 6, r / 2) + box(4, 14, 6, 6, r / 2) + box(14, 14, 6, 6, r / 2)
        case "list": return "M4 6h16M4 12h16M4 18h16"
        case "columns": return box(3, 4, 18, 16, r) + "M9 4v16M15 4v16"
        case "preview": return box(3, 5, 18, 14, r) + "M14 5v14"
        // One per sort order: a slim arrow on the left (down for the usual
        // direction, up for oldest first), and what it sorts by on the right,
        // kept apart so both read at toolbar size.
        case "sort-name": return "M6 4v16M3 17l3 3 3-3M12 11l3-7 3 7M13.2 8.4h3.6M12 14h6l-6 6h6"
        case "sort-kind": return "M6 4v16M3 17l3 3 3-3" + circle(16.5, 7.5, 3) + box(13.5, 14, 6, 6, r / 2)
        case "sort-newest": return "M6 4v16M3 17l3 3 3-3" + circle(16.5, 12, 5) + "M16.5 9v3.2l2.2 1.4"
        case "sort-oldest": return "M6 20V4M3 7l3-3 3 3" + circle(16.5, 12, 5) + "M16.5 9v3.2l2.2 1.4"
        case "sort-size": return "M6 4v16M3 17l3 3 3-3M12 6h9M12 12h6.5M12 18h4"
        case "play": return "M8 5v14l11-7z"
        case "pause": return "M8 5v14M16 5v14"
        case "open": return "M14 4h6v6M20 4l-9 9M18 14v6H4V6h6"
        case "audio": return "M4 10v4M8 7v10M12 4v16M16 8v8M20 11v2"
        case "check": return "M5 12l5 5 9-10"
        case "close": return "M6 6l12 12M18 6L6 18"
        default: return page()
        }
    }

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: glyph.filled ? "transparent" : glyph.color
            fillColor: glyph.filled ? glyph.color : "transparent"
            strokeWidth: glyph.stroke
            capStyle: theme.radius > 0 ? ShapePath.RoundCap : ShapePath.SquareCap
            joinStyle: theme.radius > 0 ? ShapePath.RoundJoin : ShapePath.MiterJoin
            scale: Qt.size(glyph.size / 24, glyph.size / 24)
            PathSvg { path: glyph.path }
        }
    }
}
