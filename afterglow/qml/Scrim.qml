// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick

// The dimmed backdrop behind a centred overlay. A click on it dismisses the
// overlay, the same as Escape.
Rectangle {
    id: scrim

    required property QtObject controller
    signal dismissed()

    anchors.fill: parent
    color: controller.scrimColor

    MouseArea {
        anchors.fill: parent
        onClicked: scrim.dismissed()
    }
}
