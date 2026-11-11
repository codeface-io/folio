import QtQuick
import QtQuick.Controls

// A menu entry: its label, the key that does the same on the right, and for
// a submenu Folio's own arrow. Highlighted (by mouse or keyboard) it takes a
// tint of the accent; no ripple, no animation.
MenuItem {
    id: entry

    property string keys
    // Hidden entries take no room, and the arrow keys pass over them: Menu
    // skips only entries that are disabled or out of the tab chain, so
    // Return would otherwise run one nobody can see.
    property bool shown: true

    // Hover is off by default here; with it, the pointer highlights an
    // entry and opens a submenu on its own, as menus elsewhere do.
    hoverEnabled: true
    visible: shown
    height: shown ? implicitHeight : 0
    activeFocusOnTab: shown
    implicitHeight: Math.max(32, theme.scaled(32))
    leftPadding: 14
    rightPadding: 14

    background: Rectangle {
        implicitWidth: 220
        x: 4
        width: parent.width - 8
        radius: Math.min(4, theme.radius)
        color: entry.highlighted && entry.enabled ? Qt.alpha(theme.accent, 0.18) : "transparent"
    }

    contentItem: Item {
        implicitWidth: label.implicitWidth + hint.implicitWidth + 40
        implicitHeight: label.implicitHeight
        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            text: entry.text
            color: !entry.enabled ? theme.secondaryForeground : entry.highlighted ? theme.brightForeground : theme.foreground
        }
        Text {
            id: hint
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: !entry.subMenu
            text: entry.keys
            color: theme.secondaryForeground
            font.pixelSize: theme.scaled(11)
        }
    }

    arrow: Glyph {
        x: entry.width - width - 14
        anchors.verticalCenter: parent.verticalCenter
        visible: !!entry.subMenu
        name: "forward"
        size: 12
        color: entry.enabled ? theme.foreground : theme.secondaryForeground
    }
    indicator: Glyph {
        x: entry.width - width - 14
        anchors.verticalCenter: parent.verticalCenter
        visible: entry.checked
        name: "check"
        size: 14
        color: theme.accent
    }
}
