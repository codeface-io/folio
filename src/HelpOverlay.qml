import QtQuick
import QtQuick.Layouts

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
        ["Ctrl+L", qsTr("Type a path (Tab completes)")],
        ["/", qsTr("Search this folder and below")],
        ["Ctrl+1 / 2 / 3", qsTr("Grid, list or columns")],
        ["Space", qsTr("Preview the selected file")],
        ["Ctrl+I", qsTr("Show or hide the details pane")],
        ["Ctrl+D", qsTr("Pin or unpin the folder (or drag it to the sidebar)")],
        ["Ctrl+C / X / V", qsTr("Copy, cut, paste files")],
        ["Ctrl+Shift+C", qsTr("Copy the path as text")],
        ["F2 / Ctrl+Shift+D", qsTr("Rename / duplicate")],
        ["Ctrl+N", qsTr("New window")],
        ["Ctrl+Shift+N", qsTr("New folder")],
        ["Ctrl+Alt+N / Ctrl+W", qsTr("Split into two panes / close the right one")],
        ["Delete", qsTr("Move to the trash")],
        ["Right-click / Alt+⏎", qsTr("Everything else")],
        ["S / Shift+S", qsTr("Sort: name, kind, newest, oldest, size")],
        ["Ctrl+H", qsTr("Show hidden files")],
        ["F5", qsTr("Reload")],
        ["?", qsTr("Show these shortcuts")],
        ["Q", qsTr("Close the window")]
    ]

    MouseArea { anchors.fill: parent; onClicked: overlay.dismissed() }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 520)
        height: content.implicitHeight + 56
        color: theme.background
        border.width: 1
        border.color: theme.accent
        radius: theme.radius

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: 28
            spacing: 14

            Text { text: qsTr("Keyboard shortcuts"); color: theme.brightForeground; font.pixelSize: theme.scaled(16); font.weight: Font.DemiBold }

            GridLayout {
                columns: 2
                rowSpacing: 10
                columnSpacing: 28
                // Each key, then what it does: the grid fills its two
                // columns in order. (QML's JavaScript has no Array.flat.)
                Repeater {
                    model: [].concat(...overlay.shortcuts)
                    delegate: Text {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: index % 2 === 1
                        text: modelData
                        color: index % 2 ? theme.foreground : theme.accent
                    }
                }
            }
        }
    }
}
