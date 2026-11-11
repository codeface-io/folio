import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Format.js" as Format

// A larger look at the selected item and the facts about it, with the one
// action that matters: open it.
Rectangle {
    id: pane

    // The pane whose selected item this shows, and whose backend and file
    // work the button acts through: in a split, the one used last.
    property var folderPane: null
    // Hidden, it shows nothing, so moving through files doesn't read them,
    // look for their trash notes or make large thumbnails for nobody.
    readonly property var item: visible && folderPane ? folderPane.current : ({})
    readonly property bool hasItem: item !== undefined && item.path !== undefined
    // An item in the trash shows where it came from and when it was
    // deleted, and offers Restore rather than Open. Its note is read in the
    // background; until it's in, the item looks trashed, as nearly all there
    // are, but Restore waits. What's inside a trashed folder has no note.
    readonly property bool noting: hasItem && folderPane.inTrash && info.origin === undefined
    readonly property string origin: info.origin || ""
    readonly property bool trashed: hasItem && origin.length > 0
    // A tailnet machine shows its system and host, and has no date.
    readonly property bool machine: hasItem && item.kind === "peer"
    // The drive whose top folder is selected or shown, for its free space.
    readonly property var drive: {
        const shown = folderPane ? folderPane.paneBackend.path : ""
        for (const d of places.devices)
            if (d.free >= 0 && ((hasItem && d.path === item.path) || d.path === shown))
                return d
        return null
    }
    function homeless(path) { return path.startsWith(places.home + "/") ? "~" + path.slice(places.home.length) : path }
    // Its type's name, and a picture's size or a text's opening lines once
    // they've been read off the UI thread.
    readonly property var info: backend.details
    // Described once the folder's loaded has put the cursor back on its
    // item: a re-read leaves another item on the cursor's row for a moment,
    // and describing that would drop what's been read of this one.
    onItemChanged: Qt.callLater(describe)
    function describe() { backend.describe(item, !!folderPane && folderPane.inTrash) }

    color: theme.darkBackground

    Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: Qt.alpha(theme.foreground, 0.12) }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 22
        visible: pane.hasItem

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(width * 0.75)

            Image {
                id: big
                anchors.fill: parent
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                source: pane.hasItem && !pane.item.isDir && pane.item.kind !== "text" && !pane.machine ? Format.thumb(pane.item.path, pane.item.mime, 512, pane.item.modified) : ""
                visible: status === Image.Ready
            }

            // Text files show their opening lines instead of a picture.
            Rectangle {
                anchors.fill: parent
                visible: pane.hasItem && pane.item.kind === "text"
                color: theme.background
                border.width: 1
                border.color: Qt.alpha(theme.foreground, 0.12)
                clip: true
                Text {
                    anchors.fill: parent
                    anchors.margins: 14
                    text: pane.info.text || ""
                    color: theme.foreground
                    font.pixelSize: 10
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                }
            }

            Glyph {
                anchors.centerIn: parent
                visible: !big.visible && pane.hasItem && pane.item.kind !== "text"
                name: pane.machine ? "peer" : pane.hasItem && pane.item.isDir ? "folder" : "file"
                size: 72
                stroke: 1.2
                color: pane.hasItem && pane.item.isDir ? theme.accent : theme.secondaryForeground
            }
        }

        ColumnLayout {
            spacing: 4
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: pane.item.name || ""
                color: theme.brightForeground
                font.pixelSize: 15
                font.weight: Font.DemiBold
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 3
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: Format.summary(pane.item, pane.info.type)
                color: theme.secondaryForeground
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            rowSpacing: 12
            columnSpacing: 16

            Text { visible: pane.trashed; text: qsTr("From"); color: theme.secondaryForeground; font.pixelSize: 12 }
            Text {
                visible: pane.trashed
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: pane.trashed ? pane.homeless(pane.origin.replace(/\/[^\/]*$/, "") || "/") : ""
                color: theme.foreground
                font.pixelSize: 12
                elide: Text.ElideMiddle
            }
            Text { visible: pane.machine; text: qsTr("Host"); color: theme.secondaryForeground; font.pixelSize: 12 }
            Text {
                visible: pane.machine
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: pane.machine ? pane.item.path.slice("tailnet:".length) : ""
                color: theme.foreground
                font.pixelSize: 12
                elide: Text.ElideMiddle
            }
            Text { visible: !pane.machine; text: pane.trashed || pane.noting ? qsTr("Deleted") : qsTr("Modified"); color: theme.secondaryForeground; font.pixelSize: 12 }
            Text {
                visible: !pane.machine
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: {
                    translator.language // Re-read when the language (and so the default locale) changes.
                    const when = pane.trashed ? pane.info.deleted : pane.noting ? null : pane.item.modified
                    return pane.hasItem && when ? when.toLocaleString(Qt.locale(), Locale.ShortFormat) : ""
                }
                color: theme.foreground
                font.pixelSize: 12
            }
            Text { visible: !!pane.drive; text: qsTr("Free space"); color: theme.secondaryForeground; font.pixelSize: 12 }
            Text {
                visible: !!pane.drive
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: {
                    translator.language // Re-read when the language (and so its digits) changes.
                    return pane.drive ? Format.size(pane.drive.free) : ""
                }
                color: theme.foreground
                font.pixelSize: 12
            }
            Text { visible: !!pane.info.dimensions; text: qsTr("Dimensions"); color: theme.secondaryForeground; font.pixelSize: 12 }
            Text {
                visible: !!pane.info.dimensions
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: pane.info.dimensions || ""
                color: theme.foreground
                font.pixelSize: 12
            }
        }

        Item { Layout.fillHeight: true }

        AbstractButton {
            id: openButton
            Layout.fillWidth: true
            implicitHeight: 40
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            enabled: !pane.noting
            text: pane.trashed || pane.noting ? qsTr("Restore") : pane.hasItem && pane.item.isDir ? qsTr("Open folder") : qsTr("Open")
            Accessible.name: text
            onClicked: pane.trashed ? pane.folderPane.paneFileOps.restore([pane.item.path]) : pane.folderPane.paneBackend.open(pane.item.path)
            background: Rectangle {
                radius: Math.min(4, theme.radius)
                color: openButton.hovered ? Qt.lighter(theme.accent, 1.08) : theme.accent
            }
            contentItem: RowLayout {
                Text { Layout.leftMargin: 14; Layout.fillWidth: true; text: openButton.text; color: theme.accentForeground; font.weight: Font.DemiBold }
                Text { Layout.rightMargin: 14; text: "⏎"; color: theme.accentForeground }
            }
        }
    }

    Text {
        anchors.centerIn: parent
        visible: !pane.hasItem
        text: qsTr("Nothing selected")
        color: theme.secondaryForeground
    }
}
