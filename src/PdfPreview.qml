import QtQuick
import QtQuick.Pdf

// Every page, scrolling, fitted to a comfortable reading width. Loaded on
// its own so a missing Qt PDF module only costs PDF previews.
Item {
    id: pdf

    function fit() { view.scaleToWidth(Math.min(width - 48, 960), height) }
    function zoomBy(factor) { view.renderScale = Math.max(0.1, Math.min(10, view.renderScale * factor)) }
    function scroll(direction) { page(direction) }
    function page(direction) {
        view.goToPage(Math.max(0, Math.min(doc.pageCount - 1, view.currentPage + direction)))
    }

    PdfDocument {
        id: doc
        source: previewer.url
        onStatusChanged: if (doc.status === PdfDocument.Ready) Qt.callLater(pdf.fit)
    }

    PdfMultiPageView {
        id: view
        anchors.fill: parent
        document: doc
        visible: doc.status === PdfDocument.Ready
    }

    Text {
        anchors.centerIn: parent
        visible: doc.status === PdfDocument.Error
        text: qsTr("Couldn't open this PDF: %1").arg(doc.error)
        color: theme.foreground
    }

    Rectangle {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        visible: doc.pageCount > 1
        width: pageLabel.implicitWidth + 20
        height: pageLabel.implicitHeight + 10
        radius: Math.min(4, theme.radius)
        color: theme.background
        Text {
            id: pageLabel
            anchors.centerIn: parent
            text: qsTr("Page %1 of %2").arg(view.currentPage + 1).arg(doc.pageCount)
            color: theme.secondaryForeground
            font.pixelSize: theme.scaled(12)
        }
    }
}
