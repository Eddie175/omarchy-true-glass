// Glass settings, shared by Service.qml and Panel.qml.
//
// ~/.local/state/omarchy-glass/settings.json (the menu writes it; the service
// applies it through bin/glass):
//   on               bool   Glass on or off
//   windows          0 | 0.5 | 1  window glass: clear, auto (clear over the wallpaper,
//                           frosted over windows), frosted
//   menus            0 | 0.5 | 1  the same for menus, panels and notifications
//   depth            0 | 0.6  thin or thick glass
//   dark             bool   Dark glass (dark themes): smoked, so light text reads on a
//                           bright wallpaper
//   lightGlass       bool   Light glass (light themes): milky white, so dark text reads
//                           over anything (light_glass in the file)
//   tryOldGpu        bool   turn Glass on even on graphics it stays off on (try_old_gpu)
//   lowPower         bool   Low power (older computers): clear, thin glass without the
//                           window light, sampled at half resolution; the settings it
//                           overrides are kept for when it's off (low_power in the file)
//   round            bool   round corners on windows, menus and buttons (off: the theme's own,
//                           square on Omarchy's stock themes)
//   clear_terminals  bool   terminal backgrounds see-through, so the glass shows behind text
//   light            bool   a light on the edge of the see-through window you're in
//   light_style      1..6   1 Comet, 2 Halo, 3 Twin (a light running round),
//                           4 Outline, 5 Aura, 6 Prism (the whole edge)
//   lock             bool   the glass lock screen (with the Lock Designs plugin)

var DEFAULTS = { on: false, windows: 0.5, menus: 0.5, depth: 0, dark: false, lightGlass: false, lowPower: false, tryOldGpu: false, round: false, clearTerminals: false,
                 light: true, lightStyle: 1, lock: false }

var LIGHT_STYLES = [
  { value: "1", label: "Comet", tooltip: "A streak of light glides round the edge" },
  { value: "3", label: "Twin", tooltip: "Two streaks, half a lap apart" },
  { value: "2", label: "Halo", tooltip: "A long, soft arc of light, sized to each window, sweeps round the edge" }
]
var LIGHT_STYLES_GLASS = [
  { value: "4", label: "Outline", tooltip: "The whole edge lit, brightest toward the light: steady and always clear" },
  { value: "5", label: "Aura", tooltip: "The whole edge lit, with a soft crest of light going round it" },
  { value: "6", label: "Prism", tooltip: "A slow light round the room; where the edge catches it, it splits into colour" }
]

// Window glass and Menu glass: three states
var FROST_OPTIONS = [
  { value: "0", label: "Clear", tooltip: "Always clear: the picture behind shows through" },
  { value: "0.5", label: "Auto", tooltip: "Clear over the wallpaper, frosted over windows" },
  { value: "1", label: "Frosted", tooltip: "Always frosted" }
]

// Depth: thin or thick glass (thicker than this, the rim smears what is behind it)
var DEPTH_OPTIONS = [
  { value: "0", label: "Thin", tooltip: "A thin pane: a narrow rim" },
  { value: "0.6", label: "Thick", tooltip: "A thick slab: a wider rim that wraps more of the picture round its edge" }
]

function clamp01(v, fallback) {
  var n = Number(v)
  return isFinite(n) ? Math.max(0, Math.min(1, Math.round(n * 100) / 100)) : fallback
}

function clampStyle(v, fallback) {
  var n = Math.round(Number(v))
  return n >= 1 && n <= 6 ? n : fallback
}

function frostValue(v) {
  var n = Number(v)
  if (!isFinite(n)) return "0.5"
  return n < 0.25 ? "0" : n > 0.75 ? "1" : "0.5"
}

function depthValue(v) {
  var n = Number(v)
  return isFinite(n) && n >= 0.3 ? "0.6" : "0"
}

