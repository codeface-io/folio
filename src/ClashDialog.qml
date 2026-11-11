import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Asks what to do when a name is already taken where files are going,
// before anything is copied or moved: replace what's there (it goes to the
// trash, where it can be restored from), merge a folder into the folder, skip
// it, or keep both, the new one under a "(copy)" name. Keep Both is the
// default; Escape or a click outside cancels the whole operation.
FolioDialog {
    id: dialog

    readonly property var clash: fileOps.clash
    readonly property bool merge: clash.merge === true
    readonly property int others: clash.others || 0

    title: qsTr("“%1” already exists in %2.").arg(clash.name || "").arg(clash.folder || "")
    visible: !!clash.name
    onScrimClicked: fileOps.dropClash()
    // Each clash starts on Keep Both; "for the others" is asked afresh for
    // each operation.
    onClashChanged: if (visible) keepBoth.forceActiveFocus()
    onVisibleChanged: {
        forAll.checked = false
        if (visible)
            keepBoth.forceActiveFocus()
    }

    function answer(choice) {
        fileOps.answerClash(choice, forAll.checked && others > 0)
    }

    Keys.onEscapePressed: fileOps.dropClash()

    Text {
        Layout.fillWidth: true
        text: dialog.merge ? qsTr("Merging adds what's in this folder to the one there. What has the same name inside is replaced, and the replaced items go to the trash.")
                           : qsTr("Replacing moves the one there now to the trash.")
        color: theme.foreground
        wrapMode: Text.Wrap
    }

    AbstractButton {
        id: forAll
        visible: dialog.others > 0
        checkable: true
        text: qsTr("Do this for the other %n clashes", "", dialog.others)
        Accessible.name: text
        Keys.onReturnPressed: toggle()
        Keys.onEnterPressed: toggle()
        contentItem: RowLayout {
            spacing: 10
            Rectangle {
                implicitWidth: 18
                implicitHeight: 18
                radius: Math.min(3, theme.radius)
                color: forAll.checked ? theme.accent : "transparent"
                border.width: forAll.checked ? 0 : 1
                border.color: forAll.visualFocus ? theme.accent : Qt.alpha(theme.foreground, 0.4)
                Glyph {
                    anchors.centerIn: parent
                    visible: forAll.checked
                    name: "check"
                    size: 14
                    stroke: 2
                    color: theme.accentForeground
                }
            }
            Text {
                text: forAll.text
                color: forAll.visualFocus ? theme.brightForeground : theme.foreground
            }
        }
    }

    RowLayout {
        Layout.alignment: Qt.AlignRight
        spacing: 10
        PlainButton {
            id: replace
            text: dialog.merge ? qsTr("Merge") : qsTr("Replace")
            onClicked: dialog.answer("replace")
            Keys.onReturnPressed: clicked()
            Keys.onEnterPressed: clicked()
            KeyNavigation.right: skip
        }
        PlainButton {
            id: skip
            text: qsTr("Skip")
            onClicked: dialog.answer("skip")
            Keys.onReturnPressed: clicked()
            Keys.onEnterPressed: clicked()
            KeyNavigation.left: replace
            KeyNavigation.right: keepBoth
        }
        PlainButton {
            id: keepBoth
            text: qsTr("Keep Both")
            primary: true
            onClicked: dialog.answer("both")
            Keys.onReturnPressed: clicked()
            Keys.onEnterPressed: clicked()
            KeyNavigation.left: skip
        }
    }
}
