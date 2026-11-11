import QtQuick
import QtQuick.Controls

// The folder as a list: name, size and date (the date, then the size, step
// aside in a narrow window). No headings: the sort menu and S sort it.
ListView {
    id: list

    function select(row, keep) { selecting.select(row, keep) }
    // A click on empty space lets go of the cursor; the pane then keeps it
    // let go of through a re-read or a switch of view.
    signal cursorLetGo()
    function letGo() { selecting.letGo(); cursorLetGo() }
    function focusView() { forceActiveFocus() }
    // Where the keyboard's menu (Alt+Return) opens: beside the cursor's item.
    function menuAnchor() { return currentItem }

    Selecting { id: selecting; view: list }
    onCurrentIndexChanged: selecting.cursorMoved()
    Keys.onPressed: (event) => selecting.extendBy(event, event.key === Qt.Key_Up ? -1 : event.key === Qt.Key_Down ? 1 : 0)

    // Every row is this tall, so the band can find them without asking.
    readonly property int rowHeight: Math.max(32, theme.scaled(32))

    Band {
        view: list
        rectOf: (i) => Qt.rect(0, i * list.rowHeight, list.width, list.rowHeight)
    }

    // → goes into the selected folder and ← back up, as in the columns.
    Keys.onRightPressed: {
        const item = backend.folder.get(currentIndex)
        if (item.isDir)
            backend.navigate(item.path)
    }
    Keys.onLeftPressed: backend.goUp()

    // Right-click on an item (selecting it) or on empty space.
    signal contextMenu(var item, string folder)
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: (point) => list.contextMenu(selecting.menuAt(point), backend.path)
    }

    clip: true
    focus: true
    keyNavigationEnabled: true
    // No cursor of its own to start with: the pane puts it back where the
    // last view had it, and one on the first item would select that instead.
    currentIndex: -1
    acceptedButtons: Qt.NoButton
    boundsBehavior: Flickable.StopAtBounds
    highlightFollowsCurrentItem: false
    model: backend.folder
    ScrollBar.vertical: FolioScrollBar { }

    delegate: FileRow {
        width: list.width
        height: list.rowHeight
        detailed: true
        current: selected
        onClicked: (modifiers) => { selecting.click(index, modifiers); list.forceActiveFocus() }
        onActivated: backend.open(path)
    }

    Text {
        anchors.centerIn: parent
        visible: list.count === 0 && !backend.folder.loading && backend.folder.error.length === 0
        text: backend.folder.query ? qsTr("Nothing matches “%1”").arg(backend.folder.query) : qsTr("This folder is empty")
        color: theme.secondaryForeground
    }
}
