import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The right-hand panel: edits one slot (press / long press, or a knob gesture) of the
// selected control. Edits are saved as soon as the draft is valid.
Rectangle {
    id: root
    objectName: "inspector"
    property string control: ""
    property string slot: ""
    signal slotSelected(string slot)

    color: Ui.panel
    radius: theme.radius
    border.color: Ui.border

    readonly property var types: [
        { value: "keys",         text: qsTr("Keyboard shortcut") },
        { value: "omarchy_bind", text: qsTr("Omarchy shortcut") },
        { value: "volume",       text: qsTr("Volume") },
        { value: "media",        text: qsTr("Media") },
        { value: "brightness",   text: qsTr("Brightness") },
        { value: "exec",         text: qsTr("Launch app or command") },
        { value: "hypr",         text: qsTr("Hyprland") },
        { value: "omarchy",      text: qsTr("Omarchy command") },
        { value: "shell",        text: qsTr("Shell command (advanced)") },
        { value: "noop",         text: qsTr("Nothing") }
    ]

    property var draft: ({})          // the action being edited
    property bool loading: false
    property int rev: backend.revision

    function reload() {
        loading = true
        const a = backend.slotAction(control, slot)
        draft = a
        typeBox.currentIndex = a.type ? typeIndex(a.type) : -1
        loading = false
    }
    function typeIndex(type) {
        for (let i = 0; i < types.length; ++i) if (types[i].value === type) return i
        return -1
    }
    // A refused edit (service offline, invalid action) must not leave its draft on screen.
    Connections { target: backend; function onEditFailed() { root.reload() } }
    onControlChanged: reload()
    onSlotChanged: reload()
    onRevChanged: { if (!inEdit) reload() }
    property bool inEdit: false

    // Applies an edit: keep the draft, and save it once it is complete.
    function change(patch) {
        if (loading) return
        const next = Object.assign({}, draft, patch)
        draft = next
        if (backend.draftReady(next)) { inEdit = true; backend.setBinding(control, slot, next); inEdit = false }
    }
    function chooseType(type) {
        if (loading) return
        const fresh = backend.defaultAction(type)
        draft = fresh
        if (backend.draftReady(fresh)) { inEdit = true; backend.setBinding(control, slot, fresh); inEdit = false }
    }

    Flickable {
        anchors.fill: parent
        anchors.margins: 16
        anchors.bottomMargin: 24
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: column
            width: parent.width
            spacing: 14

            Label {
                visible: root.control === ""
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Ui.muted
                text: qsTr("Click a key or the knob to choose what it does.")
                font.pixelSize: 14
            }

            ColumnLayout {
                visible: root.control !== ""
                Layout.fillWidth: true
                spacing: 14

                Label {
                    text: backend.controlTitle(root.control)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                    color: Ui.text
                }

                // Slot picker: press / long press for keys, five gestures for the knob. One per
                // row so long names never wrap unevenly; the bound ones show their action.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Repeater {
                        model: root.control === "" ? [] : backend.slotsOf(root.control).filter(s => s !== "release")
                        delegate: Button {
                            required property string modelData
                            Layout.fillWidth: true
                            checkable: true
                            checked: root.slot === modelData
                            font.pixelSize: 12
                            Material.roundedScale: theme.radius
                            Accessible.name: backend.slotTitle(root.control, modelData)
                            // Drawn here so the selected row is a solid accent fill with readable text,
                            // whatever the theme's accent is.
                            background: Rectangle {
                                radius: Math.min(6, theme.radius)
                                color: parent.checked ? theme.accent : (parent.hovered ? Ui.panelHigh : "transparent")
                            }
                            contentItem: RowLayout {
                                spacing: 8
                                Label {
                                    text: backend.slotTitle(root.control, modelData)
                                    color: parent.parent.checked ? theme.accentForeground : Ui.text
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Label {
                                    text: { root.rev; const l = backend.slotLabel(root.control, modelData); return l.length ? l : "—" }
                                    color: parent.parent.checked ? theme.accentForeground : Ui.muted
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                    Layout.maximumWidth: 140
                                }
                            }
                            onClicked: root.slotSelected(modelData)
                        }
                    }
                }

                Label { text: qsTr("Action"); color: Ui.muted; font.pixelSize: 12 }
                ComboBox {
                    id: typeBox
                    objectName: "typeBox"
                    Layout.fillWidth: true
                    model: root.types
                    textRole: "text"
                    valueRole: "value"
                    displayText: currentIndex >= 0 ? currentText : qsTr("Not bound")
                    Material.roundedScale: theme.radius
                    onActivated: (index) => root.chooseType(root.types[index].value)
                }

                // ---- per-type form --------------------------------------------------
                Loader {
                    id: form
                    Layout.fillWidth: true
                    active: root.control !== "" && !!root.draft.type
                    sourceComponent: {
                        switch (root.draft.type) {
                        case "keys": return keysForm
                        case "omarchy_bind": return omarchyForm
                        case "volume": return volumeForm
                        case "media": return mediaForm
                        case "brightness": return brightnessForm
                        case "exec": return execForm
                        case "hypr": return hyprForm
                        case "omarchy": return omarchyCmdForm
                        case "shell": return shellForm
                        default: return null
                        }
                    }
                }

                RowLayout {
                    spacing: 8
                    Button {
                        id: testButton
                        text: qsTr("Test")
                        highlighted: true
                        enabled: backend.daemonRunning && !!root.draft.type && backend.draftReady(root.draft)
                        Material.roundedScale: theme.radius
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Runs this action now, through the service")
                        onClicked: backend.testAction(root.draft)
                    }
                    Button {
                        text: qsTr("Clear")
                        flat: true
                        enabled: !!root.draft.type
                        Material.roundedScale: theme.radius
                        onClicked: { backend.clearBinding(root.control, root.slot) }
                    }
                }
                Label {
                    visible: !!root.draft.type && !backend.draftReady(root.draft)
                    Layout.fillWidth: true
                    Layout.bottomMargin: 8   // the panel's bottom edge must never cut the note
                    wrapMode: Text.Wrap
                    color: Ui.muted
                    font.pixelSize: 12
                    text: qsTr("Finish the action above and it is saved automatically.")
                }
            }
        }
    }

    // --- forms -----------------------------------------------------------------------
    Component {
        id: keysForm
        ColumnLayout {
            spacing: 8
            Label { text: qsTr("Shortcut"); color: Ui.muted; font.pixelSize: 12 }
            ShortcutRecorder {
                Layout.fillWidth: true
                combo: root.draft.combo || ""
                onRecorded: (combo) => root.change({ combo: combo })
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Ui.muted
                font.pixelSize: 11
                text: qsTr("Typed into the focused window with wtype.")
            }
        }
    }
    Component {
        id: omarchyForm
        ColumnLayout {
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: root.draft.description ? Ui.text : Ui.muted
                text: root.draft.description ? qsTr("Runs “%1”").arg(root.draft.description) : qsTr("Pick one of Omarchy's shortcuts")
            }
            ShortcutPicker {
                Layout.fillWidth: true
                selected: root.draft.description || ""
                onPicked: (description) => root.change({ description: description })
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Ui.muted
                font.pixelSize: 11
                text: qsTr("Runs the shortcut's own command, so it works even though Hyprland ignores simulated keys.")
            }
        }
    }
    Component {
        id: volumeForm
        ColumnLayout {
            spacing: 8
            Label { text: qsTr("What to do"); color: Ui.muted; font.pixelSize: 12 }
            ComboBox {
                Layout.fillWidth: true
                model: [{ v: "up", t: qsTr("Volume up") }, { v: "down", t: qsTr("Volume down") }, { v: "mute", t: qsTr("Mute") }]
                textRole: "t"
                currentIndex: Math.max(0, ["up", "down", "mute"].indexOf(root.draft.action))
                Material.roundedScale: theme.radius
                onActivated: (i) => root.change({ action: model[i].v })
            }
            RowLayout {
                visible: root.draft.action !== "mute"
                Label { text: qsTr("Step (%)"); color: Ui.muted; font.pixelSize: 12 }
                SpinBox {
                    from: 1; to: 20
                    value: root.draft.step || 2
                    editable: true
                    onValueModified: root.change({ step: value })
                }
            }
        }
    }
    Component {
        id: mediaForm
        ColumnLayout {
            spacing: 8
            Label { text: qsTr("What to do"); color: Ui.muted; font.pixelSize: 12 }
            ComboBox {
                Layout.fillWidth: true
                model: [{ v: "play-pause", t: qsTr("Play / Pause") }, { v: "next", t: qsTr("Next track") },
                        { v: "previous", t: qsTr("Previous track") }, { v: "stop", t: qsTr("Stop") }]
                textRole: "t"
                currentIndex: Math.max(0, ["play-pause", "next", "previous", "stop"].indexOf(root.draft.action))
                Material.roundedScale: theme.radius
                onActivated: (i) => root.change({ action: model[i].v })
            }
        }
    }
    Component {
        id: brightnessForm
        ColumnLayout {
            spacing: 8
            Label { text: qsTr("What to do"); color: Ui.muted; font.pixelSize: 12 }
            ComboBox {
                Layout.fillWidth: true
                model: [{ v: "up", t: qsTr("Brighter") }, { v: "down", t: qsTr("Dimmer") }]
                textRole: "t"
                currentIndex: Math.max(0, ["up", "down"].indexOf(root.draft.action))
                Material.roundedScale: theme.radius
                onActivated: (i) => root.change({ action: model[i].v })
            }
        }
    }
    Component {
        id: execForm
        ColumnLayout {
            spacing: 8
            Label {
                visible: !!root.draft.app
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Ui.text
                font.pixelSize: 13
                text: qsTr("Opens %1").arg(root.draft.app || "")
            }
            ComboBox {
                objectName: "appPicker"
                Layout.fillWidth: true
                model: backend.apps
                textRole: "name"
                font.pixelSize: 12
                displayText: qsTr("Pick an installed app…")
                Material.roundedScale: theme.radius
                onActivated: (i) => root.change({ app: model[i].id, command: "" })
                popup.onAboutToShow: backend.loadApps()
            }
            TextField {
                objectName: "execCommand"
                Layout.fillWidth: true
                placeholderText: qsTr("Command, e.g. foot -e htop")
                text: root.draft.command || ""
                selectByMouse: true
                Material.roundedScale: theme.radius
                onTextEdited: root.change({ command: text, app: "" })
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Ui.muted
                font.pixelSize: 11
                text: qsTr("An app is launched through uwsm or gtk-launch by its .desktop id, detached from the service.")
            }
        }
    }
    Component {
        id: hyprForm
        ColumnLayout {
            spacing: 8
            Label { text: qsTr("Dispatch"); color: Ui.muted; font.pixelSize: 12 }
            ComboBox {
                Layout.fillWidth: true
                model: ["workspace", "movetoworkspace", "movefocus", "movewindow", "swapwindow", "togglefloating",
                        "fullscreen", "killactive", "cyclenext", "togglespecialworkspace"]
                currentIndex: Math.max(0, model.indexOf(root.draft.dispatch))
                Material.roundedScale: theme.radius
                onActivated: (i) => root.change({ dispatch: model[i] })
            }
            TextField {
                Layout.fillWidth: true
                placeholderText: qsTr("Argument, e.g. +1 or l")
                text: root.draft.arg || ""
                selectByMouse: true
                Material.roundedScale: theme.radius
                onTextEdited: root.change({ arg: text })
            }
        }
    }
    Component {
        id: omarchyCmdForm
        ColumnLayout {
            spacing: 8
            Label { text: qsTr("Command"); color: Ui.muted; font.pixelSize: 12 }
            ComboBox {
                Layout.fillWidth: true
                model: [{ v: "menu", t: qsTr("Omarchy menu") }, { v: "screenshot", t: qsTr("Screenshot") },
                        { v: "screenrecord", t: qsTr("Screen recording") }, { v: "background-next", t: qsTr("Next background") },
                        { v: "lock", t: qsTr("Lock screen") }]
                textRole: "t"
                currentIndex: Math.max(0, ["menu", "screenshot", "screenrecord", "background-next", "lock"].indexOf(root.draft.action))
                Material.roundedScale: theme.radius
                onActivated: (i) => root.change({ action: model[i].v })
            }
        }
    }
    Component {
        id: shellForm
        ColumnLayout {
            spacing: 8
            TextField {
                Layout.fillWidth: true
                placeholderText: qsTr("sh -c command")
                text: root.draft.command || ""
                selectByMouse: true
                Material.roundedScale: theme.radius
                onTextEdited: root.change({ command: text })
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                color: Ui.warn
                font.pixelSize: 11
                text: qsTr("Runs through sh, so quoting and pipes work. Be careful what you paste here.")
            }
        }
    }
}
