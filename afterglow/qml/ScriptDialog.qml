// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: dialog
    required property QtObject controller
    readonly property bool selector: controller.scriptDialog.kind === "select"
    readonly property var matches: {
        const needle = input.text.toLowerCase()
        const items = controller.scriptDialog.items || []
        return items.map((item, index) => {
            const text = item.title.toLowerCase()
            let at = 0
            let score = 0
            for (let i = 0; i < needle.length; ++i) {
                const next = text.indexOf(needle[i], at)
                if (next < 0) return null
                score += next - at
                at = next + 1
            }
            return {item: item, score: score, index: index}
        }).filter(entry => entry !== null).sort((a, b) => a.score - b.score || a.index - b.index).map(entry => entry.item)
    }
    function accept() {
        if (!selector) controller.answerScriptDialog(true, input.text)
        else if (list.currentIndex >= 0 && list.currentIndex < matches.length)
            controller.answerScriptDialog(true, matches[list.currentIndex].id)
    }
    Component.onCompleted: {
        input.text = selector ? "" : (controller.scriptDialog.text || "")
        input.selectAll()
        input.forceActiveFocus()
    }
    Rectangle {
        anchors.fill: parent
        color: "#59000000"
        MouseArea {
            anchors.fill: parent
            onClicked: dialog.controller.answerScriptDialog(false, "")
        }
    }
    Pane {
        anchors.horizontalCenter: parent.horizontalCenter
        y: parent.height * 0.15
        width: Math.min(620, parent.width * 0.85)
        height: Math.min(content.implicitHeight + topPadding + bottomPadding, parent.height * 0.75)
        padding: 12
        ColumnLayout {
            id: content
            anchors.fill: parent
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: dialog.controller.scriptDialog.title || qsTr("Script")
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                font.bold: true
            }
            TextField {
                id: input
                Layout.fillWidth: true
                maximumLength: 4096
                placeholderText: dialog.selector ? qsTr("Filter choices") : qsTr("Enter text")
                Accessible.name: dialog.controller.scriptDialog.title || placeholderText
                onTextChanged: list.currentIndex = 0
                Keys.onEscapePressed: dialog.controller.answerScriptDialog(false, "")
                Keys.onUpPressed: list.currentIndex = Math.max(0, list.currentIndex - 1)
                Keys.onDownPressed: list.currentIndex = Math.min(list.count - 1, list.currentIndex + 1)
                onAccepted: dialog.accept()
            }
            ListView {
                id: list
                visible: dialog.selector
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: Math.min(contentHeight, 320)
                clip: true
                reuseItems: true
                model: dialog.matches
                highlightMoveDuration: 0
                ScrollBar.vertical: ScrollBar {}
                delegate: ItemDelegate {
                    required property var modelData
                    required property int index
                    width: list.width
                    text: modelData.title
                    highlighted: ListView.isCurrentItem
                    onClicked: dialog.controller.answerScriptDialog(true, modelData.id)
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                Button { text: qsTr("Cancel"); onClicked: dialog.controller.answerScriptDialog(false, "") }
                Button { text: qsTr("Accept"); highlighted: true; onClicked: dialog.accept() }
            }
        }
        Keys.onEscapePressed: dialog.controller.answerScriptDialog(false, "")
    }
}
