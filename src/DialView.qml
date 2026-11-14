import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// Top-down drawing of the dial: three keys on top, four on the sides, one knob. Every
// control is clickable and shows its bound action; real presses flash and the knob turns.
Item {
    id: root
    property string control: ""
    property string slot: ""
    signal activated(string control, string slot)

    readonly property var keyItems: ({ key1: k1, key2: k2, key3: k3, key4: k4, key5: k5, key6: k6, key7: k7 })
    property int rev: backend.revision      // re-read the labels whenever the bindings change

    function label(control, slot) { rev; return backend.slotLabel(control, slot) }

    Connections {
        target: backend
        function onInputEvent(control, kind, direction, steps) {
            if (control === "knob") {
                if (kind === "click") knob.pulse()
                else knob.turn(direction === "cw" ? 1 : -1, steps, kind === "press_rotate")
                return
            }
            const k = root.keyItems[control]
            if (!k) return
            if (kind === "press") k.press()
            else if (kind === "release") k.release()
            else if (kind === "long_press") k.pulse()
        }
    }

    Item {
        id: stage
        width: 480; height: 400
        anchors.centerIn: parent
        scale: Math.min(root.width / width, root.height / height)

        Image {
            anchors.fill: parent
            source: "assets/dial.svg"
            sourceSize: Qt.size(960, 800)
            smooth: true
        }

        DialKey { id: k1; control: "key1"; number: 1; x: 66;  y: 30;  width: 108; height: 92
                  label: root.label("key1", "press"); longLabel: root.label("key1", "long_press"); selected: root.control === "key1"
                  onClicked: root.activated("key1", "press") }
        DialKey { id: k2; control: "key2"; number: 2; x: 186; y: 30;  width: 108; height: 92
                  label: root.label("key2", "press"); longLabel: root.label("key2", "long_press"); selected: root.control === "key2"
                  onClicked: root.activated("key2", "press") }
        DialKey { id: k3; control: "key3"; number: 3; x: 306; y: 30;  width: 108; height: 92
                  label: root.label("key3", "press"); longLabel: root.label("key3", "long_press"); selected: root.control === "key3"
                  onClicked: root.activated("key3", "press") }
        DialKey { id: k4; control: "key4"; number: 4; x: 30;  y: 146; width: 86;  height: 92
                  label: root.label("key4", "press"); longLabel: root.label("key4", "long_press"); selected: root.control === "key4"
                  onClicked: root.activated("key4", "press") }
        DialKey { id: k5; control: "key5"; number: 5; x: 364; y: 146; width: 86;  height: 92
                  label: root.label("key5", "press"); longLabel: root.label("key5", "long_press"); selected: root.control === "key5"
                  onClicked: root.activated("key5", "press") }
        DialKey { id: k6; control: "key6"; number: 6; x: 30;  y: 256; width: 86;  height: 92
                  label: root.label("key6", "press"); longLabel: root.label("key6", "long_press"); selected: root.control === "key6"
                  onClicked: root.activated("key6", "press") }
        DialKey { id: k7; control: "key7"; number: 7; x: 364; y: 256; width: 86;  height: 92
                  label: root.label("key7", "press"); longLabel: root.label("key7", "long_press"); selected: root.control === "key7"
                  onClicked: root.activated("key7", "press") }

        // The knob: turns with the real one, pulses on a click.
        Item {
            id: knob
            x: 240 - 80; y: 250 - 80 + 8
            width: 160; height: 160
            property real angle: 0
            property real lit: 0
            property bool pressedTurn: false
            readonly property bool selected: root.control === "knob"

            function turn(direction, steps, pressed) {
                angle += direction * steps * 15
                pressedTurn = pressed
                lit = 0.7
                litFade.restart()
            }
            function pulse() { lit = 1; pop.restart(); litFade.restart() }

            NumberAnimation { id: litFade; target: knob; property: "lit"; to: 0; duration: 500; easing.type: Easing.OutCubic }
            SequentialAnimation {
                id: pop
                NumberAnimation { target: cap; property: "scale"; to: 0.93; duration: 70 }
                NumberAnimation { target: cap; property: "scale"; to: 1.0; duration: 160; easing.type: Easing.OutBack }
            }

            Rectangle {   // selection / activity ring
                anchors.centerIn: parent
                width: parent.width + 10; height: width; radius: width / 2
                color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, knob.lit * 0.35)
                border.width: knob.selected ? 3 : 0
                border.color: theme.accent
            }
            Image {
                id: cap
                anchors.fill: parent
                source: "assets/knob.svg"
                sourceSize: Qt.size(320, 320)
                smooth: true
                rotation: knob.angle
                Behavior on rotation { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
            }
            Label {
                anchors.centerIn: parent
                width: 84
                horizontalAlignment: Text.AlignHCenter
                text: { root.rev; const l = backend.slotLabel("knob", "click"); return l.length ? l : qsTr("Click") }
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                font.pixelSize: 12
                font.weight: Font.DemiBold
                color: Ui.text
            }
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Knob")
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.activated("knob", "click")
            }
        }

        // What turning the knob does, clickable like the keys.
        Repeater {
            model: [
                { slot: "rotate_ccw", x: 138, glyph: "↺" },
                { slot: "rotate_cw",  x: 244, glyph: "↻" }
            ]
            delegate: Rectangle {
                required property var modelData
                x: modelData.x; y: 338; width: 98; height: 26
                radius: 6
                color: chipMouse.containsMouse ? "#2b2b33" : "#1c1c22"
                border.width: root.control === "knob" && root.slot === modelData.slot ? 2 : 1
                border.color: root.control === "knob" && root.slot === modelData.slot ? theme.accent : "#33333b"
                Label {
                    anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    font.pixelSize: 11
                    color: Ui.text
                    text: { root.rev; const l = backend.slotLabel("knob", modelData.slot); return modelData.glyph + " " + (l.length ? l : "—") }
                }
                MouseArea {
                    id: chipMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.activated("knob", modelData.slot)
                }
            }
        }
    }
}
