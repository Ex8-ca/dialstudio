import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The left panel: profiles, their pages and match rules, and the presets.
// Every edit re-saves the whole profile list in one validated daemon save, so a refusal
// leaves the saved list intact (onProfilesChanged / onEditFailed reload it).
Rectangle {
    id: root
    objectName: "profilesPane"
    color: Ui.panel
    radius: theme.radius
    border.color: Ui.border

    property var list: []          // the saved list when clean, or the draft when editing
    property int selected: -1      // index into list of the profile being edited
    property bool dirty: false
    readonly property var current: selected >= 0 && selected < list.length ? list[selected] : null
    readonly property bool editingFallback: !!current && current.match === null

    function reload() {
        list = JSON.parse(JSON.stringify(backend.profiles))
        dirty = false
        if (selected >= list.length) selected = -1
    }
    Connections {
        target: backend
        // Ignore other-client reloads while a draft is open, unless it was a refusal.
        function onProfilesChanged() { if (!root.dirty) root.reload() }
    }
    Component.onCompleted: reload()

    function push(next) {                     // save; a refusal resets the draft via profilesChanged
        dirty = true
        backend.saveProfiles(next)
    }
    function patchSelected(key, value) {
        if (!current) return
        const copy = JSON.parse(JSON.stringify(list))
        copy[selected][key] = value
        push(copy)
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: col.implicitHeight
        clip: true
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: col
            width: parent.width
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Label { text: qsTr("Profiles"); font.pixelSize: 15; font.weight: Font.DemiBold; color: Ui.text; Layout.fillWidth: true }
                ToolButton {
                    objectName: "addProfile"
                    text: "+"
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Add a profile")
                    onClicked: {
                        const copy = JSON.parse(JSON.stringify(root.list))
                        copy.push({ name: "", match: { "class": "^example" }, pages: [ { name: qsTr("Page 1"), bindings: {} } ] })
                        root.selected = copy.length - 1
                        root.push(copy)
                    }
                }
            }

            Repeater {
                model: root.list
                delegate: Rectangle {
                    id: row
                    required property int index
                    required property var modelData
                    readonly property bool fallback: modelData.match === null
                    readonly property bool active: backend.activeProfile === modelData.name
                    Layout.fillWidth: true
                    implicitHeight: rowCol.implicitHeight + 16
                    radius: Math.min(6, theme.radius)
                    color: root.selected === index ? Ui.panelHigh : (hoverArea.containsMouse ? Ui.panelHigh : Ui.panel)
                    border.color: root.selected === index ? theme.accent : Ui.border
                    MouseArea {
                        id: hoverArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.selected = index
                    }
                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top; anchors.bottom: parent.bottom
                        width: 3
                        radius: 2
                        color: row.active ? theme.accent : "transparent"
                    }
                    ColumnLayout {
                        id: rowCol
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 10
                        spacing: 1
                        Label {
                            text: modelData.name || qsTr("Unnamed")
                            color: root.selected === row.index ? theme.accent : Ui.text
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            text: row.fallback ? qsTr("Every other window")
                                               : qsTr("When the window class matches")
                            color: Ui.muted
                            font.pixelSize: 10
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                }
            }
            Label {
                visible: root.list.length === 0
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Ui.muted
                font.pixelSize: 12
                text: qsTr("No profiles yet. Add one, or apply a preset.")
            }

            // ---- editor for the selected profile ---------------------------------------
            ColumnLayout {
                visible: !!root.current
                Layout.fillWidth: true
                spacing: 8

                Label { text: qsTr("Name"); color: Ui.muted; font.pixelSize: 11 }
                TextField {
                    objectName: "profileName"
                    Layout.fillWidth: true
                    text: root.current ? root.current.name : ""
                    selectByMouse: true
                    Material.roundedScale: theme.radius
                    onEditingFinished: {
                        const trimmed = text.trim()
                        if (!trimmed.length || !root.current || trimmed === root.current.name) return
                        root.patchSelected("name", trimmed)
                    }
                }

                Label {
                    text: qsTr("Window class regex")
                    color: Ui.muted
                    font.pixelSize: 11
                }
                TextField {
                    objectName: "profileMatch"
                    visible: !root.editingFallback
                    Layout.fillWidth: true
                    placeholderText: qsTr("^org\\.kde\\.kdenlive$ — empty leaves it out")
                    text: root.current && root.current.match ? root.current.match["class"] || "" : ""
                    selectByMouse: true
                    font.family: "monospace"
                    font.pixelSize: 11
                    Material.roundedScale: theme.radius
                    onEditingFinished: {
                        if (!root.current) return
                        if (!text.trim().length) { root.patchSelected("match", null); return }
                        root.patchSelected("match", { "class": text.trim() })
                    }
                }
                ComboBox {
                    objectName: "clientsPicker"
                    visible: !root.editingFallback
                    Layout.fillWidth: true
                    model: backend.clients
                    textRole: "class"
                    font.pixelSize: 11
                    displayText: qsTr("…or pick a running window")
                    Material.roundedScale: theme.radius
                    onActivated: (i) => {
                        const c = backend.clients[i].class
                        root.patchSelected("match", { "class": "^" + c.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + "$" })
                    }
                    popup.onAboutToShow: backend.loadClients()
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Ui.muted
                    font.pixelSize: 10
                    text: root.editingFallback
                          ? qsTr("This profile matches every window that did not match any other.")
                          : qsTr("Leave the regex empty to make a profile the fallback instead.")
                }

                Label {
                    text: qsTr("Pages of %1").arg(root.current ? root.current.name : "")
                    color: Ui.muted
                    font.pixelSize: 11
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: root.current ? root.current.pages.length : 0
                        delegate: CheckBox {
                            id: pageTab
                            required property int index
                            checked: false
                            text: root.current.pages[index].name
                            font.pixelSize: 11
                            font.family: "monospace"
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Page %1 is edited on this same pane; bindings are edited on the dial view.").arg(index + 1)
                            enabled: false     // page choice is display-only for now (the dial view shows page 0)
                        }
                    }
                    Item { Layout.fillWidth: true }
                    ToolButton {
                        objectName: "addPage"
                        text: "+"
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Add a page")
                        onClicked: root.patchSelected("pages", JSON.parse(JSON.stringify(root.current.pages)).concat([{ name: qsTr("Page %1").arg(root.current.pages.length + 1), bindings: {} }]))
                    }
                    ToolButton {
                        objectName: "deletePage"
                        text: "−"
                        visible: root.current ? root.current.pages.length > 1 : false
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Remove the last page")
                        onClicked: {
                            const copy = JSON.parse(JSON.stringify(root.current.pages))
                            copy.pop()
                            root.patchSelected("pages", copy)
                        }
                    }
                }

                Button {
                    objectName: "pinButton"
                    flat: true
                    font.pixelSize: 11
                    text: backend.pinned ? qsTr("Pinned · click to follow windows") : qsTr("Pin this profile")
                    onClicked: backend.pinProfile(backend.pinned ? "auto" : backend.activeProfile)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Pinning keeps a profile on every window until you release it.")
                }

                Button {
                    objectName: "deleteProfile"
                    flat: true
                    font.pixelSize: 11
                    enabled: root.list.length > 1 && !root.editingFallback
                    text: qsTr("Delete profile")
                    onClicked: {
                        const copy = JSON.parse(JSON.stringify(root.list))
                        copy.splice(root.selected, 1)
                        root.selected = -1
                        root.push(copy)
                    }
                }
            }

            // ---- presets ---------------------------------------------------------------
            Label { text: qsTr("Presets"); color: Ui.muted; font.pixelSize: 11; topPadding: 8 }
            ComboBox {
                id: presetBox
                Layout.fillWidth: true
                model: backend.presets
                textRole: "name"
                font.pixelSize: 11
                displayText: backend.presets.length > 0 ? currentText : qsTr("No presets yet")
                enabled: backend.presets.length > 0
                Material.roundedScale: theme.radius
            }
            Flow {
                Layout.fillWidth: true
                spacing: 6
                Button {
                    flat: true; font.pixelSize: 11
                    enabled: presetBox.model.length > 0
                    text: qsTr("Apply")
                    onClicked: backend.applyPreset(presetBox.model[presetBox.currentIndex].name)
                    ToolTip.visible: hovered; ToolTip.text: qsTr("Overwrite the profile list with the preset")
                    ToolTip.delay: 400
                }
                Button {
                    flat: true; font.pixelSize: 11
                    text: qsTr("Save current…")
                    onClicked: presetSave.open()
                }
                Button {
                    flat: true; font.pixelSize: 11
                    enabled: presetBox.model.length > 0
                    text: qsTr("Delete")
                    onClicked: backend.deletePreset(presetBox.model[presetBox.currentIndex].name)
                }
            }
            Popup {
                id: presetSave
                objectName: "presetSave"
                y: parent.height + 4
                width: 200
                padding: 10
                ColumnLayout {
                    spacing: 8
                    Label { color: Ui.muted; font.pixelSize: 11; text: qsTr("Preset name") }
                    TextField {
                        id: presetName
                        objectName: "presetName"
                        Layout.fillWidth: true
                        placeholderText: qsTr("e.g. Work setup")
                        font.pixelSize: 12
                        Material.roundedScale: theme.radius
                        onAccepted: { backend.savePreset(text.trim(), false); text = ""; presetSave.close() }
                    }
                    Button {
                        text: qsTr("Save")
                        highlighted: true
                        Layout.fillWidth: true
                        enabled: presetName.text.trim().length > 0
                        Material.roundedScale: theme.radius
                        onClicked: { backend.savePreset(presetName.text.trim(), false); presetName.text = ""; presetSave.close() }
                    }
                }
            }
        }
    }
}
