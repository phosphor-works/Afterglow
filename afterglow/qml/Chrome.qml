// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick
import QtQuick.Controls as QQC

// The window chrome, drawn over the terminal into a texture on afterglow's
// GPU device. Transparent everywhere except the tab bar and open overlays.
Item {
    id: root
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Terminal controls")

    required property QtObject controller

    SystemPalette { id: systemPalette }

    // Called by the host for every pointer event: true if the chrome takes it.
    function wantsPointer(x: real, y: real): bool {
        if (scriptDialog.active) return true
        if (contextMenu.visible || titlePrompt.visible || confirmPrompt.visible)
            return true
        if (tabBar.visible && y < tabBar.height)
            return true
        if (commandPalette.visible || pasteConfirm.visible || root.controller.clipboardPromptOpen)
            return true
        if (historyScroll.visible) {
            const p = historyScroll.mapFromItem(root, x, y)
            if (historyScroll.contains(p)) return true
        }
        if (searchBar.visible) {
            const p = searchBar.mapFromItem(root, x, y)
            return searchBar.contains(p)
        }
        return false
    }

    // Split labels: a small title in each pane's top-right corner.
    Repeater {
        model: root.controller.paneTitles
        delegate: Rectangle {
            required property var modelData
            x: modelData.x + modelData.width - width - 8
            y: modelData.y + 6
            width: Math.min(label.implicitWidth + 16 + (paneStatus.visible ? paneStatus.width + 6 : 0)
                            + (paneContext.visible ? paneContext.width + 6 : 0), modelData.width * 0.5)
            height: label.implicitHeight + 6
            radius: height / 2
            color: Qt.rgba(systemPalette.window.r, systemPalette.window.g,
                           systemPalette.window.b, 0.85)
            border.color: modelData.focused ? systemPalette.highlight
                                            : Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g,
                                                      systemPalette.windowText.b, 0.15)
            StatusIndicator {
                id: paneStatus
                controller: root.controller
                status: parent.modelData.status || ({})
                anchors.left: parent.left
                anchors.leftMargin: 6
                anchors.verticalCenter: parent.verticalCenter
            }
            Rectangle {
                id: paneContext
                readonly property string kind: parent.modelData.contextKind || ""
                readonly property color tint: kind === "elevated" ? root.controller.badColor
                                            : kind === "remote" ? root.controller.warnColor : systemPalette.highlight
                visible: (parent.modelData.context || "").length > 0
                anchors.left: paneStatus.visible ? paneStatus.right : parent.left
                anchors.leftMargin: paneStatus.visible ? 4 : 6
                anchors.verticalCenter: parent.verticalCenter
                width: visible ? paneContextText.implicitWidth + 8 : 0
                height: parent.height - 6
                radius: 3
                color: Qt.rgba(tint.r, tint.g, tint.b, 0.2)
                Text {
                    id: paneContextText
                    anchors.centerIn: parent
                    text: parent.parent.modelData.context || ""
                    textFormat: Text.PlainText
                    color: parent.tint
                    font.pixelSize: 10
                    font.bold: true
                }
            }
            QQC.Label {
                id: label
                anchors.left: paneContext.visible ? paneContext.right : paneStatus.visible ? paneStatus.right : parent.left
                anchors.leftMargin: paneContext.visible || paneStatus.visible ? 4 : 8
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 16 - (paneStatus.visible ? paneStatus.width + 2 : 0) - (paneContext.visible ? paneContext.width + 4 : 0)
                horizontalAlignment: Text.AlignHCenter
                text: parent.modelData.title.length > 0 ? parent.modelData.title : "shell"
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: systemPalette.windowText
                opacity: parent.modelData.focused ? 0.95 : 0.6
            }
        }
    }

    TabBar {
        id: tabBar
        controller: root.controller
        width: root.width
        height: root.controller.tabBarHeight > 0 ? root.controller.tabBarHeight : 0
        visible: root.controller.tabBarHeight > 0
    }

    QQC.ScrollBar {
        id: historyScroll
        x: root.controller.history.x || 0
        y: root.controller.history.y || 0
        height: root.controller.history.height || 0
        width: 12
        orientation: Qt.Vertical
        size: root.controller.history.size === undefined ? 1 : root.controller.history.size
        visible: size < 1
        active: hovered || pressed
        Accessible.name: qsTr("Scrollback position")
        // Window.scrollbar "auto": an overlay scrollbar. It appears while the view
        // scrolls or the pointer is on it, and fades out after the configured idle.
        // Output that arrives while the view follows the bottom does not reveal it.
        readonly property bool fades: root.controller.history.fade === true
        property bool revealed: false
        property real fromBottom: 0
        opacity: !fades || revealed || hovered || pressed ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
        Timer {
            id: hideTimer
            interval: root.controller.history.hideDelay === undefined ? 1200 : root.controller.history.hideDelay
            onTriggered: historyScroll.revealed = false
        }
        function reveal(): void {
            revealed = true
            hideTimer.restart()
        }
        onHoveredChanged: if (!hovered) reveal()
        onPressedChanged: if (!pressed) reveal()
        property bool syncing: false
        function syncPosition(): void {
            syncing = true
            position = root.controller.history.position || 0
            syncing = false
        }
        onPositionChanged: { if (!syncing) root.controller.scrollHistory(position) }
        Component.onCompleted: syncPosition()
        Connections {
            target: root.controller
            function onHistoryChanged(): void {
                const history = root.controller.history
                const offset = 1 - (history.position || 0) - (history.size === undefined ? 1 : history.size)
                if (Math.abs(offset - historyScroll.fromBottom) > 1e-9) {
                    historyScroll.fromBottom = offset
                    if (historyScroll.visible) historyScroll.reveal()
                }
                if (!historyScroll.pressed) historyScroll.syncPosition()
            }
        }
    }

    SearchBar {
        id: searchBar
        controller: root.controller
        anchors.top: tabBar.visible ? tabBar.bottom : root.top
        anchors.right: root.right
        anchors.margins: 8
        visible: root.controller.searchOpen
    }

    // URL mode: a label to type over each link.
    Repeater {
        model: root.controller.urlHints
        delegate: Rectangle {
            required property var modelData
            x: modelData.x
            y: modelData.y
            z: 50
            width: hintText.implicitWidth + 8
            height: hintText.implicitHeight + 2
            radius: 3
            color: root.controller.warnColor
            Text {
                id: hintText
                anchors.centerIn: parent
                textFormat: Text.StyledText
                // Typed letters dimmed, the rest bold.
                text: "<span style='color:#80000000'>" + parent.modelData.label.substring(0, parent.modelData.typed)
                      + "</span><b>" + parent.modelData.label.substring(parent.modelData.typed) + "</b>"
                color: "#000000"
                font.family: "monospace"
                font.pixelSize: 12
            }
        }
    }

    // Unicode input: the code being typed and what it makes.
    Rectangle {
        visible: root.controller.unicodeInput.active === true
        z: 60
        x: root.controller.unicodeInput.x || 0
        y: (root.controller.unicodeInput.y || 0) + 4
        width: unicodeRow.implicitWidth + 16
        height: unicodeRow.implicitHeight + 10
        radius: 6
        color: systemPalette.window
        border.color: systemPalette.highlight
        Row {
            id: unicodeRow
            anchors.centerIn: parent
            spacing: 10
            Text {
                text: "U+" + (root.controller.unicodeInput.text || "")
                color: systemPalette.windowText
                font.family: "monospace"
                font.pixelSize: 14
            }
            Text {
                text: root.controller.unicodeInput.preview || "\u2026"
                color: systemPalette.windowText
                font.pixelSize: 18
            }
        }
    }

    Rectangle {
        id: keyModeBadge
        anchors.bottom: root.bottom
        anchors.right: root.right
        anchors.margins: 12
        visible: root.controller.keyMode.length > 0
        width: keyModeLabel.implicitWidth + 16
        height: keyModeLabel.implicitHeight + 16
        color: systemPalette.highlight
        radius: 4
        QQC.Label {
            id: keyModeLabel
            anchors.centerIn: keyModeBadge
            text: qsTr("%1 · Esc to cancel").arg(root.controller.keyMode)
            textFormat: Text.PlainText
            color: systemPalette.highlightedText
        }
    }

    Rectangle {
        id: scriptStatusBadge
        anchors.bottom: root.bottom
        anchors.left: root.left
        anchors.margins: 12
        visible: root.controller.scriptStatus.length > 0
        width: Math.min(scriptStatusLabel.implicitWidth + 16, root.width * 0.6)
        height: scriptStatusLabel.implicitHeight + 12
        color: systemPalette.window
        radius: 4
        QQC.Label {
            id: scriptStatusLabel
            anchors.fill: scriptStatusBadge
            anchors.margins: 6
            textFormat: Text.PlainText
            text: root.controller.scriptStatus
            elide: Text.ElideRight
            color: systemPalette.windowText
        }
    }

    ClipboardPrompt {
        controller: root.controller
        anchors.fill: root
        visible: root.controller.clipboardPromptOpen
    }

    PasteConfirm {
        id: pasteConfirm
        controller: root.controller
        anchors.fill: root
        visible: root.controller.pasteConfirmOpen
    }

    CommandPalette {
        id: commandPalette
        controller: root.controller
        anchors.fill: root
        visible: root.controller.paletteOpen
    }

    TitlePrompt {
        id: titlePrompt
        controller: root.controller
        anchors.fill: root
        visible: root.controller.titlePrompt.open === true
    }

    ConfirmPrompt {
        id: confirmPrompt
        controller: root.controller
        anchors.fill: root
        visible: root.controller.confirmPrompt.open === true
    }

    Loader {
        id: scriptDialog
        anchors.fill: root
        active: root.controller.scriptDialog.open === true
        sourceComponent: Component { ScriptDialog { controller: root.controller } }
    }

    ContextMenu {
        id: contextMenu
        controller: root.controller
        anchors.fill: root
        visible: root.controller.contextMenu.open === true
    }
}
