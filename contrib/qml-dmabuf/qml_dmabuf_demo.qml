import QtQuick

Item {
    id: root
    width: 272
    height: 451
    property int counter: 0

    Rectangle {
        anchors.fill: parent
        color: "#ffffff"
    }

    Rectangle {
        width: parent.width
        height: 88
        color: "#002b5b"

        Text {
            anchors.centerIn: parent
            text: "QML / VC4"
            color: "white"
            font.pixelSize: 30
            font.bold: true
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 125
        text: root.counter
        color: "#111111"
        font.pixelSize: 112
        font.bold: true
    }

    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 285
        spacing: 8

        Repeater {
            model: ["#ff0000", "#00cc00", "#0066ff"]

            Rectangle {
                required property color modelData
                width: 72
                height: 72
                color: modelData
                border.color: "#111111"
                border.width: 2
            }
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 390
        text: "DMA-BUF → sharp-drm"
        color: "#222222"
        font.pixelSize: 18
    }
}
