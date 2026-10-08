// name: Glass
// description: The clock and the password field are clear glass over your wallpaper
//
// Part of the Glass theme (selected while the theme is active). The glass is drawn
// by glass/glass.frag.qsb with the same optics as the compositor's glass: the
// digits (exact outlines in glass/digits.png) and the capsule refract the
// wallpaper through a curved rim, fringe into colour there, and catch one thin
// edge of light, whose direction follows the pointer a little.
import QtQuick
import QtQuick.Effects
import QtQuick.Window
import Quickshell
import Quickshell.Io
import qs.Commons

DesignBase {
  id: lock
  inputItem: field.input
  shakeOnFail: true
  // (no reveal eye: a lock screen's field is one quiet line of glass)
  showPasswordToggle: false

  readonly property string home: Quickshell.env("HOME")
  readonly property string assets: "file://" + home + "/.config/omarchy/lock-designs/glass"
  // sizes are designed for a 1440 px short side (the 32" at 1.5x, or the tall screen across)
  readonly property real unit: Math.min(width, height) / 1440

  // ── the glass forms on the panels' spring (critically damped, ~0.3 s: large elements read slow) ──
  property real form: 0
  // (it forms once the picture is there: started on load, it had formed behind the
  // dark background while the wallpaper decoded, and then everything popped in)
  readonly property bool ready: (wallpaper.status === Image.Ready || quick.status === Image.Ready) && lock.atlas !== null
  onReadyChanged: if (ready) form = 1
  Timer { interval: 1500; running: true; onTriggered: lock.form = 1 } // (no wallpaper at all: the clock and field still come)
  // (the curve of a critically damped spring: a smooth start, most of the way in
  // the first third, a long soft settle; an ease-out cubic started at full speed: a pop)
  Behavior on form { NumberAnimation { duration: 300; easing.type: Easing.BezierSpline; easing.bezierCurve: [0.25, 0.0, 0.15, 1.0, 1.0, 1.0] } }

  // ── the light follows the pointer a little (the desktop's tilt); still at rest ──
  property real lightAngle: 225          // degrees, screen axes: from the upper left
  readonly property vector2d lightDir: Qt.vector2d(Math.cos(lightAngle * Math.PI / 180), Math.sin(lightAngle * Math.PI / 180))
  Behavior on lightAngle { NumberAnimation { duration: 700; easing.type: Easing.OutCubic } }

  // ── the Tint (the Glass menu): Dark is smoked glass, as on the desktop (the Glass
  // plugin writes glass/settings.json next to this design) ──
  property bool darkGlass: false
  // (a light theme: the wallpaper as it is, and the text in the theme's dark ink with a
  // light glow; white text needed the picture taken down, which turned pastels grey)
  property bool lightUi: false
  FileView {
    path: lock.home + "/.config/omarchy/lock-designs/glass/settings.json"
    printErrors: false
    onLoaded: {
      try { var s = JSON.parse(text()); lock.darkGlass = s.dark === true; lock.lightUi = s.light === true }
      catch (e) { lock.darkGlass = false; lock.lightUi = false }
    }
  }
  // the text's ink and its shadow (a light glow on a light theme)
  function ink(a) { return lock.lightUi ? Qt.rgba(Color.foreground.r, Color.foreground.g, Color.foreground.b, Math.min(1, a + 0.05)) : Qt.rgba(1, 1, 1, a) }
  function inkShadow(a) { return lock.lightUi ? Qt.rgba(1, 1, 1, a * 1.6) : Qt.rgba(0, 0, 0, a) }

  // ── the digit atlas ──
  property var atlas: null
  FileView {
    path: lock.home + "/.config/omarchy/lock-designs/glass/digits.json"
    printErrors: false
    onLoaded: { try { lock.atlas = JSON.parse(text()) } catch (e) { lock.atlas = null } }
  }

  // 12-hour time without the AM/PM (the clock, not a timestamp)
  readonly property string timeText: {
    var h = lock.now.getHours() % 12
    if (h === 0) h = 12
    var m = lock.now.getMinutes()
    return h + ":" + (m < 10 ? "0" : "") + m
  }
  // the clock: digits this tall on the screen (atlas cells are drawn at 300 px)
  readonly property real clockPx: 320 * unit
  readonly property real clockScale: clockPx / 300
  // per glyph: cell left (atlas px from the clock's left), atlas x, atlas width, on
  readonly property var layout: {
    var out = [], pen = 0
    if (!atlas) return { glyphs: out, advance: 0 }
    var pad = atlas._.pad
    for (var i = 0; i < timeText.length && out.length < 5; i++) {
      var c = timeText[i] === ":" ? "colon" : timeText[i]
      var a = atlas[c]
      if (!a) continue
      // (the colon's own advance jammed it against the digits: room either side)
      var room = c === "colon" ? 22 : 0
      pen += room
      out.push(Qt.vector4d(pen - pad, a.x, a.w, 1))
      pen += a.adv + room
    }
    return { glyphs: out, advance: pen }
  }
  readonly property real clockWidth: layout.advance * clockScale
  // the cells' top-left on the screen
  readonly property real clockX: Math.round((width - clockWidth) / 2) - (atlas ? atlas._.pad * clockScale : 0)
  readonly property real clockY: Math.round(height * 0.11)
  readonly property real inkTop: atlas ? clockY + atlas._.inkTop * clockScale : clockY
  readonly property real inkBottom: atlas ? clockY + atlas._.inkBottom * clockScale : clockY + clockPx
  function glyph(i) { return i < layout.glyphs.length ? layout.glyphs[i] : Qt.vector4d(0, 0, 0, 0) }

  Rectangle { anchors.fill: parent; color: Color.background }

  // a fallback for a lock picture the desktop isn't showing (so not in the image
  // cache): half resolution decodes in a fraction of the time and reads sharp on a
  // 4K screen. (An eighth of the size showed as pixelated for a second.)
  Image {
    id: quick
    anchors.fill: parent
    source: wallpaper.source
    fillMode: Image.PreserveAspectCrop
    asynchronous: true
    cache: true
    sourceSize.width: Math.round(width * Screen.devicePixelRatio / 2)
    smooth: true
    mipmap: true
  }

  Image {
    id: wallpaper
    anchors.fill: parent
    // (the same URL the desktop's background uses, with no ?v= and no sourceSize:
    // Qt's image cache then hands over the picture the desktop already decoded, so
    // the lock shows it at full resolution at once instead of decoding it again)
    source: lock.loadBackground && lock.backgroundPath ? "file://" + String(lock.backgroundPath).split("/").map(encodeURIComponent).join("/") : ""
    fillMode: Image.PreserveAspectCrop
    asynchronous: true
    // (kept decoded between locks: the same picture every time, until it changes)
    cache: true
    smooth: true
    mipmap: true
    visible: false
  }
  Image {
    id: digitsImage
    source: lock.assets + "/digits.png"
    visible: false
    smooth: true
    mipmap: false
  }

  // (both images straight to the shader: through a ShaderEffectSource each was
  // drawn again at the item's size first, a resample of the picture and of the
  // atlas's distances)
  ShaderEffect {
    anchors.fill: parent
    visible: opacity > 0
    // (fades in over the small picture: the same image coming into focus)
    opacity: lock.ready ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
    property variant wall: wallpaper.status === Image.Ready ? wallpaper : quick
    property variant digits: digitsImage
    property size size: Qt.size(width, height)
    property rect field: Qt.rect(fieldBox.x, fieldBox.y, fieldBox.width, fieldBox.height)
    property vector4d clock: Qt.vector4d(lock.clockX, lock.clockY, lock.clockScale, lock.atlas ? lock.atlas._.height : 1)
    property vector4d g0: lock.glyph(0)
    property vector4d g1: lock.glyph(1)
    property vector4d g2: lock.glyph(2)
    property vector4d g3: lock.glyph(3)
    property vector4d g4: lock.glyph(4)
    property size atlasSize: lock.atlas ? Qt.size(lock.atlas._.width, lock.atlas._.height) : Qt.size(1, 1)
    property real range: lock.atlas ? lock.atlas._.range : 40
    property real form: lock.form
    // (a gentle capsule rim: on a short capsule a steep one sheared the stripes into a Z)
    property real thick: 18 * lock.unit
    property real bezel: 20 * lock.unit
    // the digits: a rim across most of each stroke (about 19 px of half-width at
    // this size), so a stroke reads as a rounded glass rod with a flat top
    property real digitThick: 40 * lock.unit
    property real digitBezel: 11 * lock.unit
    property real ior: 1.52
    property real abbe: 6
    property vector2d light: lock.lightDir
    property real unit: lock.unit
    property real darkGlass: lock.darkGlass ? 1 : 0
    property real lightUi: lock.lightUi ? 1 : 0
    // (cropped as the desktop shows it: a wallpaper of another shape was stretched
    // over the screen, and moved when it took over from the small picture)
    property vector4d wallCrop: {
      var img = wallpaper.status === Image.Ready ? wallpaper : quick
      var iw = img.implicitWidth, ih = img.implicitHeight
      if (iw <= 0 || ih <= 0 || width <= 0 || height <= 0) return Qt.vector4d(0, 0, 1, 1)
      var ia = iw / ih, sa = width / height
      return ia > sa ? Qt.vector4d((1 - sa / ia) / 2, 0, sa / ia, 1) : Qt.vector4d(0, (1 - ia / sa) / 2, 1, ia / sa)
    }
    fragmentShader: lock.assets + "/glass.frag.qsb"
  }

  // the date, above the time as on a phone: glass-white, a soft shadow for light wallpapers
  Text {
    anchors.horizontalCenter: parent.horizontalCenter
    y: Math.round(lock.inkTop - height - 18 * lock.unit)
    text: Qt.formatDateTime(lock.now, "dddd, MMMM d")
    color: lock.ink(0.9)
    opacity: lock.form
    font.family: "Adwaita Sans"
    font.pixelSize: Math.round(34 * lock.unit)
    font.weight: Font.DemiBold
    layer.enabled: true
    layer.effect: MultiEffect { shadowEnabled: true; shadowColor: lock.inkShadow(0.32); shadowBlur: 0.5; shadowVerticalOffset: 1; shadowHorizontalOffset: 0 }
  }

  MouseArea {
    anchors.fill: parent
    hoverEnabled: true
    onClicked: { lock.wakeRequested(); lock.forcePasswordFocus() }
    onPositionChanged: function(mouse) {
      lock.wakeRequested()
      // up to +-18 degrees across the screen
      lock.lightAngle = 225 + 18 * ((mouse.x / Math.max(1, lock.width)) - 0.5) * 2
    }
  }

  // the capsule the shader draws its glass under (a phone-sized pill: wider read as
  // a form field)
  Item {
    id: fieldBox
    width: Math.round(280 * lock.unit)
    height: Math.round(46 * lock.unit)
    x: Math.round((lock.width - width) / 2)
    y: Math.round(lock.height * 0.80)
  }

  PasswordField {
    id: field
    lock: lock
    x: fieldBox.x
    y: fieldBox.y
    width: fieldBox.width
    height: fieldBox.height
    radius: height / 2
    // (no fill: the glass is the pill. The theme's lock background, 80% of its
    // background colour unless it says otherwise, painted a dark slab over it)
    color: "transparent"
    outlineThickness: 0
    showLockGlyph: false
    // (the typed dots: glass-white; the theme's border colour is fully transparent,
    // as this design draws no outline, and the dots took that colour and never showed)
    accentColor: lock.ink(0.92)
    // (its own placeholder is in the shell's monospace: drawn below in the clock's
    // family instead; its status messages still show here)
    placeholder: ""
    placeholderColor: lock.ink(0.82)
    // (its dots, text and icons in the same unit as the field: at the shell's own size
    // they stayed as big on a zoomed screen, where the field is smaller, as on a plain one)
    fontScale: lock.unit
    opacity: lock.form
    layer.enabled: true
    layer.effect: MultiEffect { shadowEnabled: true; shadowColor: lock.inkShadow(0.36); shadowBlur: 0.45; shadowVerticalOffset: 1; shadowHorizontalOffset: 0 }
  }

  Text {
    anchors.centerIn: fieldBox
    visible: field.input && field.input.text.length === 0 && !lock.authenticatingPassword && lock.failureMessage === ""
    text: "Enter Password"
    color: lock.ink(0.62)
    opacity: lock.form
    font.family: "Adwaita Sans"
    font.pixelSize: Math.round(16 * lock.unit)
    font.weight: Font.Normal
    font.letterSpacing: 0.2 * lock.unit
    layer.enabled: true
    layer.effect: MultiEffect { shadowEnabled: true; shadowColor: lock.inkShadow(0.32); shadowBlur: 0.4; shadowVerticalOffset: 1; shadowHorizontalOffset: 0 }
  }
}
