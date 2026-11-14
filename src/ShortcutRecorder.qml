import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Records a keyboard shortcut from the real keyboard and shows it as modifier chips.
// Super is usually grabbed by Hyprland before the window sees it, so the combo can also
// be typed in the field below.
ColumnLayout {
    id: root
    property string combo: ""
    signal recorded(string combo)

    property bool recording: false
    property int heldModifiers: 0
    property string error: ""
    spacing: 8

    function begin() { error = ""; heldModifiers = 0; recording = true; capture.forceActiveFocus() }
    function stop() { recording = false; capture.focus = false }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        Flow {
            id: chips
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: root.recording ? backend.modifierChips(root.heldModifiers) : backend.comboChips(root.combo)
                delegate: Rectangle {
                    required property string modelData
                    width: chipText.implicitWidth + 18; height: 28
                    radius: Math.min(6, theme.radius)
                    color: root.recording ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.25) : Ui.panelHigh
                    border.color: root.recording ? theme.accent : Ui.border
                    Label { id: chipText; anchors.centerIn: parent; text: modelData; color: Ui.text; font.pixelSize: 13; font.weight: Font.DemiBold }
                }
            }
            Label {
                visible: root.recording && backend.modifierChips(root.heldModifiers).length === 0
                text: qsTr("Press the shortcut…")
                color: theme.accent
                font.pixelSize: 13
                height: 28; verticalAlignment: Text.AlignVCenter
            }
            Label {
                visible: !root.recording && root.combo.length === 0
                text: qsTr("No shortcut yet")
                color: Ui.muted
                font.pixelSize: 13
                height: 28; verticalAlignment: Text.AlignVCenter
            }
        }
        Button {
            id: recordButton
            text: root.recording ? qsTr("Cancel") : qsTr("Record")
            flat: !root.recording
            Material.roundedScale: theme.radius
            onClicked: root.recording ? root.stop() : root.begin()
        }
    }

    // Takes the focus while recording and turns key events into a combo.
    Item {
        id: capture
        focus: false
        Keys.onPressed: (event) => {
            if (!root.recording) return
            event.accepted = true
            root.heldModifiers = event.modifiers
            if (event.key === Qt.Key_Escape && event.modifiers === Qt.NoModifier) { root.stop(); return }
            const combo = backend.comboFromKeyEvent(event.key, event.modifiers)
            if (combo.length === 0) return      // only a modifier so far
            root.stop()
            root.combo = combo
            root.recorded(combo)
        }
        Keys.onReleased: (event) => { if (root.recording) root.heldModifiers = event.modifiers }
    }

    TextField {
        id: typed
        Layout.fillWidth: true
        placeholderText: qsTr("or type it, e.g. ctrl+shift+z")
        text: root.combo
        selectByMouse: true
        Material.roundedScale: theme.radius
        onTextEdited: {
            const normal = backend.normalizeCombo(text)
            root.error = text.length && !normal.length ? qsTr("Not a valid shortcut") : ""
            if (normal.length) { root.combo = normal; root.recorded(normal) }
        }
        onEditingFinished: if (!root.error.length) text = root.combo
    }
    Label {
        visible: root.error.length > 0
        text: root.error
        color: Ui.bad
        font.pixelSize: 12
    }
}
