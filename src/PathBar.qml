import QtQuick
import QtQuick.Layouts

// The breadcrumb path from the left edge; view buttons on the right. Back and
// forward are keys (Alt+← / Alt+→) and the mouse's thumb buttons.
Rectangle {
    id: bar

    property bool previewVisible: true
    property string viewMode: "grid"
    // Ctrl+L: the path as text to edit. Enter goes there, Escape cancels,
    // Tab completes a folder name.
    property bool editing: false
    // The right pane of a split has a close button (Ctrl+W).
    property bool closable: false
    signal closeRequested()
    signal togglePreview()
    signal cycleView()
    signal editDone()

    function editPath() {
        pathField.text = backend.typedPath()
        editing = true
        pathField.forceActiveFocus()
        pathField.selectAll()
    }
    function stopEditing() {
        editing = false
        editDone()
    }

    // /: search this folder and every folder below it, as you type. The
    // results replace the folder until Escape, or until going elsewhere.
    readonly property bool searching: backend.folder.query.length > 0 || searchFocused
    readonly property bool searchFocused: searchField.activeFocus
    function search() {
        searchField.forceActiveFocus()
        searchField.selectAll()
    }
    // S and Shift+S, through the sort menu's orders.
    function stepSort(by) { sortMenu.step(by) }
    function endSearch() {
        searchDelay.stop()
        searchField.text = ""
        backend.folder.query = ""
        editDone()
    }
    Timer {
        id: searchDelay
        interval: 150
        onTriggered: backend.folder.query = searchField.text.trim()
    }
    // Going to another folder ends the search there.
    Connections {
        target: backend.folder
        function onQueryChanged() { if (backend.folder.query === "" && !searchField.activeFocus) searchField.text = "" }
    }

    // Transparent: the window draws one bar across the whole top, so the
    // path bars of one or two panes read as part of it.
    color: "transparent"
    implicitHeight: 60

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 26
        anchors.rightMargin: 20
        spacing: 4


        PlainField {
            id: pathField
            Layout.fillWidth: true
            visible: bar.editing
            font.pixelSize: theme.scaled(14)
            onAccepted: {
                if (backend.goTyped(text))
                    bar.stopEditing()
            }
            Keys.onEscapePressed: bar.stopEditing()
            Keys.onTabPressed: {
                text = backend.completePath(text)
                cursorPosition = text.length
            }
            onActiveFocusChanged: if (!activeFocus && bar.editing) bar.stopEditing()
        }

        PlainField {
            id: searchField
            Layout.fillWidth: true
            visible: bar.searching && !bar.editing
            font.pixelSize: theme.scaled(14)
            rightPadding: scope.implicitWidth + 24
            // Where it searches, at the end of the field. Unlike a
            // placeholder, it stays while typing.
            Text {
                id: scope
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Search %1 and below").arg(backend.crumbs.length > 0 ? backend.crumbs[backend.crumbs.length - 1].name : "")
                color: theme.secondaryForeground
                font.pixelSize: theme.scaled(12)
            }
            onTextEdited: searchDelay.restart()
            // Enter or ↓ goes to the results, leaving the search up.
            function toResults() {
                searchDelay.stop()
                backend.folder.query = text.trim()
                if (backend.folder.query === "")
                    bar.endSearch()
                else
                    bar.editDone()
            }
            onAccepted: toResults()
            Keys.onDownPressed: toResults()
            Keys.onEscapePressed: bar.endSearch()
        }

        Flickable {
            visible: !bar.editing && !bar.searching
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: crumbs.implicitWidth
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            // Long paths keep their end in view; that's where you are.
            onContentWidthChanged: contentX = Math.max(0, contentWidth - width)
            onWidthChanged: contentX = Math.max(0, contentWidth - width)

            Row {
                id: crumbs
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Repeater {
                    model: backend.crumbs
                    Row {
                        id: crumb
                        required property var modelData
                        required property int index
                        readonly property bool last: index === backend.crumbs.length - 1
                        spacing: 10
                        Text {
                            visible: crumb.index > 0
                            text: "/"
                            color: Qt.alpha(theme.foreground, 0.35)
                            font.pixelSize: theme.scaled(14)
                        }
                        Text {
                            text: crumb.modelData.name
                            color: crumb.last ? theme.brightForeground : crumbMouse.containsMouse ? theme.foreground : theme.secondaryForeground
                            font.pixelSize: theme.scaled(14)
                            font.weight: crumb.last ? Font.DemiBold : Font.Normal
                            MouseArea {
                                id: crumbMouse
                                anchors.fill: parent
                                anchors.margins: -4
                                hoverEnabled: true
                                cursorShape: crumb.last ? Qt.ArrowCursor : Qt.PointingHandCursor
                                onClicked: if (!crumb.last) backend.navigate(crumb.modelData.path)
                            }
                        }
                    }
                }
            }
        }

        Item { implicitWidth: 16 }
        // Shows the current view; clicking moves to the next one.
        IconButton {
            glyph: bar.viewMode
            text: bar.viewMode === "grid" ? qsTr("Grid (Ctrl+1); click for list")
                : bar.viewMode === "list" ? qsTr("List (Ctrl+2); click for columns")
                : qsTr("Columns (Ctrl+3); click for grid")
            onClicked: bar.cycleView()
        }
        IconButton {
            id: sortButton
            // Shows the order in use, and says it on hover.
            glyph: sortMenu.current.glyph
            text: sortMenu.current.tip
            active: sortMenu.visible
            onClicked: sortMenu.popup(sortButton, 0, sortButton.height + 4)
        }
        SortMenu { id: sortMenu }
        IconButton {
            glyph: "hidden"
            text: qsTr("Show hidden files (Ctrl+H)")
            active: backend.folder.showHidden
            onClicked: backend.folder.showHidden = !backend.folder.showHidden
        }
        IconButton {
            glyph: "preview"
            text: qsTr("Details (Ctrl+I)")
            active: bar.previewVisible
            onClicked: bar.togglePreview()
        }
        IconButton {
            visible: bar.closable
            glyph: "close"
            text: qsTr("Close this pane (Ctrl+W)")
            onClicked: bar.closeRequested()
        }
    }

    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Qt.alpha(theme.foreground, 0.12) }
}
