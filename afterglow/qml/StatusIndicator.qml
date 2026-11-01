// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC

// A program's status (OSC 7501, or the progress of OSC 9;4) as a small mark:
// a spinner or progress arc while working, a pulsing dot while blocked on the
// user, a check when done, a cross on error. Where hoverable, hovering shows
// what the program reported.
//
// Every change to the chrome draws a whole window frame, so the marks move in
// steps a few times a second, not at the display rate. A status can last for
// hours.
Item {
    id: root

    required property QtObject controller
    // False where the window does not pass the pointer here (the pane titles).
    property bool hoverable: true
    // The step of the spinner and the dot, and the time of one turn or pulse.
    readonly property int stepInterval: 125
    readonly property int spinPeriod: 1000
    readonly property int pulsePeriod: 1400

    SystemPalette { id: systemPalette }
    property var status: ({})

    readonly property string state_: status && status.state ? status.state : ""
    readonly property int progress: status && status.progress !== undefined ? status.progress : -1

    visible: state_ !== ""
    implicitWidth: visible ? 12 : 0
    implicitHeight: 12

    // For screen readers, which cannot see the mark.
    readonly property string description: {
        let words = ""
        if (state_ === "working")
            words = progress >= 0 ? qsTr("Working, %1 percent").arg(progress) : qsTr("Working")
        else if (state_ === "blocked")
            words = qsTr("Waiting for input")
        else if (state_ === "done")
            words = qsTr("Done")
        else if (state_ === "error")
            words = qsTr("Failed")
        const text = status && status.text ? status.text : ""
        return text.length > 0 && words.length > 0 ? words + ". " + text : words
    }
    Accessible.role: Accessible.Indicator
    Accessible.name: description

    // Counts steps while something moves. Bound to the effective visibility,
    // so a mark in a hidden tab bar asks for no frames.
    property int step: 0
    Timer {
        interval: root.stepInterval
        repeat: true
        running: arc.visible && root.progress < 0 || dot.visible
        onTriggered: ++root.step
    }

    // Working: an arc, spinning while indeterminate.
    Canvas {
        id: arc
        anchors.fill: parent
        visible: root.state_ === "working"
        property real fraction: root.progress >= 0 ? root.progress / 100 : 0.28
        property color tint: systemPalette.highlight
        onFractionChanged: requestPaint()
        onTintChanged: requestPaint()
        onVisibleChanged: requestPaint()
        // A determinate arc starts at 12 o'clock, whatever turn the spinner was at.
        rotation: root.progress < 0 ? (root.step * 360 * root.stepInterval / root.spinPeriod) % 360 : 0
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const r = width / 2 - 1.5
            ctx.lineWidth = 2
            ctx.strokeStyle = Qt.rgba(tint.r, tint.g, tint.b, 0.25)
            ctx.beginPath()
            ctx.arc(width / 2, height / 2, r, 0, Math.PI * 2)
            ctx.stroke()
            ctx.strokeStyle = tint
            ctx.lineCap = "round"
            ctx.beginPath()
            ctx.arc(width / 2, height / 2, r, -Math.PI / 2, -Math.PI / 2 + Math.PI * 2 * fraction)
            ctx.stroke()
        }
    }

    // Blocked: a dot that breathes, so it reads as "waiting on you".
    Rectangle {
        id: dot
        anchors.centerIn: parent
        width: 8
        height: 8
        radius: 4
        visible: root.state_ === "blocked"
        color: root.controller.warnColor
        readonly property real phase: (root.step * root.stepInterval % root.pulsePeriod) / root.pulsePeriod
        opacity: 0.675 + 0.325 * Math.cos(phase * 2 * Math.PI)
    }

    Text {
        anchors.centerIn: parent
        visible: root.state_ === "done" || root.state_ === "error"
        text: root.state_ === "done" ? "✓" : "✕"
        color: root.state_ === "done" ? root.controller.goodColor : root.controller.badColor
        font.pixelSize: 11
        font.bold: true
    }

    HoverHandler {
        id: hover
        enabled: root.hoverable
    }

    QQC.ToolTip {
        parent: root
        visible: root.hoverable && hover.hovered && text.length > 0
        text: root.status && root.status.text
            ? "<span>" + root.status.text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;") + "</span>" : ""
    }
}
