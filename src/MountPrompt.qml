import QtQuick
import QtQuick.Layouts

// GVfs's password request while connecting to another machine whose keys
// aren't set up, or its question about a server it can't vouch for (a key
// seen for the first time, a certificate that can't be verified), put to
// the user in the window. The wording comes from GVfs, already translated.
FolioDialog {
    id: prompt

    readonly property var mounter: backend.mounter
    readonly property bool question: mounter.choices.length > 0

    title: question ? qsTr("Check the server") : qsTr("Sign in")
    cardWidth: 600
    spacing: 18
    visible: mounter.asking

    // Each request starts fresh, whichever way the last one closed
    // (Connect, Cancel, Escape or GVfs giving up), so a password typed for
    // one machine is never offered to the next, nor kept once it's closed.
    onVisibleChanged: {
        password.text = ""
        if (visible && !question) {
            user.text = mounter.user
            password.forceActiveFocus()
        }
    }

    function submit() {
        prompt.mounter.providePassword(user.text, password.text)
    }

    // A click outside the card does nothing: GVfs waits for an answer,
    // which only the buttons or Escape give.
    Shortcut { sequence: "Escape"; enabled: prompt.visible; context: Qt.WindowShortcut; onActivated: prompt.mounter.cancel() }

    Text {
        Layout.fillWidth: true
        text: prompt.mounter.prompt
        color: theme.foreground
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }

    PlainField {
        id: user
        visible: !prompt.question
        Layout.fillWidth: true
        placeholderText: qsTr("User")
    }
    PlainField {
        id: password
        visible: !prompt.question
        Layout.fillWidth: true
        placeholderText: qsTr("Password")
        echoMode: TextInput.Password
        onAccepted: prompt.submit()
    }

    RowLayout {
        Layout.alignment: Qt.AlignRight
        spacing: 10

        // A question's own answers, none of them the default: going ahead
        // with a server that couldn't be checked takes a deliberate click.
        Repeater {
            model: prompt.mounter.choices
            PlainButton {
                required property string modelData
                required property int index
                text: modelData
                onClicked: prompt.mounter.choose(index)
            }
        }
        PlainButton {
            visible: !prompt.question
            text: qsTr("Cancel")
            onClicked: prompt.mounter.cancel()
        }
        PlainButton {
            visible: !prompt.question
            text: qsTr("Connect")
            primary: true
            onClicked: prompt.submit()
        }
    }
}
