// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// A program's status (OSC 7501) as a small mark: a spinner or progress arc
// while working, a pulsing dot while blocked on the user, a check when done,
// a cross on error. Hovering shows what the program reported.
Item {
    id: root

    required property QtObject controller
    property var status: ({})

    readonly property string state_: status && status.state ? status.state : ""
    readonly property int progress: status && status.progress !== undefined ? status.progress : -1

    visible: state_ !== ""
    implicitWidth: visible ? 12 : 0
    implicitHeight: 12

    // Working: an arc, spinning while indeterminate.
    Canvas {
        id: arc
        anchors.fill: parent
        visible: root.state_ === "working"
        property real fraction: root.progress >= 0 ? root.progress / 100 : 0.28
        property color tint: root.controller.accent
        onFractionChanged: requestPaint()
        onTintChanged: requestPaint()
        onVisibleChanged: requestPaint()
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
        RotationAnimator on rotation {
            running: arc.visible && root.progress < 0
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
        }
    }

    // Blocked: a dot that breathes, so it reads as "waiting on you".
    Rectangle {
        anchors.centerIn: parent
        width: 8
        height: 8
        radius: 4
        visible: root.state_ === "blocked"
        color: root.controller.warnColor
        SequentialAnimation on opacity {
            running: root.state_ === "blocked"
            loops: Animation.Infinite
            NumberAnimation { from: 1.0; to: 0.35; duration: 700; easing.type: Easing.InOutSine }
            NumberAnimation { from: 0.35; to: 1.0; duration: 700; easing.type: Easing.InOutSine }
        }
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
    }

    Rectangle {
        // What the program said, on hover.
        visible: hover.hovered && label.text.length > 0
        z: 100
        x: 0
        y: root.height + 6
        width: Math.min(label.implicitWidth + 12, 420)
        height: label.implicitHeight + 8
        radius: 4
        color: Qt.rgba(root.controller.background.r, root.controller.background.g, root.controller.background.b, 0.95)
        border.color: Qt.rgba(root.controller.foreground.r, root.controller.foreground.g, root.controller.foreground.b, 0.2)
        Text {
            id: label
            anchors.centerIn: parent
            width: parent.width - 12
            text: root.status && root.status.text ? root.status.text : ""
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: root.controller.foreground
            font.pixelSize: 11
        }
    }
}
