import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Searchable list of Omarchy's own shortcuts. Picking one links the control to the
// shortcut's command: it is run, not typed, so it works even though Hyprland ignores
// injected keys. Entries that cannot be run are shown dimmed and cannot be chosen.
ColumnLayout {
    id: root
    property string selected: ""
    signal picked(string description)
    spacing: 8

    TextField {
        id: search
        Layout.fillWidth: true
        placeholderText: qsTr("Search Omarchy shortcuts")
        selectByMouse: true
        Material.roundedScale: theme.radius
    }

    Label {
        visible: backend.shortcutsError.length > 0
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        color: Ui.bad
        font.pixelSize: 12
        text: backend.shortcutsError
    }

    ListView {
        id: list
        Layout.fillWidth: true
        Layout.preferredHeight: 220
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        model: {
            const q = search.text.trim().toLowerCase()
            const all = backend.shortcuts
            if (!q.length) return all
            return all.filter(s => s.description.toLowerCase().indexOf(q) >= 0 || s.combo.toLowerCase().indexOf(q) >= 0)
        }
        delegate: ItemDelegate {
            required property var modelData
            width: ListView.view.width
            height: 40
            enabled: modelData.runnable
            highlighted: modelData.description === root.selected
            Material.roundedScale: theme.radius
            Accessible.name: qsTr("%1, %2").arg(modelData.description).arg(modelData.combo)
            contentItem: RowLayout {
                spacing: 10
                Label {
                    Layout.fillWidth: true
                    text: modelData.description
                    elide: Text.ElideRight
                    color: modelData.runnable ? Ui.text : Ui.muted
                    font.pixelSize: 13
                }
                Label {
                    text: modelData.runnable ? modelData.combo : qsTr("can't be run")
                    color: Ui.muted
                    font.pixelSize: 11
                    elide: Text.ElideLeft
                    Layout.maximumWidth: 150
                }
            }
            ToolTip.visible: hovered && !modelData.runnable
            ToolTip.text: modelData.reason || ""
            onClicked: root.picked(modelData.description)
        }
        Label {
            anchors.centerIn: parent
            visible: list.count === 0
            color: Ui.muted
            text: backend.shortcuts.length === 0 ? qsTr("No shortcuts loaded") : qsTr("Nothing matches")
        }
    }
}
