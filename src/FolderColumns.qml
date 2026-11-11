import QtQuick
import QtQuick.Controls
import Folio

// Columns, as in the macOS Finder: the folders on the way here from the
// root (~, another machine, or /), the current folder, and what's inside
// the selected subfolder. → goes into the selected folder, ← back out;
// clicking anywhere to the left jumps there.
Item {
    id: view

    property alias currentIndex: here.currentIndex
    readonly property int columnWidth: 260

    function select(row, keep) { selecting.select(row, keep) }
    signal cursorLetGo()
    function letGo() { selecting.letGo(); cursorLetGo() }

    Selecting { id: selecting; view: here }
    function focusView() { here.forceActiveFocus() }
    function menuAnchor() { return here.currentItem }

    // Every column has a menu: on an item there, or on the column's folder.
    signal contextMenu(var item, string folder)

    readonly property var crumbs: backend.crumbs

    // The folders on the way here, kept as a list model that is only trimmed
    // and extended: moving one folder deeper or back out adds or drops one
    // column, and the rest stay as they are instead of reloading (which
    // flashed).
    ListModel { id: trail }
    function syncTrail() {
        const want = []
        for (let i = 0; i + 1 < crumbs.length; ++i)
            want.push({ path: crumbs[i].path, next: crumbs[i + 1].name })
        let same = 0
        while (same < trail.count && same < want.length && trail.get(same).path === want[same].path)
            ++same
        // Columns that stay may now point at a different next folder.
        for (let i = 0; i < same; ++i)
            if (trail.get(i).next !== want[i].next)
                trail.setProperty(i, "next", want[i].next)
        if (trail.count > same)
            trail.remove(same, trail.count - same)
        for (let i = same; i < want.length; ++i)
            trail.append(want[i])
    }
    onCrumbsChanged: syncTrail()
    Component.onCompleted: syncTrail()
    readonly property var selected: here.currentIndex >= 0 ? (backend.folder.count, backend.folder.get(here.currentIndex)) : ({})

    // Keeps the newest columns in view as the trail grows.
    function revealEnd() { flick.contentX = Math.max(0, flick.contentWidth - flick.width) }

    component Separator: Rectangle { width: 1; height: parent ? parent.height : 0; color: Qt.alpha(theme.foreground, 0.1) }

    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        acceptedButtons: Qt.NoButton
        contentWidth: row.width
        contentHeight: height
        boundsBehavior: Flickable.StopAtBounds
        onContentWidthChanged: view.revealEnd()
        onWidthChanged: view.revealEnd()
        ScrollBar.horizontal: FolioScrollBar { }

        Row {
            id: row
            height: flick.height

            // The folders on the way here, each showing which way the trail goes.
            Repeater {
                model: trail
                Row {
                    id: ancestor
                    required property string path
                    required property string next
                    height: row.height

                    ListView {
                        id: column
                        width: view.columnWidth
                        height: parent.height
                        clip: true
                        acceptedButtons: Qt.NoButton
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: FolioScrollBar { }
                        model: FolderModel {
                            id: folder
                            path: ancestor.path
                            showHidden: backend.folder.showHidden
                            sortMode: backend.folder.sortMode
                            onLoaded: column.revealNext()
                        }
                        function revealNext() { positionViewAtIndex(Math.max(0, folder.indexOf(ancestor.next)), ListView.Contain) }
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: (point) => {
                                const index = column.indexAt(point.position.x + column.contentX, point.position.y + column.contentY)
                                view.contextMenu(index >= 0 ? folder.get(index) : null, ancestor.path)
                            }
                        }
                        Connections {
                            target: ancestor
                            function onNextChanged() { column.revealNext() }
                        }
                        delegate: FileRow {
                            width: column.width
                            chevron: true
                            trail: name === ancestor.next
                            onClicked: isDir ? backend.navigate(path) : backend.showItem(path)
                            onActivated: backend.open(path)
                        }
                    }
                    Separator { }
                }
            }

            // The folder being shown: its selection is the window's.
            ListView {
                id: here
                width: view.columnWidth
                height: row.height
                clip: true
                focus: true
                keyNavigationEnabled: true
                // No cursor of its own to start with: the pane puts it back
                // where the last view had it, and one on the first item would
                // select that instead.
                currentIndex: -1
                acceptedButtons: Qt.NoButton
                boundsBehavior: Flickable.StopAtBounds
                highlightFollowsCurrentItem: false
                model: backend.folder
                ScrollBar.vertical: FolioScrollBar { }

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => view.contextMenu(selecting.menuAt(point), backend.path)
                }

                onCurrentIndexChanged: selecting.cursorMoved()
                Keys.onPressed: (event) => selecting.extendBy(event, event.key === Qt.Key_Up ? -1 : event.key === Qt.Key_Down ? 1 : 0)
                Keys.onRightPressed: if (view.selected.isDir) backend.navigate(view.selected.path)
                Keys.onLeftPressed: backend.goUp()

                delegate: FileRow {
                    width: here.width
                    chevron: true
                    current: selected
                    onClicked: (modifiers) => { selecting.click(index, modifiers); here.forceActiveFocus() }
                    onActivated: backend.open(path)
                }

                // Where the first item would be, lined up with the names.
                Text {
                    x: 14 + 20 + 12
                    y: 8
                    visible: here.count === 0 && !backend.folder.loading && backend.folder.error.length === 0
                    text: backend.folder.query ? qsTr("No matches") : qsTr("Empty")
                    color: theme.secondaryForeground
                }
            }
            Separator { }

            // A look inside the selected folder; clicking in it goes there.
            ListView {
                id: inside
                width: view.columnWidth
                height: row.height
                visible: view.selected.isDir === true
                clip: true
                acceptedButtons: Qt.NoButton
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: FolioScrollBar { }
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => {
                        const index = inside.indexAt(point.position.x + inside.contentX, point.position.y + inside.contentY)
                        view.contextMenu(index >= 0 ? inside.model.get(index) : null, view.selected.path)
                    }
                }
                model: FolderModel {
                    path: view.selected.isDir === true ? view.selected.path : ""
                    showHidden: backend.folder.showHidden
                    sortMode: backend.folder.sortMode
                }
                delegate: FileRow {
                    width: inside.width
                    chevron: true
                    onClicked: backend.showItem(path)
                    onActivated: backend.open(path)
                }

                Text {
                    x: 14 + 20 + 12
                    y: 8
                    visible: inside.count === 0 && !inside.model.loading
                    text: qsTr("Empty")
                    color: theme.secondaryForeground
                }
            }
        }
    }
}
