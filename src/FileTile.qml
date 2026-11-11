import QtQuick
import QtQuick.Shapes
import QtQuick.Effects
import "Format.js" as Format

// One item in the grid: a preview on top, its name below. Folders are a
// quiet card; files show a real thumbnail when one
// can be made, and a drawn stand-in for their kind when not.
Item {
    id: tile

    required property int index
    required property string name
    required property string path
    required property bool isDir
    required property string kind
    required property string mime
    required property string extension
    required property int itemCount
    required property var size
    required property var modified
    required property bool selected

    property bool current: false

    signal clicked(int modifiers)
    signal activated()

    // Pictures and drawings round their corners with the theme: square on
    // Omarchy's default, softly rounded on themes that round windows.
    readonly property real iconRadius: Math.min(10, theme.radius)

    // The badge and stand-in color for each kind, from the theme's palette.
    readonly property color kindColor: kind === "pdf" ? theme.red
                                     : kind === "text" ? theme.blue
                                     : kind === "audio" ? theme.magenta
                                     : kind === "video" ? theme.orange
                                     : kind === "archive" ? theme.yellow
                                     : kind === "image" ? theme.cyan
                                     : theme.secondaryForeground

    Item {
        id: art
        width: 128
        height: 92
        anchors.top: parent.top
        anchors.topMargin: 18
        anchors.horizontalCenter: parent.horizontalCenter

        Loader {
            anchors.fill: parent
            sourceComponent: tile.isDir ? folderArt : fileArt
        }

        // A folder takes files dropped on it, lighting up while they hover.
        DropArea {
            id: folderDrop
            anchors.fill: parent
            enabled: tile.isDir
            keys: ["text/uri-list"]
            onEntered: (drag) => { drag.accepted = fileOps.canDrop(drag.urls, tile.path) }
            onDropped: (drop) => { fileOps.dropInto(drop.urls, tile.path); drop.accept(Qt.MoveAction) }
        }
    }

    Text {
        id: label
        anchors.top: art.bottom
        anchors.topMargin: 12
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 28
        text: tile.name
        color: tile.current ? theme.accent : theme.foreground
        font.pixelSize: theme.scaled(12)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        maximumLineCount: 2
        elide: Text.ElideRight
        lineHeight: 1.15
    }

    // Size and when, quietly, under the name: "28.0 KB · 2:14 PM" today,
    // "28.0 KB · 11/9/26" before; the item count for a folder.
    Text {
        anchors.top: label.bottom
        anchors.topMargin: 3
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 28
        text: {
            translator.language // Re-read when the language (and so the default locale) changes.
            const size = tile.isDir ? Format.count(tile.itemCount) : Format.size(tile.size)
            const date = Format.when(tile.modified)
            return [size, date].filter(part => part.length > 0).join(" · ")
        }
        color: theme.secondaryForeground
        font.pixelSize: theme.scaled(10)
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
    }

    HoverHandler {
        id: hover
        // The picture that follows the pointer while dragging; taken ahead
        // of time because a drag can't wait for it.
        onHoveredChanged: if (hovered) art.grabToImage(result => tile.Drag.imageSource = result.url)
    }

    // Dragging hands the file to whatever it lands on as a URI list: the
    // sidebar pins a folder, other apps get the file.
    Drag.dragType: Drag.Automatic
    Drag.supportedActions: Qt.CopyAction | Qt.LinkAction
    Drag.hotSpot.x: tile.width / 2
    Drag.hotSpot.y: 40
    DragHandler {
        target: null
        // Dragging a selected item takes the whole selection along. The list
        // is built as the drag starts, not again on every change of selection.
        onActiveChanged: {
            if (active)
                tile.Drag.mimeData = { "text/uri-list": fileOps.uriList(tile.selected && backend.folder.selectedCount > 1 ? backend.folder.selectedPaths() : [tile.path]) }
            tile.Drag.active = active
        }
    }

    TapHandler {
        id: tap
        onTapped: tile.clicked(tap.point.modifiers)
        onDoubleTapped: tile.activated()
    }

    // A folder's outline: a card with a tab on its top left edge, drawn as
    // one silhouette, its corners rounded by `r`.
    function folderPath(r) {
        if (r <= 0)
            return "M4.5 88.5V0.5H46L50 6.5H123.5V88.5Z"
        // Corners as quadratic curves: the curve renderer draws those
        // natively (filled arcs went missing on the GPU).
        const t = Math.min(r, 3)
        return "M4.5 " + (88.5 - r) + "V" + (0.5 + r) + "Q4.5 0.5 " + (4.5 + r) + " 0.5"
             + "H" + (46 - t) + "Q46 0.5 " + (46 + t * 0.6) + " " + (0.5 + t * 0.9) + "L" + (50 - t * 0.6) + " " + (6.5 - t * 0.9)
             + "Q50 6.5 " + (50 + t) + " 6.5H" + (123.5 - r) + "Q123.5 6.5 123.5 " + (6.5 + r)
             + "V" + (88.5 - r) + "Q123.5 88.5 " + (123.5 - r) + " 88.5H" + (4.5 + r)
             + "Q4.5 88.5 4.5 " + (88.5 - r) + "Z"
    }

    // Folders are a plain card, nothing on it; the name below says the rest.
    Component {
        id: folderArt

        Shape {
            width: 128
            height: 92
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: tile.current || folderDrop.containsDrag ? Qt.tint(theme.lighterBackground, Qt.alpha(theme.accent, 0.3))
                    : hover.hovered ? Qt.tint(theme.lighterBackground, Qt.alpha(theme.foreground, 0.06))
                    : theme.lighterBackground
                strokeColor: tile.current || folderDrop.containsDrag ? theme.accent : "transparent"
                strokeWidth: folderDrop.containsDrag ? 2.5 : 1.5
                joinStyle: tile.iconRadius > 0 ? ShapePath.RoundJoin : ShapePath.MiterJoin
                PathSvg { path: tile.folderPath(tile.iconRadius) }
            }
        }
    }

    Component {
        id: fileArt

        Item {
            // The picture is laid out at its own shape (cropped to fill for
            // photos and video, fitted for pages), so its corners can round.
            readonly property bool crop: tile.kind === "image" || tile.kind === "video"
            readonly property size natural: thumb.implicitWidth > 0 ? Qt.size(thumb.implicitWidth, thumb.implicitHeight) : Qt.size(1, 1)
            readonly property real fit: Math.min(width / natural.width, height / natural.height)
            Image {
                id: thumb
                width: parent.crop ? parent.width : parent.natural.width * parent.fit
                height: parent.crop ? parent.height : parent.natural.height * parent.fit
                x: (parent.width - width) / 2
                y: (parent.height - height) / 2
                asynchronous: true
                fillMode: parent.crop ? Image.PreserveAspectCrop : Image.Stretch
                source: tile.kind === "text" ? "" : Format.thumb(tile.path, tile.mime, 256, tile.modified)
                visible: status === Image.Ready
                layer.enabled: tile.iconRadius > 0 && status === Image.Ready
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: thumbMask
                    maskThresholdMin: 0.5
                    maskSpreadAtMin: 1
                }
            }
            Rectangle {
                id: thumbMask
                width: thumb.width
                height: thumb.height
                radius: tile.iconRadius
                visible: false
                layer.enabled: true
            }

            // Videos get a play mark over their frame.
            Rectangle {
                visible: thumb.status === Image.Ready && tile.kind === "video"
                anchors.centerIn: parent
                width: 30; height: 30
                radius: Math.min(4, theme.radius)
                color: Qt.alpha(theme.darkBackground, 0.75)
                Glyph { anchors.centerIn: parent; name: "play"; size: 16; color: theme.brightForeground }
            }

            // Stand-ins while a thumbnail loads, or when none can be made.
            Loader {
                anchors.fill: parent
                active: thumb.status !== Image.Ready
                sourceComponent: tile.kind === "audio" ? waveArt : tile.kind === "video" ? reelArt : pageArt
            }

            // Selection is a border on what's drawn, hugging it: the
            // thumbnail at its painted size, the page stand-in, or the box
            // of the others.
            readonly property rect shown: {
                const w = width, h = height
                if (thumb.status === Image.Ready)
                    return Qt.rect(thumb.x, thumb.y, thumb.width, thumb.height)
                if (tile.kind === "audio" || tile.kind === "video")
                    return Qt.rect(0, 0, w, h)
                return Qt.rect((w - 66) / 2, 0, 66, h)
            }
            Rectangle {
                visible: tile.current || hover.hovered
                x: parent.shown.x - 3
                y: parent.shown.y - 3
                width: parent.shown.width + 6
                height: parent.shown.height + 6
                color: "transparent"
                border.width: tile.current ? 2 : 1
                border.color: tile.current ? theme.accent : Qt.alpha(theme.foreground, 0.2)
                radius: tile.iconRadius > 0 ? tile.iconRadius + 3 : 0
            }
        }
    }

    Component {
        id: pageArt

        Item {
            Rectangle {
                id: page
                anchors.horizontalCenter: parent.horizontalCenter
                width: 66; height: 92
                color: theme.lighterBackground
                border.width: 1
                border.color: Qt.alpha(theme.foreground, 0.16)
                radius: tile.iconRadius
                Column {
                    x: 9; y: 11
                    spacing: 6
                    Rectangle { width: 34; height: 4; color: tile.kindColor }
                    Repeater {
                        model: [48, 48, 40, 48, 30]
                        Rectangle { required property int modelData; width: modelData; height: 2; color: Qt.alpha(theme.foreground, 0.3) }
                    }
                }
            }
            Rectangle {
                visible: tile.extension.length > 0
                anchors.left: page.left
                anchors.bottom: page.bottom
                anchors.bottomMargin: 6
                width: badge.implicitWidth + 8
                height: badge.implicitHeight + 2
                color: tile.kindColor
                Text { id: badge; anchors.centerIn: parent; text: tile.extension.slice(0, 4); font.pixelSize: theme.scaled(9); font.bold: true; color: theme.background }
            }
        }
    }

    Component {
        id: waveArt

        // Decorative, not the real waveform: bars seeded from the name so each
        // file keeps its own shape. Album art replaces it when the file has some.
        Rectangle {
            color: theme.lighterBackground
            radius: tile.iconRadius
            Row {
                anchors.centerIn: parent
                spacing: 3
                Repeater {
                    model: 18
                    Rectangle {
                        required property int index
                        anchors.verticalCenter: parent.verticalCenter
                        width: 3
                        height: {
                            let h = 0
                            for (let i = 0; i < tile.name.length; ++i) h = (h * 31 + tile.name.charCodeAt(i) + index * 7) % 997
                            return 10 + (h % 40)
                        }
                        color: tile.kindColor
                    }
                }
            }
            Text { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 5; text: tile.extension; font.pixelSize: theme.scaled(9); color: theme.secondaryForeground }
        }
    }

    Component {
        id: reelArt

        Rectangle {
            color: theme.darkBackground
            radius: tile.iconRadius
            Glyph { anchors.centerIn: parent; name: "play"; size: 22; color: tile.kindColor }
        }
    }
}
