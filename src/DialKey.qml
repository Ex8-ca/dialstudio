import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// One key of the dial drawing: clickable, shows what it does, lights up on a real press.
Item {
    id: key
    objectName: control
    property string control
    property int number: 0
    property string label: ""
    property string longLabel: ""
    property bool selected: false
    property bool down: false          // physically held (from the daemon's event stream)
    property real lit: 0               // flash level 0..1, fades out after a release
    signal clicked()

    readonly property color accent: theme.accent
    readonly property real glow: Math.max(lit, down ? 1 : 0)
    // "Ctrl+Shift+Z" reads better as a small modifier line over a big Z than as three
    // broken lines on the narrow side keys.
    readonly property var parts: label.length ? label.split("+") : []
    readonly property string modifiers: parts.length > 1 ? parts.slice(0, -1).join("+") : ""
    readonly property string baseKey: parts.length ? parts[parts.length - 1] : ""

    function press() { fade.stop(); lit = 1; down = true }
    function release() { down = false; fade.restart() }
    function pulse() { down = false; lit = 1; fade.restart() }

    NumberAnimation { id: fade; target: key; property: "lit"; from: 1; to: 0; duration: 420; easing.type: Easing.OutCubic }

    Accessible.role: Accessible.Button
    Accessible.name: qsTr("%1: %2").arg(qsTr("Key %1").arg(key.number)).arg(key.label.length ? key.label : qsTr("not bound"))
    Accessible.onPressAction: key.clicked()

    Rectangle {
        id: face
        anchors.fill: parent
        anchors.margins: 7
        radius: 10      // matches the rounded keys of the drawing, not the window theme
        color: Qt.tint(mouse.containsMouse ? "#2b2b33" : "#25252c", Qt.rgba(key.accent.r, key.accent.g, key.accent.b, key.glow * 0.92))
        border.width: key.selected ? 3 : 1
        border.color: key.selected ? key.accent : "#3c3c45"
        scale: key.down ? 0.97 : 1
        Behavior on scale { NumberAnimation { duration: 70 } }

        Label {
            x: 9; y: 6
            text: key.number
            font.pixelSize: 11
            color: key.glow > 0.5 ? theme.accentForeground : Ui.muted
            opacity: 0.85
        }
        Label {
            visible: key.modifiers.length > 0
            anchors { top: parent.top; topMargin: 18; horizontalCenter: parent.horizontalCenter }
            width: parent.width - 18
            horizontalAlignment: Text.AlignHCenter
            text: key.modifiers
            elide: Text.ElideRight
            maximumLineCount: 1
            font.pixelSize: 9
            color: key.glow > 0.5 ? theme.accentForeground : Ui.muted
            opacity: 0.9
        }
        Label {
            anchors.centerIn: parent
            width: parent.width - 12
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            // The base key (or phrase, e.g. "Volume up") shrinks until it fits; modifiers sit
            // in the line above, so nothing is ever broken mid-word.
            text: key.parts.length > 1 ? key.baseKey : (key.label.length ? key.label : qsTr("Not bound"))
            wrapMode: Text.NoWrap
            elide: Text.ElideRight
            font.pixelSize: 15
            minimumPixelSize: 9
            fontSizeMode: Text.Fit
            font.weight: Font.DemiBold
            color: key.glow > 0.5 ? theme.accentForeground : (key.label.length ? Ui.text : Ui.muted)
        }
        Label {
            visible: key.longLabel.length > 0
            anchors { bottom: parent.bottom; bottomMargin: 5; horizontalCenter: parent.horizontalCenter }
            width: parent.width - 12
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("hold: %1").arg(key.longLabel)
            elide: Text.ElideRight
            font.pixelSize: 10
            color: key.glow > 0.5 ? theme.accentForeground : Ui.muted
        }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: key.clicked()
    }
}
