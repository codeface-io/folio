import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtCore

// The window: one bar across the top, under it the sidebar, one folder pane
// or two side by side (Ctrl+Alt+N splits, Ctrl+W closes the right one),
// and the details pane. Each pane's path sits in the bar above its files;
// the sidebar and the details follow the pane used last.
ApplicationWindow {
    id: win
    width: 1280
    height: 800
    minimumWidth: 720
    minimumHeight: 460
    visible: true
    title: activePane && activePane.paneBackend.title.length > 0 ? activePane.paneBackend.title : "Files"
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.foreground: theme.foreground
    Material.background: theme.background
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property bool previewVisible: true
    // Columns on a first launch; after that, whichever view was used last.
    property string viewMode: "columns"
    property real zoom: 1

    // The pane keys, the sidebar and the details go to.
    property var activePane: leftPane
    // The right pane while the window is split.
    property var rightPane: null
    readonly property bool split: rightPane !== null
    // Whether keys are going into a text field. The right pane can go before
    // the window does when it closes, leaving activePane null.
    readonly property bool typing: !!activePane && activePane.typing
    // The rightmost pane's path bar reaches over the details column.
    readonly property int detailsWidth: details.visible ? details.width : 0
    // Whether the window is wide enough for the details column at all.
    readonly property bool detailsFit: width >= (split ? 1400 : 960)

    // The process works in the folder the window used last shows, in the
    // pane used last, for Omarchy's terminal key to open there.
    onActiveChanged: if (active && activePane) activePane.paneBackend.keepWorkingDirectory()
    onActivePaneChanged: if (active && activePane) activePane.paneBackend.keepWorkingDirectory()

    // Remembers the view between launches (~/.config/folio/folio.conf).
    Settings {
        property alias viewMode: win.viewMode
        property alias zoom: win.zoom
    }

    // Ctrl+Alt+N: a second pane on the right, starting where the active
    // one is. Pressed again, it moves the keyboard there.
    function openSplit() {
        if (!rightPane) {
            rightPane = windows.openSplit(win, activePane.paneBackend.path)
            if (!rightPane)
                return
            rightPane.parent = splitHost
            rightPane.anchors.fill = splitHost
            rightPane.closable = true
            rightPane.viewMode = activePane.viewMode
            rightPane.zoom = activePane.zoom
            rightPane.barExtension = Qt.binding(() => win.detailsWidth)
            rightPane.closeRequested.connect(closeSplit)
        }
        rightPane.focusView()
    }
    function closeSplit() {
        if (!rightPane)
            return
        activePane = leftPane
        rightPane = null
        windows.closeSplit(win)
        leftPane.focusView()
    }

    Shortcut { sequence: "?"; enabled: !win.typing || win.helpVisible; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Escape"; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Q"; enabled: !win.typing; onActivated: win.close() }
    Shortcut { sequence: "Ctrl+Alt+N"; enabled: !win.typing; onActivated: win.openSplit() }
    Shortcut { sequence: "Ctrl+W"; enabled: win.split; onActivated: win.closeSplit() }
    Shortcut { sequence: "Ctrl+I"; onActivated: win.previewVisible = !win.previewVisible }
    // Tab moves the keyboard: sidebar, left pane, right pane, and around.
    Shortcut {
        sequences: ["Tab", "Backtab"]
        enabled: !win.typing
        onActivated: {
            if (sidebar.activeFocus)
                leftPane.focusView()
            else if (win.split && win.activePane === leftPane)
                win.rightPane.focusView()
            else
                sidebar.focusSidebar()
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        // The bar across the top; the panes' path bars draw over it.
        readonly property int barHeight: 60
        Rectangle {
            width: parent.width
            height: parent.barHeight
            color: theme.background
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Qt.alpha(theme.foreground, 0.12) }
        }

        RowLayout {
            anchors.fill: parent
            spacing: 0

            Sidebar {
                id: sidebar
                target: win.activePane ? win.activePane.paneBackend : backend
                onFinished: win.activePane.focusView()
                onHelpRequested: win.helpVisible = !win.helpVisible
                Layout.topMargin: parent.parent.barHeight
                Layout.preferredWidth: 190
                Layout.fillHeight: true
            }
            Rectangle { Layout.topMargin: parent.parent.barHeight; Layout.fillHeight: true; implicitWidth: 1; color: Qt.alpha(theme.foreground, 0.12) }

            FolderPane {
                id: leftPane
                barExtension: win.split ? 0 : win.detailsWidth
                barLeft: sidebar.width + 1
                Layout.fillWidth: true
                Layout.fillHeight: true
                focus: true
            }

            // openSplit() puts the right pane of a split here. Between the
            // panes the divider runs up through the top bar, so each path
            // reads as belonging to the files below it.
            Rectangle { visible: win.split; Layout.fillHeight: true; implicitWidth: 1; color: Qt.alpha(theme.foreground, 0.12) }
            Item {
                id: splitHost
                visible: win.split
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            PreviewPane {
                id: details
                visible: win.previewVisible && win.detailsFit
                Layout.topMargin: parent.parent.barHeight
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                folderPane: win.activePane
            }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            onDismissed: win.helpVisible = false
        }
    }
}
