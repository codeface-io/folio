import QtQuick
import QtQuick.Layouts

// The frame Rename, Confirm and the mount prompt share: a scrim over the
// pane and a card in its middle, titled, holding the dialog's own fields
// and buttons. A click on the scrim is the dialog's to answer; one on the
// card stays there.
Rectangle {
    id: dialog

    property string title
    property int cardWidth: 520
    property color edge: theme.accent
    property alias spacing: column.spacing
    default property alias content: column.data

    signal scrimClicked()

    color: Qt.rgba(0, 0, 0, 0.55)

    MouseArea { anchors.fill: parent; onClicked: dialog.scrimClicked() }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, dialog.cardWidth)
        height: column.implicitHeight + 56
        color: theme.background
        border.width: 1
        border.color: dialog.edge
        radius: theme.radius
        MouseArea { anchors.fill: parent }

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: 28
            spacing: 16

            Text {
                text: dialog.title
                color: theme.brightForeground
                font.pixelSize: theme.scaled(16)
                font.weight: Font.DemiBold
            }
        }
    }
}
