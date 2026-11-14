pragma Singleton
import QtQuick

// Fixed dark palette. Omarchy themes drive the accent (theme.accent); the surfaces stay
// neutral so the dial picture and the accent read the same on every theme.
QtObject {
    readonly property color window: "#0e0e10"
    readonly property color panel: "#16161a"
    readonly property color panelHigh: "#1f1f25"
    readonly property color border: "#2c2c34"
    readonly property color text: "#ececf1"
    readonly property color muted: "#9a9aa6"
    readonly property color ok: "#4cc38a"
    readonly property color warn: "#e5a73b"
    readonly property color bad: "#e5605a"
}
