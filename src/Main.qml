import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1180
    height: 740
    minimumWidth: 900
    minimumHeight: 600
    visible: true
    title: qsTr("Dial Studio")
    color: Ui.window

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property string control: ""
    property string slot: ""
    property bool helpVisible: false

    function select(c, s) { control = c; slot = s }

    // Every action shortcut is an ApplicationShortcut, except the single-letter ones, which
    // must not fire while a text field has the focus.
    readonly property bool typing: win.activeFocusItem !== null && win.activeFocusItem instanceof TextInput
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Qt.quit() }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; onActivated: backend.undo() }
    Shortcut { sequences: ["Ctrl+Shift+Z", StandardKey.Redo]; context: Qt.ApplicationShortcut; onActivated: backend.redo() }
    Shortcut { sequence: "Escape"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Ctrl+R"; context: Qt.ApplicationShortcut; onActivated: backend.refresh() }

    // Right-to-left languages mirror the window; the Translator sets the layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ---- header --------------------------------------------------------------
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 56
                color: Ui.panel
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Ui.border }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 20; anchors.rightMargin: 20
                    spacing: 18

                    Label { text: qsTr("Dial Studio"); font.pixelSize: 18; font.weight: Font.DemiBold; color: Ui.text }
                    ConnectionPill { state_: backend.connection }
                    BatteryBadge { percent: backend.battery; charging: backend.charging; visible: backend.daemonRunning }
                    Label {
                        visible: backend.firmware.length > 0
                        text: qsTr("Firmware %1").arg(backend.firmware)
                        color: Ui.muted
                        font.pixelSize: 13
                    }
                    Item { Layout.fillWidth: true }
                    Label { text: qsTr("Haptics"); color: Ui.muted; font.pixelSize: 13; visible: backend.haptics >= 0 }
                    Switch {
                        id: hapticsSwitch
                        visible: backend.haptics >= 0
                        checked: backend.haptics === 1
                        onToggled: backend.setHaptics(checked)
                        Accessible.name: qsTr("Haptics")
                    }
                    ToolButton {
                        text: "?"
                        font.pixelSize: 16
                        onClicked: win.helpVisible = true
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Keyboard shortcuts")
                        Accessible.name: qsTr("Keyboard shortcuts")
                    }
                }
            }

            // ---- banner: the service is not running ---------------------------------
            Rectangle {
                id: banner
                visible: !backend.daemonRunning
                Layout.fillWidth: true
                implicitHeight: bannerRow.implicitHeight + 20
                color: "#3a2c12"
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#5a4416" }
                RowLayout {
                    id: bannerRow
                    anchors.fill: parent
                    anchors.leftMargin: 20; anchors.rightMargin: 20
                    spacing: 14
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        color: "#f3d9a0"
                        font.pixelSize: 13
                        text: qsTr("The Dial Studio service is not running, so the dial keeps its factory behaviour and nothing here can be saved.")
                    }
                    Button {
                        text: backend.serviceBusy ? qsTr("Starting…") : qsTr("Start service")
                        enabled: !backend.serviceBusy
                        highlighted: true
                        Material.roundedScale: theme.radius
                        ToolTip.visible: hovered
                        ToolTip.text: "systemctl --user enable --now dialstudio"
                        onClicked: backend.startService()
                    }
                }
            }
            Rectangle {
                visible: backend.warnings.length > 0
                Layout.fillWidth: true
                implicitHeight: warnColumn.implicitHeight + 16
                color: "#3a1f1d"
                ColumnLayout {
                    id: warnColumn
                    anchors.fill: parent
                    anchors.margins: 8
                    anchors.leftMargin: 20
                    Repeater {
                        model: backend.warnings
                        delegate: Label { required property string modelData; text: modelData; color: "#f2b8b3"; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    }
                }
            }

            // ---- body ----------------------------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 20
                spacing: 20

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    opacity: backend.daemonRunning ? 1 : 0.55
                    DialView {
                        anchors.fill: parent
                        control: win.control
                        slot: win.slot
                        onActivated: (c, s) => win.select(c, s)
                    }
                }

                Inspector {
                    Layout.preferredWidth: 360
                    Layout.fillHeight: true
                    control: win.control
                    slot: win.slot
                    onSlotSelected: (s) => win.slot = s
                }
            }

            // ---- status bar ----------------------------------------------------------
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 32
                color: Ui.panel
                Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Ui.border }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 20; anchors.rightMargin: 20
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        color: backend.messageIsError ? Ui.bad : Ui.muted
                        text: backend.message.length ? backend.message
                              : backend.daemonRunning ? qsTr("Profile %1").arg(backend.profile) : qsTr("Not connected to the service")
                    }
                    Label {
                        text: qsTr("Undo Ctrl+Z · Redo Ctrl+Shift+Z · ? for shortcuts")
                        font.pixelSize: 12
                        color: Ui.muted
                    }
                }
            }
        }

        // ---- shortcut overlay --------------------------------------------------------
        Rectangle {
            anchors.fill: parent
            visible: win.helpVisible
            color: "#cc000000"
            MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 60, 460)
                height: helpColumn.implicitHeight + 40
                radius: theme.radius
                color: Ui.panelHigh
                border.color: Ui.border
                MouseArea { anchors.fill: parent }   // clicks inside must not close it
                ColumnLayout {
                    id: helpColumn
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 10
                    Label { text: qsTr("Keyboard shortcuts"); font.pixelSize: 18; font.weight: Font.DemiBold; color: Ui.text }
                    Repeater {
                        model: [
                            { k: "?", d: qsTr("Show or hide this list") },
                            { k: "Q", d: qsTr("Quit") },
                            { k: "Ctrl+Z", d: qsTr("Undo") },
                            { k: "Ctrl+Shift+Z", d: qsTr("Redo") },
                            { k: "Ctrl+R", d: qsTr("Reload from the service") },
                            { k: "Esc", d: qsTr("Close this list, or stop recording a shortcut") }
                        ]
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Rectangle {
                                Layout.preferredWidth: 120
                                implicitHeight: 26
                                radius: Math.min(4, theme.radius)
                                color: Ui.panel
                                border.color: Ui.border
                                Label { anchors.centerIn: parent; text: modelData.k; color: Ui.text; font.pixelSize: 12; font.family: "monospace" }
                            }
                            Label { Layout.fillWidth: true; text: modelData.d; color: Ui.text; font.pixelSize: 13; wrapMode: Text.Wrap }
                        }
                    }
                }
            }
        }
    }
}
