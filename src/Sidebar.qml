import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Home's folders, pinned folders, views, drives and tailnet machines. The one holding the current folder
// stays lit while you dig into it, so you always know which root you're under.
Rectangle {
    id: sidebar

    color: theme.background
    // A long tailnet scrolls inside the sidebar instead of spilling over
    // the status bar below it.
    clip: true

    // Keyboard: Tab moves here from the files and back. ↑ ↓ move between
    // rows, ⏎ (or →) opens one and hands the keyboard back to the files.
    signal finished()
    property int cursor: 0
    // The pane the sidebar acts on: the one used last.
    property var target: backend

    // Something is being dragged over the sidebar (anywhere on it, rows
    // included), so the pin box shows.
    property int dropHovers: 0
    readonly property bool dragging: drop.containsDrag || dropHovers > 0 || pinDrop.containsDrag
    readonly property var rows: [].concat(places.places, places.pinned, places.views, places.devices, places.peers)
    readonly property int pinnedAt: places.places.length
    readonly property int viewsAt: pinnedAt + places.pinned.length
    readonly property int devicesAt: viewsAt + places.views.length
    readonly property int peersAt: devicesAt + places.devices.length

    function focusSidebar() {
        // Start on the row for where we are, or the top.
        let at = 0
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].path === activePath)
                at = i
        cursor = at
        forceActiveFocus()
    }
    function activate(i) {
        const row = rows[i]
        if (!row)
            return
        if (row.host !== undefined)
            sidebar.target.openPeer(row.host)
        else
            sidebar.target.navigate(row.path)
        finished()
    }
    function reveal(item) {
        const y = item.mapToItem(column, 0, 0).y
        if (y < flick.contentY)
            flick.contentY = Math.max(0, y - 20)
        else if (y + item.height > flick.contentY + flick.height)
            flick.contentY = y + item.height - flick.height + 20
    }

    Keys.onUpPressed: cursor = Math.max(0, cursor - 1)
    Keys.onDownPressed: cursor = Math.min(rows.length - 1, cursor + 1)
    Keys.onReturnPressed: activate(cursor)
    Keys.onEnterPressed: activate(cursor)
    Keys.onRightPressed: activate(cursor)
    Keys.onEscapePressed: finished()

    // The deepest place the current folder sits under.
    readonly property string activePath: {
        let best = ""
        const here = sidebar.target.path
        for (const list of [places.places, places.pinned, places.views, places.devices]) {
            for (const p of list) {
                if (p.path !== "/" && (here === p.path || here.startsWith(p.path + "/")) && p.path.length > best.length)
                    best = p.path
            }
        }
        return best
    }

    function statusText(status) {
        // Up to date is the normal state and says nothing; only syncing or
        // a stopped Dropbox is worth a word.
        return status === "syncing" ? qsTr("syncing")
             : status === "stopped" ? qsTr("stopped") : ""
    }

    component SectionLabel: Text {
        Layout.topMargin: 21
        Layout.leftMargin: 14
        Layout.bottomMargin: 6
        color: theme.secondaryForeground
        font.pixelSize: theme.scaled(11)
        font.letterSpacing: 1.3
        font.capitalization: Font.AllUppercase
    }

    component PlaceRow: AbstractButton {
        id: row
        property string glyph
        property bool active: false
        // The keyboard is on this row (the sidebar has focus).
        property bool keyed: false
        // Files dropped here go into this folder, or into the trash.
        property string dropPath
        property bool dropTrashes: false
        readonly property bool dropHover: rowDrop.containsDrag
        property string status
        property color statusColor

        Layout.fillWidth: true
        implicitHeight: Math.max(34, theme.scaled(34))
        leftPadding: 14
        rightPadding: 14
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        Accessible.name: text

        background: Rectangle {
            radius: Math.min(4, theme.radius)
            color: row.active || row.dropHover ? Qt.alpha(theme.accent, 0.16)
                 : row.hovered && row.enabled ? Qt.alpha(theme.foreground, 0.06) : "transparent"
            border.width: row.dropHover ? 2 : row.keyed ? 1 : 0
            border.color: theme.accent
        }

        DropArea {
            id: rowDrop
            anchors.fill: parent
            enabled: row.dropPath.length > 0 || row.dropTrashes
            keys: ["text/uri-list"]
            onEntered: (drag) => { drag.accepted = row.dropTrashes ? drag.hasUrls : fileOps.canDrop(drag.urls, row.dropPath) }
            onDropped: (drop) => {
                if (row.dropTrashes) {
                    fileOps.trash(fileOps.localPaths(drop.urls))
                } else {
                    fileOps.dropInto(drop.urls, row.dropPath)
                }
                drop.accept(Qt.MoveAction)
            }
        }
        contentItem: RowLayout {
            spacing: 14
            Glyph { name: row.glyph; color: row.active ? theme.brightForeground : theme.foreground }
            Text {
                Layout.fillWidth: true
                text: row.text
                elide: Text.ElideRight
                color: row.active ? theme.brightForeground : theme.foreground
            }
            Text { visible: row.status.length > 0; text: row.status; color: row.statusColor; font.pixelSize: theme.scaled(11) }
        }
    }

    // Notices a drag over the sidebar, to show the pin box; rows and the
    // box take the drop itself.
    DropArea {
        id: drop
        anchors.fill: parent
        keys: ["text/uri-list"]
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.implicitHeight + 40
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: column
            x: 12
            y: 20
            width: sidebar.width - 24
            spacing: 1

            // The section's label is home itself: click it to go there.
            SectionLabel {
                Layout.topMargin: 0
                text: qsTr("Home")
                color: sidebar.target.path === places.home ? theme.accent
                     : homeHover.hovered ? theme.foreground : theme.secondaryForeground
                Accessible.role: Accessible.Button
                HoverHandler { id: homeHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: sidebar.target.navigate(places.home) }
            }
            Repeater {
                model: places.places
                PlaceRow {
                    required property var modelData
                    required property int index
                    dropPath: modelData.path
                    onDropHoverChanged: sidebar.dropHovers += dropHover ? 1 : -1
                    keyed: sidebar.activeFocus && sidebar.cursor === 0 + index
                    onKeyedChanged: if (keyed) sidebar.reveal(this)
                    text: modelData.name
                    glyph: modelData.icon
                    active: sidebar.activePath === modelData.path
                    status: sidebar.statusText(modelData.status)
                    statusColor: modelData.status === "stopped" ? theme.red : theme.yellow
                    onClicked: sidebar.target.navigate(modelData.path)
                }
            }

            // Pinned folders from anywhere, kept in GTK's bookmarks. While a
            // folder is dragged over the sidebar, the section shows (even
            // empty) as the place to drop it.
            SectionLabel { text: qsTr("Pinned"); visible: places.pinned.length > 0 || sidebar.dragging }
            Repeater {
                model: places.pinned
                PlaceRow {
                    id: pinRow
                    required property var modelData
                    required property int index
                    dropPath: modelData.path
                    onDropHoverChanged: sidebar.dropHovers += dropHover ? 1 : -1
                    keyed: sidebar.activeFocus && sidebar.cursor === sidebar.pinnedAt + index
                    onKeyedChanged: if (keyed) sidebar.reveal(this)
                    text: modelData.name
                    glyph: modelData.icon
                    active: sidebar.activePath === modelData.path
                    onClicked: sidebar.target.navigate(modelData.path)
                    IconButton {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 28
                        implicitHeight: 28
                        visible: pinRow.hovered || hovered
                        glyph: "close"
                        text: qsTr("Unpin")
                        onClicked: places.unpin(pinRow.modelData.path)
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Math.max(34, theme.scaled(34))
                visible: sidebar.dragging
                color: Qt.alpha(theme.accent, pinDrop.containsDrag ? 0.22 : 0.08)
                border.width: pinDrop.containsDrag ? 2 : 1
                border.color: theme.accent
                radius: Math.min(4, theme.radius)
                Text {
                    anchors.centerIn: parent
                    text: qsTr("Drop to pin")
                    color: theme.accent
                    font.pixelSize: theme.scaled(12)
                }
                DropArea {
                    id: pinDrop
                    anchors.fill: parent
                    keys: ["text/uri-list"]
                    onDropped: (drop) => {
                        for (const path of fileOps.localPaths(drop.urls)) {
                            const problem = places.pin(path)
                            if (problem.length > 0)
                                sidebar.target.showMessage(problem)
                        }
                        drop.accept(Qt.LinkAction)
                    }
                }
            }

            // Views: lists that aren't a folder of their own.
            SectionLabel { text: qsTr("Views") }
            Repeater {
                model: places.views
                PlaceRow {
                    required property var modelData
                    required property int index
                    dropTrashes: modelData.icon === "trash"
                    onDropHoverChanged: sidebar.dropHovers += dropHover ? 1 : -1
                    text: modelData.name
                    glyph: modelData.icon
                    active: sidebar.activePath === modelData.path
                    onClicked: sidebar.target.navigate(modelData.path)
                    keyed: sidebar.activeFocus && sidebar.cursor === sidebar.viewsAt + index
                    onKeyedChanged: if (keyed) sidebar.reveal(this)
                }
            }

            SectionLabel { text: qsTr("Devices"); visible: places.devices.length > 0 }
            Repeater {
                model: places.devices
                PlaceRow {
                    required property var modelData
                    required property int index
                    dropPath: modelData.path
                    onDropHoverChanged: sidebar.dropHovers += dropHover ? 1 : -1
                    keyed: sidebar.activeFocus && sidebar.cursor === sidebar.devicesAt + index
                    onKeyedChanged: if (keyed) sidebar.reveal(this)
                    text: modelData.name
                    glyph: modelData.icon
                    active: sidebar.activePath === modelData.path
                    onClicked: sidebar.target.navigate(modelData.path)
                }
            }

            SectionLabel { text: qsTr("Tailnet"); visible: places.peers.length > 0 }
            Repeater {
                model: places.peers
                PlaceRow {
                    required property var modelData
                    required property int index
                    keyed: sidebar.activeFocus && sidebar.cursor === sidebar.peersAt + index
                    onKeyedChanged: if (keyed) sidebar.reveal(this)
                    text: modelData.name
                    glyph: "peer"
                    active: sidebar.target.path.indexOf("/sftp:host=" + modelData.host) >= 0
                    onClicked: sidebar.target.openPeer(modelData.host)
                }
            }
        }
    }
}
