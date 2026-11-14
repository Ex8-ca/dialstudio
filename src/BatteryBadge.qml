import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

RowLayout {
    id: root
    property int percent: -1
    property bool charging: false
    spacing: 8
    Accessible.name: percent < 0 ? qsTr("Battery unknown") : qsTr("Battery %1%").arg(percent)

    Item {
        implicitWidth: 28; implicitHeight: 14
        Rectangle {
            id: shell
            width: 25; height: 14
            color: "transparent"
            border.color: Ui.muted; border.width: 1.5
            radius: 3
            Rectangle {
                x: 3; y: 3; height: parent.height - 6
                width: Math.max(0, (parent.width - 6) * Math.max(0, root.percent) / 100)
                radius: 1
                color: root.percent < 0 ? "transparent" : root.percent <= 15 ? Ui.bad : root.percent <= 30 ? Ui.warn : Ui.ok
            }
        }
        Rectangle { x: 25; y: 4; width: 3; height: 6; radius: 1; color: Ui.muted }
    }
    Label {
        text: root.percent < 0 ? "—" : qsTr("%1%").arg(root.percent)
        color: Ui.text
        font.pixelSize: 13
    }
    Label {
        visible: root.charging
        text: qsTr("Charging")
        color: Ui.muted
        font.pixelSize: 12
    }
}
