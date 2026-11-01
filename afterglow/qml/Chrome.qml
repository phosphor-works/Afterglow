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

    // How long the history scrollbar takes to fade in or out.
    readonly property int scrollbarFade: 200
    // The most each bottom badge takes while both show: half of what the
    // three 12 px margins leave, so they never overlap.
    readonly property real badgeShare: Math.max(0, (width - 36) / 2)

    // Called by the host for every pointer event: true if the chrome takes it.
    // The history scrollbar does not take the wheel, so the wheel over it still
    // scrolls the pane or reaches the program.
    function wantsPointer(x: real, y: real, wheel: bool): bool {
        if (scriptDialog.active) return true
        if (contextMenu.visible || titlePrompt.visible || confirmPrompt.visible)
            return true
        if (tabBar.visible && y < tabBar.height)
            return true
        if (commandPalette.visible || pasteConfirm.visible || root.controller.clipboardPromptOpen)
            return true
        if (historyScroll.visible && !wheel) {
            const p = historyScroll.mapFromItem(root, x, y)
            if (historyScroll.contains(p)) return true
        }
        if (searchBar.visible) {
            const p = searchBar.mapFromItem(root, x, y)
            return searchBar.contains(p)
        }
        return false
    }

    // The keyboard belongs to the topmost open overlay, so keys never go to
    // one that another hides. An overlay that opens under another (a program's
    // prompt is open) waits; one that closes hands the focus down. The list
    // is the paint order below, topmost first.
    function restoreFocus(): void {
        const overlays = [clipboardPrompt, pasteConfirm, confirmPrompt, contextMenu, scriptDialog.item,
                          titlePrompt, commandPalette, searchBar]
        for (const overlay of overlays) {
            if (overlay && overlay.visible) {
                // The keys are in it already (Tab reached Cancel): an overlay
                // that changes below must not move them back to the default.
                for (let item = root.Window.activeFocusItem; item; item = item.parent) {
                    if (item === overlay) return
                }
                overlay.takeFocus()
                return
            }
        }
    }
    // Later: the overlay sets itself up (and a Loader makes its item) first.
    // A hidden item keeps the active focus, so the walk would find a closed
    // prompt's button when that prompt opens again and leave it there ("Allow
    // once" instead of Deny). So the focus leaves a hidden item first, and a
    // visible focused button keeps it.
    function overlayChanged(): void {
        const focus = root.Window.activeFocusItem
        if (focus && !focus.visible) root.forceActiveFocus()
        Qt.callLater(root.restoreFocus)
    }
    // An overlay can be open before the scene is made, which runs in the
    // background: no visibility change then gives it the keyboard.
    Component.onCompleted: root.overlayChanged()

    // Split labels: a small title in each pane's top-right corner. By count,
    // not by the list: the list changes on every frame of a split animation,
    // and a new list would make every label (and its spinner) again.
    Repeater {
        model: root.controller.paneTitles.length
        delegate: Rectangle {
            required property int index
            readonly property var modelData: root.controller.paneTitles[index] || ({ x: 0, y: 0, width: 0, height: 0, title: "" })
            // Keep clear of the history scrollbar on the focused pane.
            readonly property real scrollbarSpace: modelData.focused && historyScroll.visible ? historyScroll.width : 0
            x: modelData.x + modelData.width - width - 8 - scrollbarSpace
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
                // The window passes the pointer here only for the scrollbar.
                hoverable: false
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
                text: (parent.modelData.title || "").length > 0 ? parent.modelData.title : qsTr("Shell")
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
        width: root.controller.history.width || 0
        orientation: Qt.Vertical
        // The Basic and Fusion handles show only while active or always on.
        // Whether the bar shows at all is up to visible and opacity below.
        policy: QQC.ScrollBar.AlwaysOn
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
        Behavior on opacity { NumberAnimation { duration: root.scrollbarFade; easing.type: Easing.OutCubic } }
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
        onVisibleChanged: root.overlayChanged()
    }

    // URL mode: a label to type over each link.
    Repeater {
        model: root.controller.urlHints
        delegate: Rectangle {
            required property var modelData
            x: modelData.x
            y: modelData.y
            width: hintText.implicitWidth + 8
            height: hintText.implicitHeight + 2
            radius: 3
            color: root.controller.warnColor
            Text {
                id: hintText
                anchors.centerIn: parent
                textFormat: Text.StyledText
                // Typed letters dimmed, the rest bold.
                function escaped(text: string): string {
                    return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
                }
                text: "<span style='color:#80000000'>" + escaped(parent.modelData.label.substring(0, parent.modelData.typed))
                      + "</span><b>" + escaped(parent.modelData.label.substring(parent.modelData.typed)) + "</b>"
                color: "#000000"
                font.family: "monospace"
                font.pixelSize: 12
            }
        }
    }

    // Unicode input: the code being typed and what it makes. Below the
    // cursor, or above it when the cursor is on the last rows; always inside
    // the window.
    Rectangle {
        visible: root.controller.unicodeInput.active === true
        readonly property real below: (root.controller.unicodeInput.y || 0) + 4
        readonly property real above: (root.controller.unicodeInput.top === undefined
                                       ? below - 4 : root.controller.unicodeInput.top) - height - 4
        x: Math.max(0, Math.min(root.controller.unicodeInput.x || 0, root.width - width))
        y: below + height <= root.height ? below : Math.max(0, above)
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
        width: Math.min(keyModeLabel.implicitWidth + 16, scriptStatusBadge.visible ? root.badgeShare : root.width * 0.6)
        height: keyModeLabel.implicitHeight + 16
        color: systemPalette.highlight
        radius: 4
        // The label says how to leave the mode: only the window knows which
        // keys do (a script key mode sees Escape itself).
        QQC.Label {
            id: keyModeLabel
            anchors.fill: keyModeBadge
            anchors.margins: 8
            horizontalAlignment: Text.AlignHCenter
            text: root.controller.keyMode
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: systemPalette.highlightedText
        }
    }

    Rectangle {
        id: scriptStatusBadge
        anchors.bottom: root.bottom
        anchors.left: root.left
        anchors.margins: 12
        visible: root.controller.scriptStatus.length > 0
        width: Math.min(scriptStatusLabel.implicitWidth + 16, keyModeBadge.visible ? root.badgeShare : root.width * 0.6)
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

    CommandPalette {
        id: commandPalette
        controller: root.controller
        anchors.fill: root
        visible: root.controller.paletteOpen
        onVisibleChanged: root.overlayChanged()
    }

    TitlePrompt {
        id: titlePrompt
        controller: root.controller
        anchors.fill: root
        visible: root.controller.titlePrompt.open === true
        onVisibleChanged: root.overlayChanged()
    }

    Loader {
        id: scriptDialog
        anchors.fill: root
        active: root.controller.scriptDialog.open === true
        onActiveChanged: root.overlayChanged()
        // Made inside the background incubation, the item can come after
        // the activation: restoreFocus needs it.
        onLoaded: root.overlayChanged()
        sourceComponent: Component { ScriptDialog { controller: root.controller } }
    }

    ContextMenu {
        id: contextMenu
        controller: root.controller
        anchors.fill: root
        visible: root.controller.contextMenu.open === true
        onVisibleChanged: root.overlayChanged()
    }

    // The prompts that a program, a script or D-Bus opens (a link, a close,
    // a paste, a clipboard request) can open over any overlay and take the
    // keyboard: they come last, so they paint over it. Declaration order is
    // the paint order (every overlay has z 0).
    ConfirmPrompt {
        id: confirmPrompt
        controller: root.controller
        anchors.fill: root
        visible: root.controller.confirmPrompt.open === true
        onVisibleChanged: root.overlayChanged()
    }

    PasteConfirm {
        id: pasteConfirm
        controller: root.controller
        anchors.fill: root
        visible: root.controller.pasteConfirmOpen
        onVisibleChanged: root.overlayChanged()
    }

    ClipboardPrompt {
        id: clipboardPrompt
        controller: root.controller
        anchors.fill: root
        visible: root.controller.clipboardPromptOpen
        onVisibleChanged: root.overlayChanged()
    }
}
