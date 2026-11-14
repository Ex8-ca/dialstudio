import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Shows the daemon/dial link: online, connecting, offline, Bluetooth off.
Rectangle {
    id: pill
    property string state_: "offline"

    readonly property color tone: state_ === "online" ? Ui.ok
                                : state_ === "connecting" ? Ui.warn
                                : state_ === "offline" ? Ui.muted : Ui.bad
    readonly property string text_: state_ === "online" ? qsTr("Online")
                                  : state_ === "connecting" ? qsTr("Connecting…")
                                  : state_ === "bluetooth-off" ? qsTr("Bluetooth off")
                                  : state_ === "not-paired" ? qsTr("Not paired")
                                  : state_ === "no-bluez" ? qsTr("Bluetooth unavailable")
                                  : qsTr("Offline")

    implicitHeight: 28
    implicitWidth: row.implicitWidth + 24
    radius: height / 2      // a pill is a true capsule, not a themed corner
    color: Qt.rgba(tone.r, tone.g, tone.b, 0.14)
    border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.55)
    border.width: 1

    Accessible.role: Accessible.StaticText
    Accessible.name: qsTr("Connection: %1").arg(text_)

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 8
        Rectangle {
            width: 8; height: 8; radius: 4
            color: pill.tone
            SequentialAnimation on opacity {
                running: pill.state_ === "connecting"
                loops: Animation.Infinite
                NumberAnimation { to: 0.3; duration: 600 }
                NumberAnimation { to: 1.0; duration: 600 }
            }
        }
        Label { text: pill.text_; color: Ui.text; font.pixelSize: 13 }
    }
}
