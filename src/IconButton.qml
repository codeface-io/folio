import QtQuick
import QtQuick.Controls

// A square toolbar button holding one glyph. `text` is its accessible name
// and tooltip, never drawn.
AbstractButton {
    id: button

    property string glyph
    property bool active: false

    // The 9px around the glyph stays; the button grows with the mark.
    implicitWidth: Math.max(36, theme.scaled(18) + 18)
    implicitHeight: implicitWidth
    focusPolicy: Qt.NoFocus
    hoverEnabled: true

    Accessible.name: text
    // Its own tooltip, so it can appear without fading in or out.
    ToolTip {
        visible: button.hovered && button.text.length > 0
        delay: 600
        text: button.text
        enter: null
        exit: null
    }

    background: Rectangle {
        radius: Math.min(4, theme.radius)
        color: button.active ? Qt.alpha(theme.accent, 0.16)
             : button.hovered && button.enabled ? Qt.alpha(theme.foreground, 0.08)
             : "transparent"
    }

    contentItem: Item {
        Glyph {
            anchors.centerIn: parent
            name: button.glyph
            color: !button.enabled ? Qt.alpha(theme.foreground, 0.35)
                 : button.active ? theme.brightForeground : theme.foreground
        }
    }
}
