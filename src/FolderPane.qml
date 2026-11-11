import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One folder pane: its path bar, problems banner and the files (grid,
// list or columns), with its own menu and dialogs. A window
// has one, or two side by side (Ctrl+Alt+N); each has its own folder,
// history, selection and preview through its own `backend` and `fileOps`.
// Keys act on the pane that was used last.
FocusScope {
    id: pane

    // The window this pane is in, for the state the panes share.
    readonly property var host: ApplicationWindow.window
    readonly property bool active: !!host && host.activePane === pane
    readonly property var paneBackend: backend
    readonly property var paneFileOps: fileOps
    property bool closable: false
    signal closeRequested()
    // How far this pane's path bar reaches right, over the details column,
    // so the rightmost pane's buttons sit at the window's edge.
    property int barExtension: 0
    property int barLeft: 0

    // Each pane has its own view. The window's own pane is the one
    // remembered between launches; the right pane of a split starts with
    // the view of the pane it was split from.
    property string viewMode: host ? host.viewMode : "columns"
    onViewModeChanged: if (host && !closable) host.viewMode = viewMode
    // How big the grid's tiles are, in steps (Ctrl+= / Ctrl+- / Ctrl+0, or
    // Ctrl+scroll), shared and remembered the same way as the view.
    property real zoom: host ? host.zoom : 1
    onZoomChanged: if (host && !closable) host.zoom = zoom
    readonly property var zoomSteps: [0.75, 1, 1.25, 1.5, 2]
    function zoomBy(step) {
        const at = zoomSteps.indexOf(zoom)
        zoom = zoomSteps[Math.max(0, Math.min(zoomSteps.length - 1, (at < 0 ? 1 : at) + step))]
    }
    function cycleView() {
        viewMode = viewMode === "grid" ? "list" : viewMode === "list" ? "columns" : "grid"
    }

    // The cursor's item, carried across a switch of view and a re-read of
    // the folder. By path: Recents can list two files of the same name.
    property string lastPath: ""
    property string loadedPath: ""
    // Set when a click on empty space lets go of the cursor: a re-read or a
    // switch of view leaves it let go of, until it moves again or another
    // folder arrives.
    property bool cursorLetGo: false
    // While a text field has the keyboard (renaming, a password), single-key
    // and clipboard shortcuts stand aside so typing works.
    readonly property bool typing: rename.visible || confirm.visible || clash.visible || backend.mounter.asking || pathBar.editing || pathBar.searchFocused || (!!host && host.helpVisible)
    readonly property bool keys: active && !typing
    // What file actions act on: everything selected, or the item under
    // the cursor when nothing is.
    function selectedPaths() {
        return backend.folder.selectedCount > 0 ? backend.folder.selectedPaths() : (current.path ? [current.path] : [])
    }
    readonly property var current: {
        // Re-read when the folder changes underneath the selection.
        backend.folder.count
        const index = view.item ? view.item.currentIndex : -1
        return index >= 0 ? backend.folder.get(index) : ({})
    }
    // An open preview stays on the selected item, followed once the
    // folder's loaded has put the cursor back on it: a re-read leaves
    // another item on the cursor's row for a moment. A tailnet machine
    // isn't a file: the preview keeps what it showed last.
    onCurrentChanged: Qt.callLater(follow)
    function follow() { if (backend.previewing && current.path && current.kind !== "peer") backend.followSelection(current.path) }

    // In the trash, Delete means for good (after asking), and there's
    // Restore and Empty Trash. Nothing is renamed or made there: an item
    // without its note (or under another name) can't be restored.
    readonly property bool inTrash: fileOps.inTrash(backend.path)
    // The tailnet view lists machines, not files: there's nothing to copy,
    // move, rename, make, pin or preview there, only machines to open.
    readonly property bool inTailnet: backend.path === "tailnet:"
    readonly property bool fileKeys: keys && !inTailnet
    // A folder to work in: not a view, which lists things from all over,
    // nor the trash.
    readonly property bool inFolder: backend.path.startsWith("/") && !inTrash
    Binding { target: backend.folder; property: "peers"; value: places.peers }
    function deleteSelected() {
        const paths = pane.selectedPaths()
        if (pane.inTrash)
            confirmDelete(paths)
        else
            fileOps.trash(paths)
    }
    function confirmDelete(paths) {
        if (paths.length === 0)
            return
        const name = paths[0].slice(paths[0].lastIndexOf("/") + 1)
        confirm.ask(qsTr("Delete for good?"),
                    paths.length === 1 ? qsTr("%1 will be deleted permanently. This can't be undone.").arg(name)
                                       : qsTr("%n items will be deleted permanently. This can't be undone.", "", paths.length),
                    qsTr("Delete"), () => fileOps.deleteForever(paths))
    }
    function emptyTrash() {
        const count = fileOps.trashCount()
        if (count === 0)
            return
        confirm.ask(qsTr("Empty the trash?"),
                    qsTr("%n items will be deleted permanently. This can't be undone.", "", count),
                    qsTr("Empty Trash"), () => fileOps.emptyTrash())
    }

    // Whichever pane has the keyboard is the one keys, the sidebar and the
    // details pane follow.
    onActiveFocusChanged: if (activeFocus && host) host.activePane = pane
    function focusView() { if (view.item) view.item.focusView() }

    // Pins a folder to the sidebar, or unpins it when it's pinned already.
    function togglePin(target) {
        if (places.isPinned(target)) {
            places.unpin(target)
            return
        }
        const problem = places.pin(target)
        if (problem.length > 0)
            backend.showMessage(problem)
    }

    Shortcut { sequence: "Space"; enabled: pane.fileKeys; onActivated: backend.togglePreview(pane.current.path || "") }
    Shortcut { sequences: [StandardKey.Copy]; enabled: pane.fileKeys; onActivated: fileOps.copy(pane.selectedPaths()) }
    Shortcut { sequences: [StandardKey.Cut]; enabled: pane.fileKeys; onActivated: fileOps.cut(pane.selectedPaths()) }
    Shortcut { sequences: [StandardKey.Paste]; enabled: pane.fileKeys; onActivated: fileOps.paste(backend.path) }
    // "Yank": the selected path (or the folder's) as text, for a terminal.
    Shortcut { sequence: "Ctrl+Shift+C"; enabled: pane.fileKeys; onActivated: { const paths = pane.selectedPaths(); fileOps.yank(paths.length > 0 ? paths : [backend.path]) } }
    Shortcut { sequence: "Delete"; enabled: pane.fileKeys; onActivated: pane.deleteSelected() }
    Shortcut { sequence: "F2"; enabled: pane.fileKeys && !pane.inTrash; onActivated: if (pane.current.path) rename.open(pane.current) }
    Shortcut { sequence: "Ctrl+Z"; enabled: pane.keys; onActivated: fileOps.undo() }
    Shortcut { sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]; enabled: pane.keys; onActivated: fileOps.redo() }
    Shortcut { sequences: [StandardKey.SelectAll]; enabled: pane.keys; onActivated: backend.folder.selectAll() }
    // Escape stops file work under way; otherwise it narrows a selection
    // of several back to the cursor, then ends a search.
    Shortcut { sequence: "Escape"; enabled: pane.keys && fileOps.busy.length > 0; onActivated: fileOps.cancel() }
    Shortcut {
        sequence: "Escape"
        enabled: pane.keys && fileOps.busy.length === 0 && (backend.folder.selectedCount > 1 || backend.folder.query.length > 0)
        onActivated: {
            if (backend.folder.selectedCount > 1)
                backend.folder.selectOnly(view.item ? view.item.currentIndex : -1)
            else
                pathBar.endSearch()
        }
    }
    Shortcut { sequence: "/"; enabled: pane.keys && backend.path !== "recent:"; onActivated: pathBar.search() }
    // The right-click menu from the keyboard, beside the selected item.
    Shortcut {
        sequences: ["Alt+Return", "Alt+Enter", "Menu", "Shift+F10"]
        enabled: pane.keys
        onActivated: {
            const anchor = view.item && view.item.menuAnchor ? view.item.menuAnchor() : null
            fileMenu.show(pane.current.path ? pane.current : null, backend.path, anchor)
        }
    }
    Shortcut { sequence: "Ctrl+Shift+D"; enabled: pane.fileKeys && !pane.inTrash; onActivated: if (pane.current.path) fileOps.duplicate(pane.current.path) }
    Shortcut { sequences: [StandardKey.New]; enabled: pane.keys; onActivated: backend.newWindow() }
    Shortcut { sequence: "Ctrl+Alt+T"; enabled: pane.keys && pane.inFolder; onActivated: fileOps.openTerminal(backend.path) }
    Shortcut { sequence: "Ctrl+Shift+N"; enabled: pane.fileKeys && !pane.inTrash; onActivated: fileOps.newFolder(backend.path) }
    Shortcut { sequences: ["Ctrl+L", "Alt+D"]; enabled: pane.keys; onActivated: pathBar.editPath() }
    // Pins the selected folder, or the folder being shown when a file is
    // selected; pressed again, unpins it.
    Shortcut { sequence: "Ctrl+D"; enabled: pane.active && !pane.inTailnet; onActivated: pane.togglePin(pane.current.isDir ? pane.current.path : backend.path) }
    Shortcut { sequence: "Ctrl+1"; enabled: pane.active; onActivated: pane.viewMode = "grid" }
    Shortcut { sequence: "Ctrl+2"; enabled: pane.active; onActivated: pane.viewMode = "list" }
    Shortcut { sequence: "Ctrl+3"; enabled: pane.active; onActivated: pane.viewMode = "columns" }
    Shortcut { sequences: [StandardKey.ZoomIn, "Ctrl+="]; enabled: pane.active; onActivated: pane.zoomBy(1) }
    Shortcut { sequences: [StandardKey.ZoomOut]; enabled: pane.active; onActivated: pane.zoomBy(-1) }
    Shortcut { sequence: "Ctrl+0"; enabled: pane.active; onActivated: pane.zoom = 1 }
    Shortcut { sequences: ["Return", "Enter"]; enabled: pane.keys && pane.activeFocus; onActivated: if (pane.current.path) backend.open(pane.current.path) }
    Shortcut { sequences: ["Backspace", "Alt+Up"]; enabled: pane.keys; onActivated: backend.goUp() }
    Shortcut { sequences: ["Alt+Left", "Back"]; enabled: pane.active; onActivated: backend.back() }
    Shortcut { sequences: ["Alt+Right", "Forward"]; enabled: pane.active; onActivated: backend.forward() }
    Shortcut { sequence: "Alt+Home"; enabled: pane.active; onActivated: backend.navigate(places.home) }
    Shortcut { sequence: "Ctrl+H"; enabled: pane.active; onActivated: backend.folder.showHidden = !backend.folder.showHidden }
    // S steps through the sort orders, Shift+S back.
    Shortcut { sequence: "S"; enabled: pane.keys; onActivated: pathBar.stepSort(1) }
    Shortcut { sequence: "Shift+S"; enabled: pane.keys; onActivated: pathBar.stepSort(-1) }
    Shortcut { sequences: ["F5", "Refresh"]; enabled: pane.active; onActivated: { backend.folder.reload(); places.refresh() } }

    // A drive being ejected can't be in use: a pane showing a folder on it
    // goes home, and its preview closes.
    Connections {
        target: places
        function onReleasing(root) {
            if (backend.path !== root && !backend.path.startsWith(root + "/"))
                return
            if (backend.previewing)
                backend.togglePreview("")
            backend.navigate(places.home)
        }
    }

    Connections {
        target: backend
        function onSelectRequested(name) { if (view.item) view.item.select(backend.folder.indexOf(name), false) }
        function onActivateRequested() { if (pane.host) { pane.host.raise(); pane.host.requestActivate() } }
        function onPeerOpened(host) { places.remember(host) }
    }

    Connections {
        target: view.item
        function onContextMenu(item, folder) { fileMenu.show(item, folder) }
        function onCursorLetGo() { pane.cursorLetGo = true }
        // The cursor's item is remembered as the cursor moves, and once the
        // folder's loaded has placed it. Not when a re-read leaves the cursor
        // on its row while a new file pushes its item down: loaded then puts
        // it back on the item.
        function onCurrentIndexChanged() {
            const path = backend.folder.get(view.item.currentIndex).path
            if (path) {
                pane.lastPath = path
                pane.cursorLetGo = false
            }
        }
    }

    // Finished file work selects what it made (the pasted file, the new
    // folder, the new name): right away when the folder already lists it
    // (a long move's folder arrived long before the move was done),
    // otherwise once the folder has re-read.
    Connections {
        target: fileOps
        function onDone(folder, name) {
            if (folder !== backend.path)
                return
            const row = backend.folder.indexOf(name)
            if (row >= 0 && view.item)
                view.item.select(row, false)
            else
                backend.expectSelection(name)
        }
        function onFailed(message) { backend.showMessage(message) }
        function onNotice(notice) { backend.showNotice(notice) }
    }

    Connections {
        target: backend.folder
        // A folder arriving selects where we came from (or the first item);
        // the same folder re-read (a remembered listing refreshed, or files
        // changing on disk) keeps what was selected, and the cursor on its
        // item. When that's gone (trashed, or moved away), the cursor stays
        // on its row, so Delete pressed again takes the next item, not the
        // first.
        function onLoaded() {
            if (!view.item)
                return
            const pending = backend.takeSelection()
            const same = backend.folder.path === pane.loadedPath
            pane.loadedPath = backend.folder.path
            const row = backend.folder.indexOfPath(pane.lastPath)
            if (pending || !same)
                pane.cursorLetGo = false
            if (pending)
                view.item.select(backend.folder.indexOf(pending), false)
            else if (!same)
                view.item.select(-1, false)
            else if (!pane.cursorLetGo)
                view.item.select(row >= 0 ? row : Math.min(view.item.currentIndex, backend.folder.count - 1), true)
            // Remembered here too: put on the row it was already on, the
            // cursor doesn't signal a move, and an item from the folder
            // before would stay remembered.
            pane.lastPath = backend.folder.get(view.item.currentIndex).path || ""
        }
    }

    FileMenu {
        id: fileMenu
        onRenameRequested: (target) => rename.open(target)
        onDeleteRequested: (paths) => pane.confirmDelete(paths)
        onEmptyTrashRequested: pane.emptyTrash()
        onPinRequested: (path) => pane.togglePin(path)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Room for the path bar, which is drawn on top so it can reach over
        // the details column.
        Item { Layout.fillWidth: true; implicitHeight: pathBar.implicitHeight }
        // Problems and slow work are said in the window, not swallowed.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 40
            visible: bannerText.text.length > 0
            color: backend.message.length > 0 || backend.folder.error.length > 0 ? Qt.alpha(theme.red, 0.14) : Qt.alpha(theme.accent, 0.10)
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 24
                anchors.rightMargin: 12
                Text {
                    id: bannerText
                    Layout.fillWidth: true
                    text: backend.message || backend.folder.error || backend.busy || fileOps.busy || backend.notice
                    color: theme.brightForeground
                    elide: Text.ElideRight
                }
                // File work can be stopped where it's shown. A click leaves
                // the keyboard with the files, since the button goes once
                // the work stops.
                PlainButton {
                    visible: fileOps.busy.length > 0 && bannerText.text === fileOps.busy
                    focusPolicy: Qt.NoFocus
                    implicitHeight: 28
                    text: qsTr("Cancel")
                    onClicked: fileOps.cancel()
                }
                IconButton {
                    visible: backend.message.length > 0
                    glyph: "close"
                    text: qsTr("Dismiss")
                    onClicked: backend.dismissMessage()
                }
            }
        }

        // The trash offers one thing: emptying it.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 48
            visible: pane.inTrash && backend.folder.count > 0
            color: Qt.alpha(theme.foreground, 0.04)
            PlainButton {
                anchors.centerIn: parent
                text: qsTr("Empty Trash")
                onClicked: pane.emptyTrash()
            }
        }

        Item { implicitHeight: 16 }

        // Grid, list or columns: each takes the folder's model and keeps the
        // pane's one selection.
        Loader {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: pane.viewMode === "grid" ? 24 : pane.viewMode === "list" ? 20 : 0
            Layout.rightMargin: pane.viewMode === "columns" ? 0 : 24
            sourceComponent: pane.viewMode === "list" ? listView
                           : pane.viewMode === "columns" ? columnsView
                           : gridView
            // A new view picks up the selection the old one had.
            onLoaded: {
                if (pane.cursorLetGo)
                    item.letGo()
                else
                    item.select(backend.folder.indexOfPath(pane.lastPath), true)
                item.focusView()
            }

            // Files dropped on empty space land in this folder; folders
            // inside take drops on themselves first.
            DropArea {
                id: viewDrop
                anchors.fill: parent
                z: -1
                keys: ["text/uri-list"]
                onEntered: (drag) => { drag.accepted = fileOps.canDrop(drag.urls, backend.path) }
                onDropped: (drop) => { fileOps.dropInto(drop.urls, backend.path); drop.accept(Qt.MoveAction) }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 2
                    visible: viewDrop.containsDrag
                    color: Qt.alpha(theme.accent, 0.05)
                    border.width: 2
                    border.color: theme.accent
                    radius: Math.min(6, theme.radius)
                }
            }
        }
        Component { id: gridView; FolderGrid { zoom: pane.zoom; onZoomRequested: (step) => pane.zoomBy(step) } }
        Component { id: listView; FolderList { } }
        Component { id: columnsView; FolderColumns { } }
    }

    // Drawn over the window's top bar. It reaches left over the sidebar for
    // the first pane (so the path starts at the window's edge) and right over
    // the details column for the last one (so the buttons end at the edge).
    PathBar {
        id: pathBar
        x: -pane.barLeft
        width: pane.barLeft + pane.width + pane.barExtension
        onEditDone: pane.focusView()
        previewVisible: pane.host ? pane.host.previewVisible : true
        previewFits: pane.host ? pane.host.detailsFit : true
        viewMode: pane.viewMode
        closable: pane.closable
        onTogglePreview: if (pane.host) pane.host.previewVisible = !pane.host.previewVisible
        onCycleView: pane.cycleView()
        onCloseRequested: pane.closeRequested()
    }

    // With two panes, a line along the top marks the one keys go to.
    Rectangle {
        visible: !!pane.host && pane.host.split
        y: pathBar.height - 1
        width: parent.width
        height: 2
        color: pane.active ? theme.accent : "transparent"
    }

    // The mouse's back and forward thumb buttons move through history, as in
    // a browser. Only those buttons are taken; every other click passes
    // through to what's underneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.BackButton | Qt.ForwardButton
        onPressed: mouse => mouse.button === Qt.BackButton ? backend.back() : backend.forward()
    }

    // A dialog takes the keyboard while it's open; closing only hides it,
    // so the keyboard is handed back to the files.
    MountPrompt {
        anchors.fill: parent
        onVisibleChanged: if (!visible) pane.focusView()
    }

    RenameDialog {
        id: rename
        anchors.fill: parent
        onVisibleChanged: if (!visible) pane.focusView()
    }

    ConfirmDialog {
        id: confirm
        anchors.fill: parent
        onVisibleChanged: if (!visible) pane.focusView()
    }

    ClashDialog {
        id: clash
        anchors.fill: parent
        onVisibleChanged: if (!visible) pane.focusView()
    }
}
