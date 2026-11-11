import QtQuick
import QtQuick.Layouts
import QtQuick.Effects
import "Format.js" as Format

// One item as a row, for the list and column views: a small picture or
// glyph, the name, and in the list the size and date beside it.
Item {
    id: row

    required property int index
    required property string name
    required property string path
    required property bool isDir
    required property string kind
    required property string mime
    required property var size
    required property var modified
    required property int itemCount
    required property bool selected

    property bool current: false
    // In a column to the left: the folder the next column is showing.
    property bool trail: false
    property bool detailed: false
    property bool chevron: false

    signal clicked(int modifiers)
    signal activated()

    implicitHeight: Math.max(32, theme.scaled(32))

    // The name always gets room: in a narrow window the date goes first,
    // then the size. (The icon says what kind a file is; there's no column
    // for it.)
    readonly property bool showDate: detailed && width >= theme.scaled(480)
    readonly property bool showSize: detailed && width >= theme.scaled(380)

    readonly property string glyph: row.isDir ? "folder"
                                  : row.kind === "image" ? "pictures"
                                  : row.kind === "video" ? "videos"
                                  : row.kind === "audio" ? "music"
                                  : row.kind === "pdf" || row.kind === "text" ? "documents"
                                  : "file"

    // A folder row takes files dropped on it, lighting up while they hover.
    DropArea {
        id: folderDrop
        anchors.fill: parent
        enabled: row.isDir
        keys: ["text/uri-list"]
        onEntered: (drag) => { drag.accepted = fileOps.canDrop(drag.urls, row.path) }
        onDropped: (drop) => { fileOps.dropInto(drop.urls, row.path); drop.accept(Qt.MoveAction) }
    }

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        radius: Math.min(4, theme.radius)
        border.width: folderDrop.containsDrag ? 2 : 0
        border.color: theme.accent
        color: row.current || folderDrop.containsDrag ? Qt.alpha(theme.accent, 0.18)
             : row.trail ? Qt.alpha(theme.foreground, 0.08)
             : hover.hovered ? Qt.alpha(theme.foreground, 0.05)
             : "transparent"
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 12

        Item {
            implicitWidth: 20
            implicitHeight: 20
            Image {
                id: thumb
                anchors.fill: parent
                layer.enabled: theme.radius > 0 && status === Image.Ready
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: rowMask
                    maskThresholdMin: 0.5
                    maskSpreadAtMin: 1
                }
                asynchronous: true
                fillMode: Image.PreserveAspectCrop
                source: row.kind === "image" || row.kind === "video" ? Format.thumb(row.path, row.mime, 128, row.modified) : ""
                visible: status === Image.Ready
            }
            Rectangle {
                id: rowMask
                width: 20
                height: 20
                radius: Math.min(4, theme.radius)
                visible: false
                layer.enabled: true
            }
            Glyph {
                anchors.centerIn: parent
                visible: !thumb.visible
                name: row.glyph
                size: 16
                color: row.isDir ? theme.accent : theme.secondaryForeground
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.minimumWidth: 120
            text: row.name
            color: row.current ? theme.accent : theme.foreground
            elide: Text.ElideMiddle
        }
        Text {
            visible: row.showSize
            Layout.preferredWidth: theme.scaled(96)
            horizontalAlignment: Text.AlignRight
            text: row.isDir ? Format.count(row.itemCount) : Format.size(row.size)
            color: theme.secondaryForeground
            font.pixelSize: theme.scaled(12)
            elide: Text.ElideRight
        }
        Text {
            visible: row.showDate
            Layout.preferredWidth: theme.scaled(96)
            Layout.leftMargin: 12
            text: {
                translator.language // Re-read when the language (and so the default locale) changes.
                return Format.when(row.modified)
            }
            color: theme.secondaryForeground
            font.pixelSize: theme.scaled(12)
            elide: Text.ElideRight
        }
        Glyph {
            visible: row.chevron && row.isDir
            name: "forward"
            size: 12
            color: theme.secondaryForeground
        }
    }

    HoverHandler {
        id: hover
        onHoveredChanged: if (hovered) row.grabToImage(result => row.Drag.imageSource = result.url)
    }
    TapHandler {
        id: tap
        onTapped: row.clicked(tap.point.modifiers)
        onDoubleTapped: row.activated()
    }

    // Like the tiles: dragged, a row is a file for the sidebar or another app.
    Drag.dragType: Drag.Automatic
    Drag.supportedActions: Qt.CopyAction | Qt.LinkAction
    DragHandler {
        target: null
        // Dragging a selected item takes the whole selection along. The list
        // is built as the drag starts, not again on every change of selection.
        onActiveChanged: {
            if (active)
                row.Drag.mimeData = { "text/uri-list": fileOps.uriList(row.selected && backend.folder.selectedCount > 1 ? backend.folder.selectedPaths() : [row.path]) }
            row.Drag.active = active
        }
    }
}
