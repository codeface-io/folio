import QtQuick
import QtQuick.Controls

// The folder as a grid of tiles with real previews.
GridView {
    id: grid

    function select(row, keep) { selecting.select(row, keep) }
    // A click on empty space lets go of the cursor; the pane then keeps it
    // let go of through a re-read or a switch of view.
    signal cursorLetGo()
    function letGo() { selecting.letGo(); cursorLetGo() }
    function focusView() { forceActiveFocus() }
    // Where the keyboard's menu (Alt+Return) opens: beside the cursor's item.
    function menuAnchor() { return currentItem }

    Selecting { id: selecting; view: grid }
    onCurrentIndexChanged: selecting.cursorMoved()
    Keys.onPressed: (event) => {
        // With no cursor (after a click on empty space), Down starts at the
        // first item, not a row's width on from before it.
        if (event.key === Qt.Key_Down && currentIndex < 0 && count > 0) {
            currentIndex = 0
            event.accepted = true
            return
        }
        selecting.extendBy(event, event.key === Qt.Key_Left ? -1 : event.key === Qt.Key_Right ? 1
                                : event.key === Qt.Key_Up ? -columns : event.key === Qt.Key_Down ? columns : 0)
    }

    Band {
        view: grid
        rectOf: (i) => Qt.rect((i % grid.columns) * grid.cellWidth + 20, Math.floor(i / grid.columns) * grid.cellHeight + 12,
                               grid.cellWidth - 40, grid.cellHeight - 24)
    }

    // Right-click on an item (selecting it) or on empty space.
    signal contextMenu(var item, string folder)
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: (point) => grid.contextMenu(selecting.menuAt(point), backend.path)
    }

    clip: true
    focus: true
    keyNavigationEnabled: true
    // No cursor of its own to start with: the pane puts it back where the
    // last view had it, and one on the first item would select that instead.
    currentIndex: -1
    // Pressing and dragging picks up a file (to pin it, or drop it in another
    // app) or draws a band, rather than flicking; the wheel and touchpad
    // still scroll, which turning the view's interactive off would stop too.
    acceptedButtons: Qt.NoButton
    boundsBehavior: Flickable.StopAtBounds
    model: backend.folder
    readonly property int columns: Math.max(1, Math.floor(width / 168))
    cellWidth: Math.floor(width / columns)
    // The picture keeps its size; the name and details under it make room
    // for larger text.
    cellHeight: Math.max(192, 148 + theme.scaled(44))
    highlightFollowsCurrentItem: false
    ScrollBar.vertical: FolioScrollBar { }

    delegate: FileTile {
        width: grid.cellWidth
        height: grid.cellHeight
        current: selected
        onClicked: (modifiers) => { selecting.click(index, modifiers); grid.forceActiveFocus() }
        onActivated: backend.open(path)
    }

    Text {
        anchors.centerIn: parent
        visible: grid.count === 0 && !backend.folder.loading && backend.folder.error.length === 0
        text: backend.folder.query ? qsTr("Nothing matches “%1”").arg(backend.folder.query) : qsTr("This folder is empty")
        color: theme.secondaryForeground
    }
}
