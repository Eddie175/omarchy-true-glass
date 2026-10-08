import QtQuick
import QtQuick.Controls
import Quickshell
import Quickshell.Io
import qs.Ui
import qs.Commons
import "Model.js" as Model

// The Glass menu (bar widget). Off, it explains Glass and turns it on; on, it
// sets how the glass looks. It writes ~/.local/state/omarchy-glass/settings.json;
// Service.qml applies it (bin/glass) and reports back in status there.
Panel {
  id: root
  moduleName: "io.github.eddie175.glass"
  ipcTarget: "io.github.eddie175.glass"

  readonly property string dir: String(Qt.resolvedUrl(".")).replace(/^file:\/\//, "").replace(/\/$/, "")
  readonly property string stateDir: (Quickshell.env("XDG_STATE_HOME") || Quickshell.env("HOME") + "/.local/state") + "/omarchy-glass"

  property var settings: Model.parse("")
  property string status: "off"
  property bool confirmRemove: false
  property bool lockDesigns: false

  readonly property bool on: settings.on === true
  readonly property bool lowPower: settings.lowPower === true
  readonly property bool building: status === "building"
  readonly property string problem: Model.statusText(status)

  function set(key, value) {
    var s = Model.copy(settings)
    if (key === "lightStyle") s.lightStyle = Model.clampStyle(value, s.lightStyle)
    else if (typeof s[key] === "boolean") s[key] = !!value
    else s[key] = Model.clamp01(value, s[key])
    settings = s
    settingsFile.setText(Model.serialize(s))
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  onOpenedChanged: {
    if (!opened) { confirmRemove = false; return }
    lockCheck.running = true
    // (read again on every open: a file created after the menu loaded isn't watched)
    settingsFile.reload()
    statusFile.reload()
    themeModeFile.reload()
  }

  FileView {
    id: settingsFile
    path: root.stateDir + "/settings.json"
    watchChanges: true
    atomicWrites: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: root.settings = Model.parse(text())
    onLoadFailed: root.settings = Model.parse("")
  }

  FileView {
    id: statusFile
    path: root.stateDir + "/status"
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: root.status = String(text() || "off").trim()
    onLoadFailed: root.status = "off"
  }
  // the theme in use is a light one (bin/glass records it with each apply): the menu
  // offers Light glass there and Dark glass on a dark theme, the one that fits its text
  property bool lightTheme: false
  FileView {
    id: themeModeFile
    path: root.stateDir + "/theme-mode"
    watchChanges: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: root.lightTheme = String(text()).trim() === "light"
    onLoadFailed: root.lightTheme = false
  }

  // the glass lock screen is offered only with the Lock Designs plugin installed
  Process {
    id: lockCheck
    command: ["bash", "-c", "ls \"$HOME/.config/omarchy/plugins\" 2>/dev/null | grep -q 'lock-designs$'"]
    onExited: function(code) { root.lockDesigns = code === 0 }
  }

  BarIconButton {
    id: button
    // (a second click within the double-click time comes as a double click, which the
    // button drops: a menu opened and closed again fast stayed open)
    Connections {
      target: { for (var i = 0; i < button.children.length; i++) if (button.children[i].doubleClicked !== undefined) return button.children[i]; return null }
      function onDoubleClicked(mouse) { if (button.pressable) button.triggerPress(mouse.button) }
    }
    anchors.fill: parent
    bar: root.bar
    tooltipText: "Glass"
    text: String.fromCodePoint(0xF00B5)
    onPressed: function(b) { root.toggle() }
  }

  // A label on the left with its control on the right, and an optional line under it
  component SettingRow: Item {
    id: row
    property string label: ""
    property string note: ""
    default property alias control: slot.data
    width: content.width
    implicitHeight: Math.max(labels.implicitHeight, slot.childrenRect.height)

    Column {
      id: labels
      anchors.left: parent.left
      anchors.right: slot.left
      anchors.rightMargin: Style.space(12)
      anchors.verticalCenter: parent.verticalCenter
      spacing: Style.space(2)

      Text {
        width: parent.width
        text: row.label
        color: root.bar.foreground
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.body
        elide: Text.ElideRight
      }
      Text {
        width: parent.width
        visible: row.note !== ""
        text: row.note
        wrapMode: Text.WordWrap
        color: Qt.darker(root.bar.foreground, 1.4)
        font.family: root.bar.fontFamily
        font.pixelSize: Style.font.caption
      }
    }

    Item {
      id: slot
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      width: childrenRect.width
      height: childrenRect.height
    }
  }

  component Choice: ButtonGroup {
    foreground: root.bar.foreground
    fontFamily: root.bar.fontFamily
    fontSize: Style.font.caption
    focusable: false
  }

  component GlassSwitch: ToggleSwitch {
    foreground: root.bar.foreground
  }

  KeyboardPanel {
    id: panel
    // the bar strip is click-through, so a click on another bar icon goes to the
    // bar and switches menus, as with Omarchy's own menus
    mask: Region {
      width: panel.screenW
      height: panel.screenH
      Region {
        intersection: Intersection.Subtract
        x: panel.barPos === "right" ? panel.screenW - panel._barStripSize : 0
        y: panel.barPos === "bottom" ? panel.screenH - panel._barStripSize : 0
        width: panel.barPos === "left" || panel.barPos === "right" ? panel._barStripSize : panel.screenW
        height: panel.barPos === "left" || panel.barPos === "right" ? panel.screenH : panel._barStripSize
      }
    }
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: keyCatcher
    contentWidth: panel.fittedContentWidth(Style.space(340))
    contentHeight: panel.fittedContentHeight(content.implicitHeight, Style.space(900))

    PanelKeyCatcher {
      id: keyCatcher
      anchors.fill: parent
      onCloseRequested: root.close()
      onTabRequested: function(direction) { root.switchPanel(direction) }

      Column {
        id: content
        width: parent.width
        spacing: Style.space(14)

        // ── header: icon, name and state, and the one switch ─────────────────
        Item {
          width: parent.width
          implicitHeight: Math.max(heroIcon.implicitHeight, heroLabels.implicitHeight, powerSwitch.implicitHeight)

          Text {
            id: heroIcon
            textFormat: Text.PlainText
            text: String.fromCodePoint(0xF00B5)
            color: root.bar.foreground
            opacity: root.on ? 1.0 : 0.5
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.display
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
          }

          GlassSwitch {
            id: powerSwitch
            checked: root.on
            enabled: !root.building
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            onToggled: root.set("on", !root.on)
          }

          Column {
            id: heroLabels
            anchors.left: heroIcon.right
            anchors.leftMargin: Style.space(14)
            anchors.right: powerSwitch.left
            anchors.rightMargin: Style.space(12)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Style.space(2)

            Text {
              text: "Glass"
              color: root.bar.foreground
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.title
              font.bold: true
            }
            Text {
              width: parent.width
              textFormat: Text.PlainText
              text: root.headline()
              color: Qt.darker(root.bar.foreground, 1.4)
              font.family: root.bar.fontFamily
              font.pixelSize: Style.font.caption
              font.bold: true
              font.letterSpacing: 1.2
              elide: Text.ElideRight
            }
          }
        }

        // ── off: what it does ─────────────────────────────────────────────────
        Text {
          visible: !root.on
          width: parent.width
          textFormat: Text.PlainText
          wrapMode: Text.WordWrap
          lineHeight: 1.15
          text: "Your bar, menus, notifications and windows as real glass. It bends what's behind it at the edges and stays clear in the middle. Works with any theme; turning it off puts everything back."
          color: Qt.darker(root.bar.foreground, 1.4)
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.caption
        }

        // ── building, or a problem ────────────────────────────────────────────
        Text {
          // (and with Glass off, when it switched itself off: a crash, or this GPU)
          visible: root.problem !== "" && (root.on || /^(crashed|gpu)/.test(root.status))
          width: parent.width
          textFormat: Text.PlainText
          wrapMode: Text.WordWrap
          lineHeight: 1.15
          text: root.problem
          color: root.building ? Qt.darker(root.bar.foreground, 1.4) : Color.urgent
          font.family: root.bar.fontFamily
          font.pixelSize: Style.font.caption
        }
        // (older graphics: off by default; this turns it on anyway, in Low power)
        Button {
          visible: root.status === "gpu old"
          text: "Try anyway, in Low power"
          fontSize: Style.font.caption
          foreground: root.bar.foreground
          fontFamily: root.bar.fontFamily
          horizontalPadding: Style.spacing.md
          verticalPadding: Style.spacing.controlPaddingY
          bordered: true
          onClicked: {
            var s = Model.copy(root.settings)
            s.tryOldGpu = true; s.lowPower = true; s.on = true
            root.settings = s
            settingsFile.setText(Model.serialize(s))
          }
        }

        // ── on: the settings ──────────────────────────────────────────────────
        Column {
          visible: root.on && root.status === "on"
          width: parent.width
          spacing: Style.space(14)

          PanelSeparator { foreground: root.bar.foreground }

          PanelSectionHeader { text: "GLASS"; foreground: root.bar.foreground; fontFamily: root.bar.fontFamily }

          SettingRow {
            visible: !root.lowPower
            label: "Windows"
            Choice {
              options: Model.FROST_OPTIONS
              value: Model.frostValue(root.settings.windows)
              onChanged: function(v) { root.set("windows", Number(v)) }
            }
          }
          SettingRow {
            visible: !root.lowPower
            label: "Menus"
            Choice {
              options: Model.FROST_OPTIONS
              value: Model.frostValue(root.settings.menus)
              onChanged: function(v) { root.set("menus", Number(v)) }
            }
          }
          SettingRow {
            visible: !root.lowPower
            label: "Depth"
            Choice {
              options: Model.DEPTH_OPTIONS
              value: Model.depthValue(root.settings.depth)
              onChanged: function(v) { root.set("depth", Number(v)) }
            }
          }
          SettingRow {
            visible: !root.lightTheme
            label: "Dark glass"
            note: "Use this setting when using a bright wallpaper"
            GlassSwitch {
              checked: root.settings.dark === true
              onToggled: root.set("dark", !(root.settings.dark === true))
            }
          }
          SettingRow {
            visible: root.lightTheme
            label: "Light glass"
            note: "Use this setting when using a light theme or wallpaper"
            GlassSwitch {
              checked: root.settings.lightGlass === true
              onToggled: root.set("lightGlass", !(root.settings.lightGlass === true))
            }
          }
          SettingRow {
            label: "Round corners"
            note: root.settings.round === true ? "Windows, menus and buttons" : "Off: your theme's own corners"
            GlassSwitch {
              checked: root.settings.round === true
              onToggled: root.set("round", !(root.settings.round === true))
            }
          }

          PanelSeparator { visible: !root.lowPower; foreground: root.bar.foreground }

          SettingRow {
            visible: !root.lowPower
            label: "Window light"
            note: root.settings.light !== false ? "Marks the window you're in" : "Off: windows show their border"
            GlassSwitch {
              checked: root.settings.light !== false
              onToggled: root.set("light", !(root.settings.light !== false))
            }
          }
          Choice {
            visible: !root.lowPower && root.settings.light !== false
            options: Model.LIGHT_STYLES
            value: String(root.settings.lightStyle || 1)
            onChanged: function(v) { root.set("lightStyle", Number(v)) }
          }
          Choice {
            visible: !root.lowPower && root.settings.light !== false
            options: Model.LIGHT_STYLES_GLASS
            value: String(root.settings.lightStyle || 1)
            onChanged: function(v) { root.set("lightStyle", Number(v)) }
          }
          Text {
            // (a light going round redraws the window's edge every frame, even on a still screen)
            width: parent.width
            visible: !root.lowPower && root.settings.light !== false && (root.settings.lightStyle || 1) !== 4
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: "Moving styles use a little power all the time. Outline uses none once it settles."
            color: Qt.darker(root.bar.foreground, 1.4)
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.caption
          }

          PanelSeparator { foreground: root.bar.foreground }

          SettingRow {
            label: "Low power"
            note: root.lowPower ? "Clear, thin glass without the window light" : "Use this setting on older or low-power computers"
            GlassSwitch {
              checked: root.lowPower
              onToggled: root.set("lowPower", !root.lowPower)
            }
          }
          SettingRow {
            label: "Clear terminals"
            note: "See-through, so the glass shows"
            GlassSwitch {
              checked: root.settings.clearTerminals === true
              onToggled: root.set("clearTerminals", !(root.settings.clearTerminals === true))
            }
          }
          SettingRow {
            visible: root.lockDesigns
            label: "Glass lock screen"
            note: "Clock and password field"
            GlassSwitch {
              checked: root.settings.lock === true
              onToggled: root.set("lock", !(root.settings.lock === true))
            }
          }
        }

        PanelSeparator { foreground: root.bar.foreground }

        // ── remove ────────────────────────────────────────────────────────────
        Item {
          width: parent.width
          implicitHeight: removeButton.implicitHeight

          Text {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - removeButton.width - Style.space(10)
            visible: root.confirmRemove
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            text: "Turns Glass off, puts back everything it changed and deletes the plugin."
            color: Qt.darker(root.bar.foreground, 1.4)
            font.family: root.bar.fontFamily
            font.pixelSize: Style.font.caption
          }

          Button {
            id: removeButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            // (red, outlined: it deletes the plugin, not just the glass)
            text: root.confirmRemove ? "Delete" : "Delete Glass plugin…"
            fontSize: Style.font.caption
            foreground: Color.urgent
            fontFamily: root.bar.fontFamily
            horizontalPadding: Style.spacing.md
            verticalPadding: Style.spacing.controlPaddingY
            bordered: false
            onClicked: {
              if (!root.confirmRemove) { root.confirmRemove = true; return }
              root.close()
              Quickshell.execDetached(["bash", "-c", "bash \"$1/bin/glass\" stop; omarchy plugin remove io.github.eddie175.glass --yes", "glass", root.dir])
            }
          }
          // (its outline in the theme's red: the button's own border takes the theme's
          // grey)
          Rectangle {
            anchors.fill: removeButton
            color: "transparent"
            radius: Style.cornerRadius
            border.width: 1
            border.color: Color.urgent
          }
        }
      }
    }
  }

  // the header's state line
  function headline() {
    if (!root.on) return "OFF"
    if (root.building) return "BUILDING THE ENGINE…"
    if (root.status !== "on") return root.status === "relogin" ? "LOG IN AGAIN TO FINISH" : "NOT RUNNING"
    var f = root.lowPower ? "0" : Model.frostValue(root.settings.windows)
    var parts = [f === "0" ? "CLEAR" : f === "1" ? "FROSTED" : "AUTO FROST"]
    if (!root.lowPower && Model.depthValue(root.settings.depth) !== "0") parts.push("THICK")
    if (root.lowPower) parts.push("LOW POWER")
    if (!root.lightTheme && root.settings.dark === true) parts.push("DARK")
    if (root.lightTheme && root.settings.lightGlass === true) parts.push("LIGHT")
    return parts.join(" · ")
  }
}
