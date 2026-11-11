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
    // Tiles grow with the zoom; the name and details under them don't. A
    // narrow pane (one side of a split) caps it at a tile its width holds.
    property real zoom: 1
    readonly property real tileZoom: Math.max(0.5, Math.min(zoom, (width - 40) / 128))
    signal zoomRequested(int step)
    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedModifiers: Qt.ControlModifier
        // A touchpad sends many small deltas: a step per notch's worth
        // (120), and nothing for an event that doesn't move up or down.
        property real pending: 0
        onWheel: (event) => {
            // Every whole notch is a step; the rest waits for the next event.
            pending += event.angleDelta.y
            while (Math.abs(pending) >= 120) {
                const step = pending > 0 ? 1 : -1
                grid.zoomRequested(step)
                pending -= step * 120
            }
        }
    }
    // The rows reflow, so bring the cursor's tile back into view.
    onTileZoomChanged: Qt.callLater(() => { if (currentIndex >= 0) { forceLayout(); positionViewAtIndex(currentIndex, GridView.Contain) } })
    readonly property int columns: Math.max(1, Math.floor(width / (40 + 128 * tileZoom)))
    cellWidth: Math.floor(width / columns)
    // Picture follows tile zoom; the name and details under it follow the text size.
    cellHeight: 100 + Math.round(92 * tileZoom) - 44 + theme.scaled(44)
    highlightFollowsCurrentItem: false
    ScrollBar.vertical: FolioScrollBar { }

    delegate: FileTile {
        width: grid.cellWidth
        height: grid.cellHeight
        zoom: grid.tileZoom
        current: selected
        onClicked: (modifiers) => { selecting.click(index, modifiers); grid.forceActiveFocus() }
        onActivated: backend.open(path)
    }

    Text {
        anchors.centerIn: parent
        visible: grid.count === 0 && !backend.folder.loading && backend.folder.error.length === 0
        text: backend.folder.query ? qsTr("Nothing matches “%1”").arg(backend.folder.query)
            : backend.path === "tailnet:" ? qsTr("No machines are online") : qsTr("This folder is empty")
        color: theme.secondaryForeground
    }
}