function parse(text) {
  var out = copy(DEFAULTS)
  try {
    var data = JSON.parse(String(text || ""))
    if (data && typeof data === "object") {
      if (typeof data.on === "boolean") out.on = data.on
      out.windows = Number(frostValue(clamp01(data.windows, out.windows)))
      out.menus = Number(frostValue(clamp01(data.menus, out.menus)))
      out.depth = Number(depthValue(clamp01(data.depth, out.depth)))
      if (typeof data.dark === "boolean") out.dark = data.dark
      if (typeof data.light_glass === "boolean") out.lightGlass = data.light_glass
      if (typeof data.low_power === "boolean") out.lowPower = data.low_power
      if (typeof data.try_old_gpu === "boolean") out.tryOldGpu = data.try_old_gpu
      // (a short-lived Tint setting)
      if (data.tone === "dark") out.dark = true
      if (data.tone === "light") out.lightGlass = true
      if (typeof data.round === "boolean") out.round = data.round
      if (typeof data.clear_terminals === "boolean") out.clearTerminals = data.clear_terminals
      if (typeof data.light === "boolean") out.light = data.light
      out.lightStyle = clampStyle(data.light_style, out.lightStyle)
      if (typeof data.lock === "boolean") out.lock = data.lock
    }
  } catch (e) {}
  return out
}

function serialize(s) {
  return JSON.stringify({
    on: s.on === true, windows: s.windows, menus: s.menus, depth: s.depth, dark: s.dark === true, light_glass: s.lightGlass === true, low_power: s.lowPower === true, try_old_gpu: s.tryOldGpu === true, round: s.round === true,
    clear_terminals: s.clearTerminals === true, light: s.light !== false,
    light_style: clampStyle(s.lightStyle, 1), lock: s.lock === true
  }, null, 2) + "\n"
}

function copy(s) {
  return { on: s.on, windows: s.windows, menus: s.menus, depth: s.depth, dark: s.dark, lightGlass: s.lightGlass, lowPower: s.lowPower, tryOldGpu: s.tryOldGpu, round: s.round, clearTerminals: s.clearTerminals,
           light: s.light, lightStyle: s.lightStyle, lock: s.lock }
}

// The service's state (~/.local/state/omarchy-glass/status): what the menu says about it
function statusText(status) {
  var s = String(status || "off").trim()
  if (s === "building") return "Building the glass engine for your Hyprland, once. About a minute, a few on an older computer; it runs at low priority, so you can keep working."
  if (s.indexOf("unsupported") === 0) return "Glass needs Hyprland 0.56. This system has " + s.split(" ")[1] + ", so the glass stays off until Glass is updated for it."
  if (s.indexOf("tools") === 0) return "Building the engine needs: " + s.slice(6) + ". Install base-devel (sudo pacman -S --needed base-devel), then turn Glass on again."
  if (s.indexOf("failed") === 0) return "The glass engine didn't build or load. Details: " + (s.slice(7) || "~/.local/share/omarchy-glass/build.log")
  if (s === "relogin") return "Hyprland was updated. Log out and back in, and Glass builds its engine for the new version."
  if (s.indexOf("crashed") === 0) return "Hyprland crashed while Glass was on, so Glass stayed off this time. Turn it on to try again, or turn on Low power first. Crash report: " + s.slice(8)
  if (s === "gpu old") return "This computer's graphics are older than Glass supports (Intel from before 2015, or an open-source NVIDIA or older AMD driver), so it stays off: on graphics like these the glass can hang the GPU, and Hyprland closes when it does. You can try anyway, in Low power."
  if (s === "gpu software") return "This computer draws the desktop without a graphics driver (software rendering), which is too slow for Glass, so it stays off."
  if (s.indexOf("gpu shaders") === 0) return "Glass's shaders don't build on this graphics chip (" + s.slice(12) + "), so it turned itself off. Nothing else was changed."
  if (s === "restart") return "An older Glass engine is still loaded. Log out and back in, then turn Glass on."
  return ""
}
