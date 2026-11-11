import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "Format.js" as Format

// Video and audio, playing as soon as they show, through Qt Multimedia's
// FFmpeg backend. Click the picture or press K to pause.
Item {
    id: media

    readonly property bool isVideo: previewer.viewer === "video"
    readonly property bool playing: player.playbackState === MediaPlayer.PlayingState

    function toggle() { media.playing ? player.pause() : player.play() }
    function seek(seconds) { player.position = Math.max(0, Math.min(player.duration, player.position + seconds * 1000)) }

    function clock(ms) {
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600)
        const m = Math.floor(total / 60) % 60
        const s = (total % 60).toString().padStart(2, "0")
        return h > 0 ? h + ":" + m.toString().padStart(2, "0") + ":" + s : m + ":" + s
    }

    MediaPlayer {
        id: player
        source: previewer.url
        videoOutput: video
        audioOutput: AudioOutput { }
        // Starts each file once it has loaded; play() before then is
        // ignored.
        autoPlay: true
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            VideoOutput {
                id: video
                anchors.fill: parent
                visible: media.isVideo
                fillMode: VideoOutput.PreserveAspectFit
            }

            // Audio shows its cover art when the file has one (the installed
            // thumbnailer pulls it out), and its title and artist.
            ColumnLayout {
                anchors.centerIn: parent
                visible: !media.isVideo
                spacing: 16
                Image {
                    id: cover
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 320
                    Layout.preferredHeight: 320
                    visible: status === Image.Ready
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    source: !media.isVideo && previewer.item.path ? Format.thumb(previewer.item.path, previewer.item.mime, 512, previewer.item.modified) : ""
                }
                Glyph {
                    Layout.alignment: Qt.AlignHCenter
                    visible: !cover.visible
                    name: "audio"
                    size: 120
                    stroke: 1.2
                    color: theme.accent
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: player.metaData.stringValue(MediaMetaData.Title) || previewer.item.name || ""
                    color: theme.brightForeground
                    font.pixelSize: theme.scaled(16)
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: player.metaData.stringValue(MediaMetaData.ContributingArtist) || player.metaData.stringValue(MediaMetaData.AlbumArtist) || ""
                    visible: text.length > 0
                    color: theme.secondaryForeground
                }
            }

            TapHandler { onTapped: media.toggle() }

            Text {
                anchors.centerIn: parent
                width: parent.width - 96
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: player.error !== MediaPlayer.NoError
                text: qsTr("Couldn't play this file: %1").arg(player.errorString)
                color: theme.foreground
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 52
            color: theme.background

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 24
                spacing: 12
                IconButton {
                    glyph: media.playing ? "pause" : "play"
                    text: media.playing ? qsTr("Pause (K)") : qsTr("Play (K)")
                    onClicked: media.toggle()
                }
                Text { text: media.clock(player.position); color: theme.foreground; font.pixelSize: theme.scaled(12) }
                Slider {
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, player.duration)
                    value: player.position
                    enabled: player.seekable
                    focusPolicy: Qt.NoFocus
                    onMoved: player.position = value
                }
                Text { text: media.clock(player.duration); color: theme.secondaryForeground; font.pixelSize: theme.scaled(12) }
            }
        }
    }
}
