import QtQuick
import QtQuick.Controls

// A text field in Folio's look: a dark box with an accent edge while typing
// and the hint inside it, where it stays (no floating label animation).
TextField {
    id: field
    font.pixelSize: theme.scaled(13)
    color: theme.brightForeground
    selectionColor: theme.accent
    selectedTextColor: theme.accentForeground
    // Material floats its own placeholder up onto the edge as a label, so
    // that one stays hidden and the hint below takes its place.
    placeholderTextColor: "transparent"
    leftPadding: 12
    rightPadding: 12
    topPadding: 9
    bottomPadding: 9
    background: Rectangle {
        implicitHeight: Math.max(38, theme.scaled(38))
        color: theme.darkBackground
        border.width: 1
        border.color: field.activeFocus ? theme.accent : Qt.alpha(theme.foreground, 0.18)
        radius: Math.min(4, theme.radius)
    }

    Text {
        x: field.leftPadding
        width: field.width - field.leftPadding - field.rightPadding
        anchors.verticalCenter: parent.verticalCenter
        visible: field.length === 0 && field.preeditText.length === 0
        text: field.placeholderText
        color: theme.secondaryForeground
        font: field.font
        elide: Text.ElideRight
    }
}
