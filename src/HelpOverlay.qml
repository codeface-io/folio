import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import "Format.js" as Format

// Every shortcut, one keypress away (?).
Rectangle {
    id: overlay

    signal dismissed()

    color: Qt.rgba(0, 0, 0, 0.55)

    property var shortcuts: [
        ["← ↑ → ↓", qsTr("Move the selection")],
        ["→ / ←", qsTr("In list and columns: into a folder / back out")],
        ["Shift+click / Shift+arrows", qsTr("Select a range")],
        ["Alt+click", qsTr("Add or remove one from the selection")],
        ["Drag on empty space", qsTr("Select with a rectangle (Alt adds)")],
        ["Ctrl+A", qsTr("Select everything")],
        ["Tab", qsTr("Move between the sidebar and the panes")],
        ["⏎", qsTr("Open")],
        ["Backspace", qsTr("Go to the parent folder")],
        ["Alt+← / Alt+→", qsTr("Back / forward (or the mouse's thumb buttons)")],
        ["Alt+Home", qsTr("Go home")],
        ["Ctrl+L", qsTr("Type a path, or a server's address like sftp://host/ (Tab completes)")],
        ["/", qsTr("Search this folder and below")],
        ["Ctrl+1 / 2 / 3", qsTr("Grid, list or columns")],
        ["Ctrl+= / Ctrl+- / Ctrl+0", qsTr("Bigger, smaller or normal tiles (or Ctrl+scroll)")],
        ["Space", qsTr("Preview the selected file")],
        ["Ctrl+I", qsTr("Show or hide the details pane")],
        ["Ctrl+D", qsTr("Pin or unpin the folder (or drag it to the sidebar)")],
        ["Ctrl+C / X / V", qsTr("Copy, cut, paste files")],
        ["Ctrl+Shift+C", qsTr("Copy the path as text")],
        ["F2 / Ctrl+Shift+D", qsTr("Rename / duplicate")],
        ["Ctrl+N", qsTr("New window")],
        ["Ctrl+Shift+N", qsTr("New folder")],
        ["Ctrl+Alt+N / Ctrl+W", qsTr("Split into two panes / close the right one")],
        ["Ctrl+Alt+T", qsTr("Open a terminal in this folder")],
        ["Delete", qsTr("Move to the trash")],
        ["Ctrl+Z / Ctrl+Shift+Z", qsTr("Undo / redo the last change to files (or Ctrl+Y)")],
        ["Escape", qsTr("Stop the file work under way")],
        ["Right-click / Alt+⏎", qsTr("Everything else")],
        ["S / Shift+S", qsTr("Sort: name, kind, newest, oldest, size")],
        ["Ctrl+H", qsTr("Show hidden files")],
        ["F5", qsTr("Reload")],
        ["?", qsTr("Show these shortcuts")],
        ["Q", qsTr("Close the window")]
    ]

    MouseArea { anchors.fill: parent; onClicked: overlay.dismissed() }

    // Sized to what it holds: two columns of shortcuts side by side when
    // the window is wide enough, and scrolling when it's too short for them.
    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, content.implicitWidth + 56)
        height: Math.min(parent.height - 64, content.implicitHeight + 56)
        color: theme.background
        border.width: 1
        border.color: theme.accent
        radius: theme.radius

        Flickable {
            anchors.fill: parent
            anchors.margins: 28
            contentWidth: width
            contentHeight: content.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FolioScrollBar { }

            ColumnLayout {
                id: content
                width: parent.width
                spacing: 14

                Text { text: qsTr("Keyboard shortcuts"); color: theme.brightForeground; font.pixelSize: 16; font.weight: Font.DemiBold }

                GridLayout {
                    // Pairs of key and description, two pairs a row when the
                    // window has room for them.
                    columns: overlay.width >= 1500 ? 4 : 2
                    rowSpacing: 10
                    columnSpacing: 28
                    // Each key, then what it does: the grid fills its columns
                    // in order. (QML's JavaScript has no Array.flat.)
                    Repeater {
                        model: [].concat(...overlay.shortcuts)
                        delegate: Text {
                            required property var modelData
                            required property int index
                            Layout.maximumWidth: index % 2 ? 340 : -1
                            text: index % 2 ? modelData : Format.keys(modelData)
                            wrapMode: Text.WordWrap
                            color: index % 2 ? theme.foreground : theme.accent
                        }
                    }
                }
            }
        }
    }
}
