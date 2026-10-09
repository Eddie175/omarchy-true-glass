import QtQuick
import Quickshell
import Quickshell.Io
import Quickshell.Hyprland
import qs.Commons
import "Model.js" as Model

// Glass's service: turns the glass on and off as the menu's settings say (all the
// work is in bin/glass), and puts the engine's configuration back after every
// Hyprland config reload, which resets it (a theme switch reloads Hyprland).
Item {
  id: root

  // Injected by omarchy-shell.
  property var shell: null
  property var manifest: null

  readonly property string dir: String(Qt.resolvedUrl(".")).replace(/^file:\/\//, "").replace(/\/$/, "")
  // (outside the plugin's folder: a file written inside it reloads every plugin)
  readonly property string stateDir: (Quickshell.env("XDG_STATE_HOME") || Quickshell.env("HOME") + "/.local/state") + "/omarchy-glass"
  property var applied: null

  // The settings file is made first (with the defaults, on a first start), and only
  // then watched: a watch set on a missing file never hears it being created, so on
  // a fresh install the menu's Turn on went unheard until the shell restarted.
  property bool settingsReady: false
  Process {
    id: prepare
    running: true
    command: ["bash", "-c", "mkdir -p \"$1\" && { [ -s \"$1/settings.json\" ] || printf '%s' \"$2\" > \"$1/settings.json\"; }",
              "glass", root.stateDir, Model.serialize(Model.parse(""))]
    onExited: root.settingsReady = true
  }
  FileView {
    id: settingsFile
    path: root.settingsReady ? root.stateDir + "/settings.json" : ""
    watchChanges: true
    atomicWrites: true
    printErrors: false
    onFileChanged: reload()
    onLoaded: root.settingsChanged(Model.parse(text()))
    onLoadFailed: root.settingsChanged(Model.parse(""))
  }

  // What changed decides what runs: on/off, the rules (Hyprland reloads and the
  // glass is applied again from scratch), the lock screen, or just the live settings
  function settingsChanged(s) {
    var before = root.applied
    root.applied = Model.copy(s)
    if (!before) { run("start"); return }        // (the shell started: the glass comes back on)
    if (before.on !== s.on) { run(s.on ? "start" : "stop"); return }
    if (!s.on) return
    if (before.clearTerminals !== s.clearTerminals) run("terminals")
    if (before.lock !== s.lock) run("lock")
    // (the window rules change: Hyprland reloads, and the glass is applied again
    // from scratch when it has)
    // (Round corners too: the reload puts the theme's own corners back)
    if (before.clearTerminals !== s.clearTerminals || before.light !== s.light || before.round !== s.round || before.lowPower !== s.lowPower) reloadProc.running = true
    else run("settings")
  }

  Connections {
    target: Hyprland
    function onRawEvent(event) {
      if (event.name === "configreloaded") root.run("apply")
    }
  }

  // one bin/glass command at a time, in order (a repeat of the last queued is dropped)
  property var queue: []
  function run(cmd) {
    if (queue.length && queue[queue.length - 1] === cmd) return
    queue = queue.concat([cmd])
    if (!proc.running) next()
  }
  function next() {
    if (!queue.length) return
    proc.command = ["bash", root.dir + "/bin/glass", queue[0]]
    queue = queue.slice(1)
    proc.running = true
  }
  Process {
    id: proc
    // (the shell's corners follow Hyprland's rounding, read when the theme is
    // applied: read again once Glass has set it, or put the theme's back)
    onExited: {
      var cmd = command[command.length - 1]
      if (cmd === "start" || cmd === "apply" || cmd === "stop") Style.scheduleRefresh()
      root.next()
    }
  }

  // Disabled or removed (Omarchy disables a plugin before deleting it): Glass turns
  // itself off and puts everything back. A shell restart also ends the service;
  // bin/glass tells the two apart (the plugin is still enabled after a restart).
  // (from the copy bin/glass keeps in the runtime folder: the plugin's folder may
  // already be deleted by now)
  Component.onDestruction: Quickshell.execDetached(["setsid", "bash", "-c",
    "t=\"${XDG_RUNTIME_DIR:-$HOME/.cache}/omarchy-glass-gone\"; [ -f \"$t/glass\" ] && " +
    "GLASS_DIR=\"$(cat \"$t/dir\")\" GLASS_STATE=\"$t/state\" exec bash \"$t/glass\" gone"])

  Process {
    id: reloadProc
    command: ["hyprctl", "reload"]
  }
}
