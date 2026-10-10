#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include "GlassLayerSurface.hpp"
#include "BuiltInPresets.hpp"
#include "Diagnostics.hpp"
#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "LayerGeometry.hpp"
#include "ShapeField.hpp"
#include "WorkspaceAnimation.hpp"

#include <hyprland/src/config/ConfigManager.hpp>

#include <algorithm>
#include <unistd.h>
#include <cmath>
#include <GLES3/gl32.h>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Misc.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>

#include <cstdlib>
#include <vector>


// Drop shadow geometry under layer glass, logical px
static constexpr float SHADOW_RANGE  = 48.0f;
static constexpr float SHADOW_OFFSET = 12.0f;
// how far a card's shadow can reach (logical px), at the deepest glass (the shader
// scales range x1.6 and offset x2.2 at depth 1): the drawn area and the field must
// cover it, or the shadow is cut off in a straight line below the card
static constexpr float SHADOW_REACH  = SHADOW_RANGE * 1.6f + SHADOW_OFFSET * 2.2f;

static CBox transformedLayerBox(CBox pixelBox, PHLMONITOR monitor) {
    const auto transform = Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform));
    pixelBox.transform(transform, monitor->m_transformedSize.x, monitor->m_transformedSize.y).noNegativeSize().round();
    return pixelBox;
}

CGlassLayerSurface::CGlassLayerSurface(PHLLS layerSurface, bool popups)
    : m_layerSurface(layerSurface), m_popups(popups) {
}

std::optional<CBox> CGlassLayerSurface::logicalBox() const {
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return std::nullopt;

    if (!m_popups)
        return CBox{layerSurface->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT),
                    layerSurface->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};

    if (!layerSurface->m_popupHead)
        return m_ghostLive ? m_ghostLogical : std::nullopt;

    double x1 = INFINITY, y1 = INFINITY, x2 = -INFINITY, y2 = -INFINITY;
    layerSurface->m_popupHead->breadthfirst(
        [&](SP<Desktop::View::CPopup> popup, void*) {
            if (!popup || !popup->m_mapped || !popup->visible())
                return;
            const auto pos  = popup->coordsGlobal();
            const auto size = popup->size();
            if (size.x <= 0 || size.y <= 0)
                return;
            x1 = std::min(x1, pos.x);
            y1 = std::min(y1, pos.y);
            x2 = std::max(x2, pos.x + size.x);
            y2 = std::max(y2, pos.y + size.y);
        },
        nullptr);

    if (!(x2 > x1 && y2 > y1) || !std::isfinite(x1) || !std::isfinite(y1))
        return m_ghostLive ? m_ghostLogical : std::nullopt; // (gone: the close still draws where they were)
    return CBox{x1, y1, x2 - x1, y2 - y1};
}

std::optional<CBox> CGlassLayerSurface::glassBox(PHLMONITOR monitor) const {
    if (!m_popups) {
        const auto layerSurface = m_layerSurface.lock();
        return layerSurface ? LayerGeometry::computeLayerBox(layerSurface, monitor) : std::nullopt;
    }

    auto box = logicalBox();
    if (!box || !monitor)
        return std::nullopt;
    box->translate(-monitor->m_position);
    box->scale(monitor->m_scale).round().noNegativeSize();
    if (!std::isfinite(box->x) || !std::isfinite(box->y) || box->w <= 0.0 || box->h <= 0.0)
        return std::nullopt;
    return box;
}

std::optional<CBox> CGlassLayerSurface::paddedLogicalBox(PHLMONITOR monitor, float paddingPx) const {
    auto box = glassBox(monitor);
    if (!box || !monitor)
        return std::nullopt;

    const float scale = monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;
    box->scale(1.0 / scale).expand(paddingPx / scale).noNegativeSize().round();
    if (!std::isfinite(box->x) || !std::isfinite(box->y) || box->w <= 0.0 || box->h <= 0.0)
        return std::nullopt;
    return box;
}

// How a card comes and goes: one motion::Presence per surface (the motion
// library, vendored in src/motion: the spring model solved
// exactly; its values in tokens/motion.json). A card hanging from the bar uses
// tokens::card() (out of the bar and back), any other tokens::cardInPlace().
// show() and hide() may come at any moment: every channel turns from where it is
// at the speed it has, so a close mid-open (a quick second click) or a reopen
// mid-close is the same motion, never a second code path.
static constexpr float  OPEN_FROM        = 0.90f; // follow-the-fade layers (no held close): their scale at nothing
static constexpr float  PRESENCE_LATENCY = 0.02f; // s: part of how old a content measurement is when it lands
static constexpr auto   MAP_HIDE         = std::chrono::milliseconds(150); // longest a new card waits hidden for its first measurement
static constexpr double CATCH_UP         = 0.12;  // s over which an event seen late is made up (jumping there jumped the card)
static constexpr auto   COLLAPSE_WAIT    = std::chrono::milliseconds(150); // a held card's sudden collapse: how long its fade may take to follow
static constexpr auto   COLLAPSE_HOLD_MAX = std::chrono::milliseconds(600); // (and the most it is held waiting for a measurement to decide)
static constexpr auto   SWITCH_WINDOW    = std::chrono::milliseconds(150); // one card closing and another of its kind opening this close together: a switch
static constexpr auto   SWITCH_HOLD      = std::chrono::milliseconds(100); // a bar card that went without a fade (the shell swapping menus) waits this long for the next
static constexpr auto   STABLE_CARD      = std::chrono::milliseconds(120); // a card's box still this long before it is held for its close

// Out of the bar: the variant plugin:hyprglass:motion picks: 1 late snap, 3 thick
// and bouncy; anything else the melt
static motion::MorphSpec islandSpec() {
    const auto& c = g_pGlobalState->config;
    const int   v = c.motion ? static_cast<int>(**c.motion + 0.5f) : 0;
    return v == 1 ? motion::tokens::island() : v == 3 ? motion::tokens::islandBouncy() : motion::tokens::islandMelt();
}
static motion::tokens::MorphGeometry islandGeometry() {
    const auto& c = g_pGlobalState->config;
    const int   v = c.motion ? static_cast<int>(**c.motion + 0.5f) : 0;
    return v == 1 ? motion::tokens::islandGeometry() : v == 3 ? motion::tokens::islandBouncyGeometry() : motion::tokens::islandMeltGeometry();
}

// the motion a card opens and closes with: out of the bar or in place, or a plain
// fade where it is with animations off
static motion::MorphSpec cardSpec(bool fromBar) {
    return GlassRenderer::reducedMotion() ? motion::tokens::islandFade() : fromBar ? islandSpec() : motion::tokens::islandInPlace();
}
static motion::tokens::MorphGeometry cardGeometry(bool fromBar) {
    return fromBar && !GlassRenderer::reducedMotion() ? islandGeometry() : motion::tokens::islandInPlaceGeometry();
}

static double nowSecs() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static double smooth01d(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// The motion's clock: real time, plus what is being made up of events seen late
// (an open measured ~30 ms after the card appeared, a close seen two measurements
// after its fade began): continuous and never slower than real time.
double CGlassLayerSurface::motionTime(double now) const {
    // (lab: HG_SLOWMO=k runs the motion k times slower, to look at one moment)
    static const double slow = getenv("HG_SLOWMO") ? std::max(1.0, atof(getenv("HG_SLOWMO"))) : 1.0;
    static const double origin = now;
    if (slow > 1.0)
        now = origin + (now - origin) / slow;
    return now + m_warpBase + m_warpLag * smooth01d(0.0, CATCH_UP, now - m_warpAt);
}
void CGlassLayerSurface::catchUp(double now, double lag) {
    m_warpBase += m_warpLag * smooth01d(0.0, CATCH_UP, now - m_warpAt);
    m_warpLag = std::clamp(lag, 0.0, 0.15);
    m_warpAt  = now;
}

void CGlassLayerSurface::noteMapped(bool mapped) {
    if (mapped && !m_seenMapped) {
        m_mapStart = std::chrono::steady_clock::now();
        // a fresh map starts from nothing, so its content opens; the last
        // mapping's outline and box are measured again before any glass shows
        m_presence = 0.0f;
        m_lastDropped = false;
        m_heldValid = false;
        forgetClose();
        // (a reopen while the last close still runs keeps the card's measured box:
        // dropped, the card vanished for the frames before its first measurement)
        if (!m_motionActive) {
            m_contentLocal.reset();
            m_cardExactValid = false;
        }
        m_contentDirty = true;
        m_plainCover   = false;
    }
    m_seenMapped = mapped;
}

// A settled, held card whose window simply goes, without a fade of its own (the
// volume pop-up, the password dialog, reminders: the shell hides their windows the
// moment they close), closes from its held copy as a fading one does. It popped
// out in one frame (Hyprland's own fade, kept instead, drew the client's opaque
// background fading without glass: a black pill).
void CGlassLayerSurface::switchAway() {
    if (m_switchedAway || !m_motionActive || m_motion.shown())
        return;
    m_switchedAway = true;
    namespace ch   = motion::tokens::ch;
    const double t = motionTime(nowSecs());
    m_motion.advance(t);
    // (the new card's glass starts as this one's shape, this frame: this one's glass
    // hands over at once, its content fades where it is, inside the glass moving on;
    // no neck to the bar, no shrinking: one piece of glass, never two)
    for (int k : {ch::Near, ch::Far, ch::Sides})
        m_motion.retarget(k, motion::Spring::smooth(0.2), m_motion.value(k, t), t);
    m_motion.retarget(ch::Scale, motion::Spring::smooth(0.2), m_motion.value(ch::Scale, t), t);
    m_motion.retarget(ch::Source, motion::Spring::smooth(0.05), 0.0, t);
    m_motion.retarget(ch::Fall, motion::Spring::smooth(0.05), 0.0, t);
    m_motion.retarget(ch::Alpha, motion::Spring::smooth(0.09), 0.0, t);
    m_motion.retarget(ch::Glass, motion::Spring::smooth(0.016), 0.0, t);
    damageSampleRegion();
}

void CGlassLayerSurface::switchOthersAway() const {
    const auto self = m_layerSurface.lock();
    if (!self || !g_pGlobalState)
        return;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [raw, other] : g_pGlobalState->layerSurfaces) {
        if (!other || other.get() == this)
            continue;
        const auto ls = other->m_layerSurface.lock();
        if (!ls || ls->m_namespace != self->m_namespace || ls->m_monitor.lock() != self->m_monitor.lock())
            continue;
        // (held for this one, closing already, or its window just went and the close
        // starts next frame)
        if (other->m_hideDeferredUntil) {
            const auto mon = ls->m_monitor.lock();
            other->hideNow(other->motionTime(nowSecs()), mon && mon->m_scale > 0.0f ? mon->m_scale : 1.0f);
            other->switchAway();
        } else if (other->m_motionActive && !other->m_motion.shown() && now - other->m_closeStartAt < SWITCH_WINDOW)
            other->switchAway();
        else if (other->m_unmapAt && now - *other->m_unmapAt < SWITCH_WINDOW)
            other->m_switchAwayWanted = true; // (its close starts next frame: as a hand-over)
    }
}

static std::array<double, 4> edgesOf(const CBox& b);

// The card a new one opens straight out of: on the same monitor, hanging from the
// same bar, open or closing this moment (the shell swaps them in one frame: the old
// one's window goes as the new one's comes). Its shape now, in this quad's px.
std::optional<CBox> CGlassLayerSurface::switchSeedFrom(PHLMONITOR monitor, const CBox& transformBox) const {
    const auto self = m_layerSurface.lock();
    if (!self || !monitor || m_popups || !m_openFromBar || !g_pGlobalState)
        return std::nullopt;
    const auto  now   = std::chrono::steady_clock::now();
    const float scale = monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;
    for (const auto& [raw, other] : g_pGlobalState->layerSurfaces) {
        if (!other || other.get() == this || other->m_popups || !other->m_openFromBar || other->m_barLayer.lock() != m_barLayer.lock())
            continue;
        // (only one of its own kind: the shell swaps a panel for a panel; a toast
        // hanging from the same bar is no part of it, and the calendar opened out
        // of the notification on screen, sliding across from the far corner)
        const auto ls = other->m_layerSurface.lock();
        if (!ls || ls->m_monitor.lock() != monitor || ls->m_namespace != self->m_namespace)
            continue;
        const bool closingNow = other->m_hideDeferredUntil.has_value() ||
                                (other->m_motionActive && !other->m_motion.shown() && now - other->m_closeStartAt < SWITCH_WINDOW);
        const bool goneNow    = other->m_unmapAt && now - *other->m_unmapAt < SWITCH_WINDOW;
        const bool openNow    = ls->m_mapped && other->m_motion.shown() && (other->m_motionActive || other->m_seenMapped);
        if (!closingNow && !goneNow && !openNow)
            continue;
        CBox r = other->m_motionActive ? other->morphFrame(other->motionTime(nowSecs()), scale).rect : other->m_motionCard;
        if (r.w < 2.0 || r.h < 2.0)
            continue;
        r.translate(other->m_quadPos - transformBox.pos());
        return r;
    }
    return std::nullopt;
}

// A close during (or after) an open that came from another card: the shape goes
// back into the bar, like any close, from exactly where it is. The edges are put
// where they are, measured against the bar's own start.
void CGlassLayerSurface::rebaseOntoBar(double t, float monitorScale) {
    namespace ch = motion::tokens::ch;
    if (!m_switchSeed)
        return;
    m_motion.advance(t);
    const CBox now = morphFrame(t, monitorScale).rect;
    m_switchSeed.reset();
    m_motion.setSpec(cardSpec(m_openFromBar), t);
    m_geo = cardGeometry(m_openFromBar);
    CBox seed, drop;
    morphSeed(monitorScale, seed, drop);
    const auto s = edgesOf(seed), c = edgesOf(m_motionCard), e = edgesOf(now);
    const int  nearSide = m_openFromBar ? m_nearSide : 0;
    int nearI = 1, farI = 3, sideA = 0;
    if (nearSide == 1) { nearI = 3; farI = 1; }
    else if (nearSide == 2) { nearI = 0; farI = 2; sideA = 1; }
    else if (nearSide == 3) { nearI = 2; farI = 0; sideA = 1; }
    const auto p = [&](int i) { const double d = c[i] - s[i]; return std::abs(d) < 1e-3 ? 1.0 : std::clamp((e[i] - s[i]) / d, -0.5, 1.5); };
    m_motion.setValue(ch::Near, p(nearI), t);
    m_motion.setValue(ch::Far, p(farI), t);
    m_motion.setValue(ch::Sides, p(sideA), t);
}

// A collapse that turned out to be a resize: the held card lets go and the live one
// carries on at its new size (the motion never left it: nothing re-opens)
void CGlassLayerSurface::releaseCollapseHold() {
    m_closing        = false;
    m_collapseAt.reset();
    m_cardExactValid = false;
    // (the new size is the card now: not a collapse again while it settles)
    m_collapseQuietUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    if (m_hideDeferredUntil) {
        m_hideDeferredUntil.reset();
        m_ghostLive    = false;
        m_ghostHold.reset();
        m_motionActive = false;
        m_unmapAt.reset();
    }
    damageSampleRegion();
}

// the close's motion starts (from where an open out of another card has it)
void CGlassLayerSurface::hideNow(double t, float monitorScale) {
    m_hideDeferredUntil.reset();
    m_hideAt = t;
    if (m_switchSeed)
        rebaseOntoBar(t, monitorScale);
    m_motion.hide(t);
    m_closeStartAt = std::chrono::steady_clock::now();
    m_motionActive = true; // (held at rest, the motion had settled)
}

// ── The card's life: every change of state goes through one of these ─────────
// (the motion's own direction and progress are m_motion's; see phase())

// a fresh map: no close of the last mapping carries over
void CGlassLayerSurface::forgetClose() {
    m_closing = false;
    m_collapseAt.reset();
    m_ghostLive = false;
    m_ghostDone = false;
    m_fadeGhost = false;
    m_unmapAt.reset();
    m_ghostHold.reset();
}

// its fade seen (visibility dropping as only a fade drops it): the close starts
// from here, measured from the frame the fade began in
void CGlassLayerSurface::closeSeen(float presence) {
    m_closing   = true;
    m_closeForm = 1.0f;
    m_closeLow  = presence;
}

// visible again while closing: a reopen (the motion turns back in cameBack)
void CGlassLayerSurface::reopenSeen() {
    m_closing = false;
    m_collapseAt.reset();
}

// a settled card that collapsed in one step: held as it was until a fade
// confirms the close (collapseFades) or it stays (releaseCollapseHold)
void CGlassLayerSurface::collapseSeen(float presence) {
    m_collapseAt       = std::chrono::steady_clock::now();
    m_collapsePresence = presence;
    closeSeen(presence);
    m_firstDropAt = m_probe.resultFrameAt();
}

void CGlassLayerSurface::collapseFades(double t, float monitorScale) {
    m_collapseAt.reset();
    hideNow(t, monitorScale);
}

// back while it was closing (or after its close ran out): the live card again,
// the motion turning back from where it has the card
void CGlassLayerSurface::cameBack(bool turning) {
    m_ghostLive = false;
    m_ghostDone = false;
    m_ghostHold.reset();
    dropPartialHeld();
    if (turning) {
        m_motion.show(motionTime(nowSecs()));
        m_openLive = true;
        m_openPeak = m_presence;
    }
}

// the motion has come to rest: landed, or gone
void CGlassLayerSurface::motionSettled() {
    m_motionActive = false;
    m_openLive     = false;
    if (m_motion.shown()) {
        m_openClamp    = false; // (a close from rest is unclamped: it was never under way)
        m_contentDirty = true;  // measure the settled card: the live value is read relative to it
    } else {
        m_ghostDone = m_ghostLive;
        m_ghostLive = false;
        m_ghostHold.reset();
        dropPartialHeld();
    }
}

// A copy held from a card still fading in (a close before it landed) is only that
// close's: what it shows is the card part way in, its most opaque parts (the
// weather's pills) whole and the rest faint. Kept, the next close started from it
// as if it were the whole card, and the glass shrank to the pills.
void CGlassLayerSurface::dropPartialHeld() {
    if (m_heldPartial)
        m_heldValid = false;
    m_heldPartial = false;
}

// a stack card's fade in place has run out
void CGlassLayerSurface::fadeEnded() {
    m_fadeGhost = false;
    m_ghostLive = false;
    m_ghostDone = true;
    m_ghostHold.reset();
}

CGlassLayerSurface::ECard CGlassLayerSurface::phase() const {
    if (m_fadeGhost)
        return ECard::Fading;
    if (m_hideDeferredUntil)
        return ECard::Held;
    if (m_closing || (m_motionActive && !m_motion.shown()))
        return ECard::Closing;
    if (m_motionActive)
        return ECard::Opening;
    if (m_motion.shown())
        return ECard::Open;
    return m_ghostDone ? ECard::Done : ECard::Hidden;
}

void CGlassLayerSurface::finishCloseNow() {
    const bool closingMotion = m_motionActive && !m_motion.shown();
    if (!closingMotion && !m_fadeGhost && !m_ghostLive)
        return;
    m_motionActive = false;
    m_openLive     = false;
    m_fadeGhost    = false;
    m_ghostDone    = m_ghostLive || m_ghostDone;
    m_ghostLive    = false;
    m_ghostHold.reset();
    damageSampleRegion();
}

bool CGlassLayerSurface::beginUnmapClose() {
    m_seenMapped = false; // (its next map is a fresh one)
    // (already started: this is called from the close event and the fade-out stage)
    if (m_unmapAt)
        return true;
    const auto ls = m_layerSurface.lock();
    if (!ls)
        return false;
    // (a bar card taken away while it was still opening, a menu swapped again before
    // it settled: still the engine's, for the next card to take its glass over; it
    // holds what is on screen this frame, or its glass alone. Left to Hyprland, it
    // vanished and the next card started over from the bar)
    if (!m_popups && m_openFromBar && m_motionActive && m_motion.shown() && !m_ghostLive && !m_ghostDone && m_contentLocal &&
        !g_pGlobalState->layerNamespaceNoHeldClose.contains(ls->m_namespace)) {
        m_unmapAt     = std::chrono::steady_clock::now();
        m_closing     = true;
        m_closeForm   = 1.0f;
        m_closeLow    = m_presence;
        m_firstDropAt = std::chrono::steady_clock::now();
        m_ghostHold   = ls; // (its window is destroyed with the shell's: kept until its glass is handed over)
        damageSampleRegion();
        return true;
    }
    // (a fade seen in its last frame marks it closing; the close itself starts here)
    // (and only from a copy of the card as it is laid out now)
    if (m_popups || m_ghostLive || m_ghostDone || m_motionActive || m_openLive || !m_heldValid || !m_refValid || !m_contentLocal ||
        m_heldRect != *m_contentLocal)
        return false;
    if (g_pGlobalState->layerNamespaceNoHeldClose.contains(ls->m_namespace)) {
        // (a stack of cards: no motion of one shape, they fade where they are; one
        // the shell already faded out is not shown again)
        if (!m_held || !m_held->isAllocated() || m_closing || m_presence < 0.9f)
            return false;
        m_unmapAt     = std::chrono::steady_clock::now();
        m_ghostHold   = ls; // (a client that drops its window without a fade frees it at once: kept while its glass fades)
        m_fadeGhost   = true;
        m_fadeGhostAt = std::chrono::steady_clock::now();
        m_ghostLive   = true;
        damageSampleRegion();
        return true;
    }
    m_unmapAt     = std::chrono::steady_clock::now();
    m_closing     = true;
    m_ghostHold   = ls; // (a client that drops its window without a fade frees it at once: kept while its close runs)
    // (another of its kind opened a moment ago: a switch, so this one fades in place)
    for (const auto& [raw, other] : g_pGlobalState->layerSurfaces) {
        const auto ols = other ? other->m_layerSurface.lock() : nullptr;
        if (other && other.get() != this && ols && ols->m_namespace == ls->m_namespace && other->m_motionActive && other->m_motion.shown() &&
            *m_unmapAt - other->m_openStartAt < SWITCH_WINDOW)
            m_switchAwayWanted = true;
    }
    m_closeForm   = 1.0f;
    m_closeLow    = m_presence; // (a measurement still in flight from its last frame is no reopen)
    m_firstDropAt = std::chrono::steady_clock::now();
    damageSampleRegion();
    return true;
}

void CGlassLayerSurface::startOpen(PHLMONITOR monitor, const CBox& transformBox, const CBox& contentLocal) {
    m_openLive  = true;
    m_openPeak  = 0.0f;
    const bool wasHandingOver = m_switchedAway;
    m_switchedAway = m_switchAwayWanted = false;
    m_openClamp    = true;
    m_openStartAt  = std::chrono::steady_clock::now();
    m_paneAnchor.reset();
    m_liveValid = false; // the texture still holds the last close's final value
    m_liveDirty = true;
    m_refValid  = false; // and the settled measure is taken again once it has opened
    damageSampleRegion(); // the animation's next frame (this runs while the pass draws)
    // from the click that opened it (within 0.8 s, inside this layer), else
    // from the middle of what appeared
    const auto& press = g_pGlobalState->pressGlow;
    const auto  box   = logicalBox();
    const bool  recentPress = press.at.time_since_epoch().count() > 0 &&
                              std::chrono::steady_clock::now() - press.at < std::chrono::milliseconds(800);
    if (box && recentPress && box->containsPoint(press.pos)) {
        CBox a{(press.pos - monitor->m_position) * monitor->m_scale, {1.0, 1.0}};
        a.transform(Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform)),
                    monitor->m_transformedSize.x, monitor->m_transformedSize.y);
        m_openAnchorPx = a.pos() - transformBox.pos();
    } else
        m_openAnchorPx = contentLocal.middle();
    // a card hanging from the bar grows out of it (the close, in reverse)
    // (with animations off it fades where it is: nothing joins it to the bar)
    m_openFromBar = barAnchor(monitor, transformBox, m_drawnLocal, m_openAnchorPx, m_openBarAnchorPx) && !GlassRenderer::reducedMotion();
    m_spanWant    = m_openFromBar && !m_popups;
    m_spanValid   = false;
    m_spanTries   = 0;
    // (another card is open or just closing at this bar: this one comes out of it,
    // read before that card is told to hand over)
    // (this one may itself still be handing its glass over from an earlier swap:
    // going back to it is a swap too, out of the card on screen now)
    // (with animations off each fades on its own: the last one out, this one in)
    const bool handOver = !GlassRenderer::reducedMotion();
    m_switchSeed = handOver && (!m_motionActive || wasHandingOver) ? switchSeedFrom(monitor, transformBox) : std::nullopt;
    if (handOver)
        switchOthersAway();

    // The motion: from nothing, or from wherever a close still running has the
    // card (a quick reopen turns back, at its speed). The ~30 ms between the card
    // appearing and its first measurement are made up over the first frames.
    const double now   = nowSecs();
    const bool   fresh = !m_motionActive || m_switchSeed.has_value();
    if (m_switchSeed) {
        m_motion.setSpec(motion::tokens::islandSwitch(), now);
        m_geo       = motion::tokens::islandSwitchGeometry();
        m_openClamp = false;
    } else {
        m_motion.setSpec(cardSpec(m_openFromBar), now);
        m_geo = cardGeometry(m_openFromBar);
    }
    const auto sinceMap = std::chrono::steady_clock::now() - m_mapStart;
    catchUp(now, fresh && sinceMap < MAP_HIDE ? std::chrono::duration<double>(sinceMap).count() : 0.0);
    m_motion.show(motionTime(now), fresh);
    m_motionActive = true;
    m_ghostLive    = false;
    m_ghostDone    = false;
    m_ghostHold.reset();
}

void CGlassLayerSurface::offerItemSpan(const PHLLS& bar, const CBox& barBox, const SP<Render::IFramebuffer>& barFb, const SP<Render::IFramebuffer>& target) {
    if (!m_spanWant || m_barLayer.lock() != bar)
        return;
    if (++m_spanTries > 60) { // (no underline: a widget that draws none, or a plugin's own window; ~0.35 s)
        m_spanWant = false;
        return;
    }
    const auto key = g_pGlobalState->layerNamespaceFillKey.find(bar->m_namespace);
    if (key == g_pGlobalState->layerNamespaceFillKey.end())
        return;
    const uint32_t rgb    = (static_cast<uint32_t>(key->second[0] * 255.0f + 0.5f) << 16) | (static_cast<uint32_t>(key->second[1] * 255.0f + 0.5f) << 8) |
                            static_cast<uint32_t>(key->second[2] * 255.0f + 0.5f);
    const bool     alongX = m_nearSide <= 1;
    const double   anchor = alongX ? m_quadPos.x + m_openAnchorPx.x : m_quadPos.y + m_openAnchorPx.y;
    double         lo = 0.0, hi = 0.0;
    // (only the strip along the bar's edge facing the card, where the underline is
    // drawn: an icon in a colour near the accent, a blue gauge, read as the item)
    CBox       strip = barBox;
    const double thick = alongX ? barBox.h : barBox.w;          // (the bar's thickness, px: 26 logical)
    const double band  = std::max(6.0, thick * 0.21), reach = thick * 6.7;
    switch (m_nearSide) {
        case 0: strip = {barBox.x, barBox.y + barBox.h - band, barBox.w, band}; break;
        case 1: strip = {barBox.x, barBox.y, barBox.w, band}; break;
        case 2: strip = {barBox.x + barBox.w - band, barBox.y, band, barBox.h}; break;
        default: strip = {barBox.x, barBox.y, band, barBox.h}; break;
    }
    // (only around the click: the readback is synchronous)
    if (alongX) {
        const double x0 = std::max(strip.x, anchor - reach - 4.0), x1 = std::min(strip.x + strip.w, anchor + reach + 4.0);
        strip = {x0, strip.y, std::max(x1 - x0, 0.0), strip.h};
    } else {
        const double y0 = std::max(strip.y, anchor - reach - 4.0), y1 = std::min(strip.y + strip.h, anchor + reach + 4.0);
        strip = {strip.x, y0, strip.w, std::max(y1 - y0, 0.0)};
    }
    if (!ShapeField::accentSpan(barFb, strip, alongX, anchor, reach, rgb, lo, hi, target)) {
        if (m_spanTries == 1)
        return;
    }
    // (the click lands on the item, and the item's underline is centred under it at
    // 0.55 of its width: an underline further from the click than that is another
    // item's, still drawn as it fades out; look again until this one's shows)
    {
        const double w    = hi - lo, slack = 0.41 * w + 3.0;
        const double dist = anchor < lo ? lo - anchor : anchor > hi ? anchor - hi : 0.0;
        if (dist > slack)
            return;
    }
    m_spanLo    = lo - (alongX ? m_quadPos.x : m_quadPos.y);
    m_spanHi    = hi - (alongX ? m_quadPos.x : m_quadPos.y);
    m_spanValid = true;
    m_spanAt    = std::chrono::steady_clock::now();
    m_spanWant  = false;
}

bool CGlassLayerSurface::barAnchor(PHLMONITOR monitor, const CBox& transformBox, const CBox& card, const Vector2D& near, Vector2D& anchor) const {
    // the bar: a popup's own layer, else the glassed shape-box layer on this monitor
    // (a card hanging from it on its own layer, like the calendar); both boxes in the
    // quad's px, so a rotated monitor needs nothing more
    const auto layerSurface = m_layerSurface.lock();
    if (!monitor || card.w <= 0 || card.h <= 0)
        return false;
    std::optional<CBox> barRaw;
    if (m_popups && layerSurface)
        barRaw = LayerGeometry::computeLayerBox(layerSurface, monitor);
    else {
        for (const auto& [raw, state] : g_pGlobalState->layerSurfaces) {
            const auto ls = state ? state->getLayerSurface() : nullptr;
            if (ls && ls != layerSurface && ls->m_mapped && ls->m_monitor.lock() == monitor &&
                state->resolveMaskSource() == EMaskSource::SHAPE_BOX) {
                barRaw      = LayerGeometry::computeLayerBox(ls, monitor);
                m_barLayer  = ls;
                break;
            }
        }
    }
    if (!barRaw)
        return false;
    CBox         bar    = transformedLayerBox(*barRaw, monitor).translate(-transformBox.pos());
    const double ax     = std::clamp(near.x, card.x, card.x + card.w);
    const double ay     = std::clamp(near.y, card.y, card.y + card.h);
    const double reach  = 64.0 * monitor->m_scale;
    // (the bar runs the long way of its own box: on the tall screen, rotated in the
    // buffer, its box is 39 x 2160, and a card starting a few px inside it overlapped
    // it "along x" too, so the bar was taken as below the card: the drop opened from
    // the wrong side, and held to 66 px across, as a thin strip)
    const bool   barAcross = bar.w >= bar.h;
    const bool   alongX = barAcross && bar.x < card.x + card.w && bar.x + bar.w > card.x;
    const bool   alongY = !barAcross && bar.y < card.y + card.h && bar.y + bar.h > card.y;
    // (the card may start a little inside the bar's box: the shell places it from
    // the bar's visible height, and at scale 1.5 the 26 px bar's box is 39 px while
    // the calendar starts at 32; requiring it below the box's edge took every card
    // on a large screen for one that didn't hang from the bar. Up to half the bar's
    // thickness counts, and the bar's edge is then taken where the card begins.)
    const double inX = 0.5 * bar.w, inY = 0.5 * bar.h;
    // (the bar keeps its own edge: the card here is the probe's, in 16 px cells, and
    // trimming the bar to it put the bar's edge 7 px above its real one on the 32":
    // the drop came out of the bar's inside, not its bottom edge)
    if (alongX && bar.y < card.y && bar.y + bar.h <= card.y + inY && card.y - (bar.y + bar.h) <= reach) {
        anchor      = {ax, std::max(card.y, bar.y + bar.h)};
        m_barAlongX = true;
        m_nearSide  = 0;
    } else if (alongX && bar.y + bar.h > card.y + card.h && bar.y >= card.y + card.h - inY && bar.y - (card.y + card.h) <= reach) {
        anchor      = {ax, std::min(card.y + card.h, bar.y)};
        m_barAlongX = true;
        m_nearSide  = 1;
    } else if (alongY && bar.x < card.x && bar.x + bar.w <= card.x + inX && card.x - (bar.x + bar.w) <= reach) {
        anchor      = {std::max(card.x, bar.x + bar.w), ay};
        m_barAlongX = false;
        m_nearSide  = 2;
    } else if (alongY && bar.x + bar.w > card.x + card.w && bar.x >= card.x + card.w - inX && bar.x - (card.x + card.w) <= reach) {
        anchor      = {std::min(card.x + card.w, bar.x), ay};
        m_barAlongX = false;
        m_nearSide  = 3;
    } else
        return false;
    m_barPx = bar;
    return true;
}

void CGlassLayerSurface::startClose(PHLMONITOR monitor, const CBox& transformBox, bool held, bool deferHide) {
    const auto steadyNow = std::chrono::steady_clock::now();
    const double now     = nowSecs();
    // drawn by the compositor from the held card past the client's unmap (without
    // one, the glass leaves with the client's own fading content)
    m_ghostLive = held;
    m_ghostDone = false;
    if (held) {
        m_ghostHold    = m_layerSurface.lock();
        m_ghostLogical = m_lastLogical;
    }
    if (!m_motionActive && !m_motion.shown()) {
        // a card the motion never opened (there before the engine was): taken over at rest
        m_openFromBar = barAnchor(monitor, transformBox, m_drawnLocal, m_openAnchorPx, m_openBarAnchorPx);
    m_spanWant    = m_openFromBar && !m_popups;
    m_spanValid   = false;
    m_spanTries   = 0;
        m_motion.setSpec(cardSpec(m_openFromBar), motionTime(now));
        m_geo = cardGeometry(m_openFromBar);
        m_motion.snapShown(motionTime(now));
    }
    // (seen two measurements after its fade began: that time made up as it starts)
    catchUp(now, steadyNow - m_firstDropAt < MAP_HIDE ? std::chrono::duration<double>(steadyNow - m_firstDropAt).count() : 0.0);
    m_switchedAway = false;
    // (a bar card the shell took away in one frame: the menus swapping. It stays as
    // it is, drawn from its held copy, until the next card takes its glass over, or
    // closes as any card does if none comes)
    if (deferHide && !m_switchAwayWanted) {
        m_hideDeferredUntil = steadyNow + (m_collapseAt ? COLLAPSE_HOLD_MAX : SWITCH_HOLD);
        m_motionActive      = true;
        damageSampleRegion();
        return;
    }
    hideNow(motionTime(now), monitor && monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f);
    if (m_switchAwayWanted) {
        m_switchAwayWanted = false;
        switchAway();
    }
    // (from full: the armed held copy kept the card whole from the client's first
    // fading frame, so the close takes the content from there, as the open brings it)
    m_motionActive = true;
    damageSampleRegion();
}

// Omarchy's corner radius (the shell mirrors decoration:rounding), framebuffer px
static float cardRadiusPx(float monitorScale) {
    const auto  value = Config::mgr()->getConfigValue("decoration:rounding");
    const auto* p     = reinterpret_cast<Hyprlang::INT* const*>(value.dataptr);
    return static_cast<float>(p && *p ? **p : 20) * monitorScale;
}

static double mix(double a, double b, double t) {
    return a + (b - a) * t;
}

// a box's edges as x0 y0 x1 y1
static std::array<double, 4> edgesOf(const CBox& b) {
    return {b.x, b.y, b.x + b.w, b.y + b.h};
}
static CBox boxOf(const std::array<double, 4>& e) {
    const double x0 = std::min(e[0], e[2]), x1 = std::max(e[0], e[2]);
    const double y0 = std::min(e[1], e[3]), y1 = std::max(e[1], e[3]);
    return {x0, y0, x1 - x0, y1 - y0};
}

void CGlassLayerSurface::morphSeed(float monitorScale, CBox& seed, CBox& bar) const {
    const CBox& C = m_motionCard;
    bar           = CBox{0.0, 0.0, 0.0, 0.0};
    // (opened from another card: from that card's shape, apart from the bar)
    if (m_switchSeed) {
        seed = *m_switchSeed;
        return;
    }
    if (!m_openFromBar) {
        // where it is: the card, smaller, about its middle
        seed = CBox{C.middle().x - C.w * m_geo.seedAlong * 0.5, C.middle().y - C.h * m_geo.seedAway * 0.5, C.w * m_geo.seedAlong, C.h * m_geo.seedAway};
        return;
    }
    // From the bar: the bar's own glass is the liquid. The card starts inside the
    // bar (where the bar draws itself, so nothing of it shows), sourceWidth wide at
    // the item it was opened from, and pushes out through the bar's edge.
    const CBox&  B      = m_barPx;
    const bool   alongX = m_nearSide <= 1;
    // (as wide as the item it opens from, when the bar showed which: a card out of
    // a small icon starts as that icon, not as a wider drop over its neighbours)
    const double sb     = spanBlend();
    const double item   = mix(std::min(m_geo.sourceWidth, DROP_MAX) * monitorScale, dropItem(monitorScale), sb);
    const double mid    = 0.5 * (m_spanLo + m_spanHi);
    const Vector2D at   = alongX ? Vector2D{mix(m_openAnchorPx.x, mid, sb), m_openAnchorPx.y} : Vector2D{m_openAnchorPx.x, mix(m_openAnchorPx.y, mid, sb)};
    // (an item near the card's edge, the paw in the screen's corner: the drop narrows
    // to stay centred on it; pushed inside the card at full width, it sat over the
    // neighbouring item, the power button, and seemed to come out of that)
    const double along  = alongX ? at.x : at.y, cLo = alongX ? C.x : C.y, cHi = alongX ? C.x + C.w : C.y + C.h;
    const double room   = std::max(2.0 * std::min(along - cLo, cHi - along), 16.0 * monitorScale);
    const double width  = std::min({item, alongX ? C.w : C.h, room});
    const double depth  = std::max(m_geo.seedAway * (alongX ? B.h : B.w), 1.0);
    // (a popup draws only inside its own box: it starts as a sliver along the edge
    // facing the bar, and has no neck)
    const double edge   = m_popups ? (m_nearSide == 0 ? C.y : m_nearSide == 1 ? C.y + C.h : m_nearSide == 2 ? C.x : C.x + C.w)
                                   : (m_nearSide == 0 ? B.y + B.h : m_nearSide == 1 ? B.y : m_nearSide == 2 ? B.x + B.w : B.x);
    const double d      = m_popups ? 2.0 : depth;
    if (alongX) {
        const double ax = std::clamp(at.x, C.x + width * 0.5, C.x + C.w - width * 0.5);
        seed = CBox{ax - width * 0.5, m_nearSide == 0 ? edge - (m_popups ? 0.0 : d) : edge - (m_popups ? d : 0.0), width, d};
    } else {
        const double ay = std::clamp(at.y, C.y + width * 0.5, C.y + C.h - width * 0.5);
        seed = CBox{m_nearSide == 2 ? edge - (m_popups ? 0.0 : d) : edge - (m_popups ? d : 0.0), ay - width * 0.5, d, width};
    }
    if (!m_popups)
        bar = B;
}

// The edges of the card's rect for edge progresses (near, far, sides): each from
// the seed's edge to the card's, the near one facing the bar.
static std::array<double, 4> morphEdges(const CBox& seed, const CBox& C, int nearSide, double pNear, double pFar, double pSides) {
    const auto s = edgesOf(seed), c = edgesOf(C);
    // which of x0 y0 x1 y1 is near, far, and the two sides
    int nearI = 1, farI = 3, sideA = 0, sideB = 2;
    if (nearSide == 1) { nearI = 3; farI = 1; }
    else if (nearSide == 2) { nearI = 0; farI = 2; sideA = 1; sideB = 3; }
    else if (nearSide == 3) { nearI = 2; farI = 0; sideA = 1; sideB = 3; }
    std::array<double, 4> e{};
    e[nearI] = mix(s[nearI], c[nearI], pNear);
    e[farI]  = mix(s[farI], c[farI], pFar);
    e[sideA] = mix(s[sideA], c[sideA], pSides);
    e[sideB] = mix(s[sideB], c[sideB], pSides);
    // (a far edge springing past its place never crosses the near one: the drop
    // flattens into the bar, it doesn't turn inside out)
    if (farI > nearI) e[farI] = std::max(e[farI], e[nearI]);
    else e[farI] = std::min(e[farI], e[nearI]);
    // (nor the sides each other, past the middle)
    if (e[sideA] > e[sideB]) e[sideA] = e[sideB] = 0.5 * (e[sideA] + e[sideB]);
    return e;
}

CGlassLayerSurface::SMorphFrame CGlassLayerSurface::morphFrame(double t, float monitorScale) const {
    namespace ch = motion::tokens::ch;
    SMorphFrame f;
    CBox        seed, drop; // (drop: the bar, when it comes out of it)
    morphSeed(monitorScale, seed, drop);
    const CBox& C        = m_motionCard;
    const int   nearSide = m_openFromBar ? m_nearSide : 0;
    const double pNear = m_motion.value(ch::Near, t), pFar = m_motion.value(ch::Far, t), pSides = m_motion.value(ch::Sides, t);
    f.rect = boxOf(morphEdges(seed, C, nearSide, pNear, pFar, pSides));
    // Opening out of the bar, the drop keeps a drop's proportions: what shows of it
    // below the bar is never more than about 1.8 times as wide as it is tall, or as
    // tall as it is wide (more for a card that is itself longer than that). Its
    // edges' springs alone made a wide card a flat slab under the bar first, and a
    // tall one a needle; delaying one axis only swapped which.
    if (m_openClamp && m_openFromBar && drop.w > 0.0) {
        const bool   alongX = nearSide <= 1;
        const double wC = alongX ? C.w : C.h, hC = alongX ? C.h : C.w, wS = alongX ? seed.w : seed.h;
        const double rW = std::max(1.8, 1.2 * wC / std::max(hC, 1.0)), rH = std::max(1.8, 1.2 * hC / std::max(wC, 1.0));
        double       w = alongX ? f.rect.w : f.rect.h;
        // (away from the bar: how far it reaches past the bar's edge)
        const double barEdge = nearSide == 0 ? seed.y + seed.h : nearSide == 1 ? seed.y : nearSide == 2 ? seed.x + seed.w : seed.x;
        const double far     = nearSide == 0 ? f.rect.y + f.rect.h : nearSide == 1 ? f.rect.y : nearSide == 2 ? f.rect.x + f.rect.w : f.rect.x;
        const double vis     = std::max(0.0, nearSide == 0 || nearSide == 2 ? far - barEdge : barEdge - far);
        const double wMax    = std::max(wS, rW * vis);
        if (w > wMax) {
            const double mid = alongX ? f.rect.x + 0.5 * f.rect.w : f.rect.y + 0.5 * f.rect.h;
            if (alongX) { f.rect.x = mid - 0.5 * wMax; f.rect.w = wMax; }
            else        { f.rect.y = mid - 0.5 * wMax; f.rect.h = wMax; }
            w = wMax;
        }
        const double hMax = rH * w;
        if (vis > hMax) {
            const double cut = vis - hMax;
            switch (nearSide) {
                case 0: f.rect.h -= cut; break;
                case 1: f.rect.y += cut; f.rect.h -= cut; break;
                case 2: f.rect.w -= cut; break;
                default: f.rect.x += cut; f.rect.w -= cut; break;
            }
        }
    }
    // corners: the seed's (a pill out of the bar; a softer card where it is) to the card's
    const double rCard = std::min<double>(cardRadiusPx(monitorScale), 0.5 * std::min(C.w, C.h));
    const double rSeed = m_switchSeed ? std::min(rCard, 0.5 * std::min(seed.w, seed.h)) :
        m_openFromBar ? 0.5 * std::min(seed.w, seed.h) : std::min(0.5 * std::min(seed.w, seed.h), rCard * 2.5);
    const double prog  = std::clamp((pNear + pFar + pSides) / 3.0, 0.0, 1.0);
    f.radius           = static_cast<float>(std::min(mix(rSeed, rCard, prog), 0.5 * std::min(f.rect.w, f.rect.h)));
    // the drip: the card hangs a little past its place (away from the bar)
    const double fallPx = m_geo.fall * monitorScale * m_motion.value(ch::Fall, t);
    const Vector2D away = nearSide == 0 ? Vector2D{0.0, 1.0} : nearSide == 1 ? Vector2D{0.0, -1.0} : nearSide == 2 ? Vector2D{1.0, 0.0} : Vector2D{-1.0, 0.0};
    f.rect.translate(away * fallPx);
    // ... held to the bar by an hourglass waist at the item it came from, from just
    // inside the bar to just inside the card, that thins, pinches and snaps (the
    // shader draws it from waistN; the bar draws itself, so only what is outside
    // the bar shows)
    if (drop.w > 0.0) {
        const double n     = std::clamp(m_motion.value(ch::Source, t), 0.0, 1.0);
        const bool   alongX = nearSide <= 1;
        const double cardW  = alongX ? C.w : C.h;
        f.source       = drop;
        f.sourceRadius = 0.0f;
        f.neck         = static_cast<float>(m_geo.neck * monitorScale * std::min(1.0, n * 3.0));
        // (never wider than the card is now: sized from the settled card, a closing
        // card gathering in left the waist's ends sticking out past it, wings and
        // bands under the bar)
        const double nowW = alongX ? f.rect.w : f.rect.h;
        // (and its end inside the card's flat top, between its rounded corners: as wide
        // as the card, the waist met each corner in a little hook)
        // (a fillet's width inside it, so the smooth union rounds the meeting into one curve)
        const double flat = std::max(0.5 * nowW - f.radius - 0.75 * m_geo.neck * monitorScale, 0.2 * nowW);
        f.waistHalf    = static_cast<float>(std::min({0.5 * m_geo.waist * cardW, 0.5 * m_geo.waistMax * monitorScale, flat}));
        f.waistN       = static_cast<float>(n);
        f.waistAlongX  = alongX;
        f.waistHold    = static_cast<float>(m_geo.waistHold);
        f.waistSnap    = static_cast<float>(m_geo.waistSnap);
        const CBox&  r     = f.rect;
        const double into  = 4.0 * monitorScale;
        const double depth = std::min(0.25 * (alongX ? r.h : r.w), 30.0 * monitorScale);
        // (along the bar: at the item, kept inside the card)
        const double lo = alongX ? r.x : r.y, hi = alongX ? r.x + r.w : r.y + r.h;
        // (the item it came out of, from the bar's underline: Omarchy's is 0.55 of the
        // item's width; at the bar the waist is the item's width, never wider)
        const double sb   = spanBlend();
        const double item = dropItem(monitorScale);
        f.waistBarHalf    = sb > 0.0 ? static_cast<float>(mix(f.waistHalf, std::min(std::max(0.5 * item, 5.0 * monitorScale), static_cast<double>(f.waistHalf)), sb)) : 0.0f;
        const double at   = mix(alongX ? m_openAnchorPx.x : m_openAnchorPx.y, 0.5 * (m_spanLo + m_spanHi), sb);
        // (an item near the card's edge: the waist narrows to stay on it; at full width
        // it was pushed inside the card, onto the neighbouring item)
        const double edgeRoom = std::min(at - lo, hi - at);
        if (edgeRoom < f.waistHalf) {
            f.waistHalf    = static_cast<float>(std::max(edgeRoom, 8.0 * monitorScale));
            f.waistBarHalf = std::min(f.waistBarHalf, f.waistHalf);
        }
        const double c  = std::clamp(at, lo + f.waistHalf, std::max(lo + f.waistHalf, hi - f.waistHalf));
        double a = 0.0, b = 0.0; // from the bar to the card, across
        switch (nearSide) {
            case 0: a = drop.y + drop.h - into; b = r.y + depth; break;
            case 1: a = drop.y + into; b = r.y + r.h - depth; break;
            case 2: a = drop.x + drop.w - into; b = r.x + depth; break;
            default: a = drop.x + into; b = r.x + r.w - depth; break;
        }
        f.waist = {static_cast<float>(c), static_cast<float>(a), static_cast<float>(b), static_cast<float>(n)};
        // (how far the card is still inside the bar: the bar's edge minus its near edge, away from the bar)
        switch (nearSide) {
            case 0: f.overlap = static_cast<float>(drop.y + drop.h - r.y); break;
            case 1: f.overlap = static_cast<float>(r.y + r.h - drop.y); break;
            case 2: f.overlap = static_cast<float>(drop.x + drop.w - r.x); break;
            default: f.overlap = static_cast<float>(r.x + r.w - drop.x); break;
        }
    }
    // the content: evenly scaled about the middle of its edge facing the bar (its
    // middle, in place), carried by the shape's same point
    f.scale  = static_cast<float>(std::max(mix(m_geo.contentFrom, 1.0, m_motion.value(ch::Scale, t)), 0.05));
    f.alpha  = static_cast<float>(std::clamp(m_motion.value(ch::Alpha, t), 0.0, 1.0));
    // (opening only: closing, a selected button's solid fill blurred into a soft
    // blob that read as a lens of its own, a glass pill, as the card shrank)
    f.blurPx = m_motion.shown() ? static_cast<float>(m_geo.blur * monitorScale * (1.0 - f.alpha)) : 0.0f;
    f.glass  = static_cast<float>(std::clamp(m_motion.value(ch::Glass, t), 0.0, 1.0));
    auto nearMid = [nearSide](const CBox& b) -> Vector2D {
        switch (nearSide) {
            case 1: return {b.x + b.w * 0.5, b.y + b.h};
            case 2: return {b.x, b.y + b.h * 0.5};
            case 3: return {b.x + b.w, b.y + b.h * 0.5};
            default: return {b.x + b.w * 0.5, b.y};
        }
    };
    f.contentAnchor = m_openFromBar ? nearMid(C) : C.middle();
    f.screenAnchor  = m_openFromBar ? nearMid(f.rect) : f.rect.middle();
    return f;
}

std::optional<CBox> CGlassLayerSurface::morphExtent(double t, float monitorScale, const CBox& quad) const {
    namespace ch = motion::tokens::ch;
    if (m_motionCard.w <= 0.0 || m_motionCard.h <= 0.0)
        return std::nullopt;
    CBox seed, drop;
    morphSeed(monitorScale, seed, drop);
    const int  nearSide = m_openFromBar ? m_nearSide : 0;
    const auto n = m_motion.range(ch::Near, t), fa = m_motion.range(ch::Far, t), si = m_motion.range(ch::Sides, t);
    // (edges are linear in their progress: the extremes are at the range's ends)
    CBox ext = boxOf(morphEdges(seed, m_motionCard, nearSide, n.first, fa.first, si.first));
    for (const auto& e : {morphEdges(seed, m_motionCard, nearSide, n.second, fa.second, si.second),
                          morphEdges(seed, m_motionCard, nearSide, n.first, fa.second, si.second),
                          morphEdges(seed, m_motionCard, nearSide, n.second, fa.first, si.first)}) {
        const CBox b = boxOf(e);
        const double x0 = std::min(ext.x, b.x), y0 = std::min(ext.y, b.y);
        ext = CBox{x0, y0, std::max(ext.x + ext.w, b.x + b.w) - x0, std::max(ext.y + ext.h, b.y + b.h) - y0};
    }
    {
        // (the drip: the card hangs past its place by up to the fall's range)
        const auto fr = m_motion.range(ch::Fall, t);
        const double lo = m_geo.fall * monitorScale * std::min(fr.first, 0.0), hi = m_geo.fall * monitorScale * std::max(fr.second, 0.0);
        const bool   vert = nearSide <= 1, neg = nearSide == 1 || nearSide == 3;
        const double a = neg ? -hi : lo, b = neg ? -lo : hi;
        if (vert) { ext.y += a; ext.h += b - a; } else { ext.x += a; ext.w += b - a; }
    }
    if (drop.w > 0.0) {
        // (the bar: only where the neck can reach, beside the card)
        const double reach = 3.0 * m_geo.neck * monitorScale;
        CBox         near  = drop;
        if (m_nearSide <= 1) {
            near.x = std::max(drop.x, ext.x - reach);
            near.w = std::min(drop.x + drop.w, ext.x + ext.w + reach) - near.x;
        } else {
            near.y = std::max(drop.y, ext.y - reach);
            near.h = std::min(drop.y + drop.h, ext.y + ext.h + reach) - near.y;
        }
        const double x0 = std::min(ext.x, near.x), y0 = std::min(ext.y, near.y);
        ext = CBox{x0, y0, std::max(ext.x + ext.w, near.x + near.w) - x0, std::max(ext.y + ext.h, near.y + near.h) - y0};
    }
    // its shadow, the neck's bulge and the content's blur
    ext.expand((SHADOW_REACH + m_geo.neck + m_geo.blur) * monitorScale + 4.0);
    ext = ext.intersection(CBox{0.0, 0.0, quad.w, quad.h});
    if (ext.w <= 0.0 || ext.h <= 0.0)
        return std::nullopt;
    return ext.round();
}

std::pair<float, float> CGlassLayerSurface::lensPx(PHLMONITOR monitor) const {
    const float           scale = monitor && monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;
    const SResolveContext ctx   = {resolvePresetName(), resolveThemeIsDark(), g_pGlobalState->config, g_pGlobalState->customPresets};
    return {resolvePresetFloat(ctx, &SPresetValues::thickness, &SOverridableConfig::thickness, 0.0f) * scale,
            resolvePresetFloat(ctx, &SPresetValues::bezelWidth, &SOverridableConfig::bezelWidth, 0.0f) * scale};
}

CGlassLayerSurface::SGrade CGlassLayerSurface::grade() const {
    SGrade                g;
    const SResolveContext ctx      = {resolvePresetName(), resolveThemeIsDark(), g_pGlobalState->config, g_pGlobalState->customPresets};
    const auto&           defaults = ctx.isDark ? DARK_THEME_DEFAULTS : LIGHT_THEME_DEFAULTS;
    g.g1 = {resolvePresetFloat(ctx, &SPresetValues::brightness, &SOverridableConfig::brightness, defaults.brightness),
            resolvePresetFloat(ctx, &SPresetValues::adaptiveDim, &SOverridableConfig::adaptiveDim, defaults.adaptiveDim),
            resolvePresetFloat(ctx, &SPresetValues::contrast, &SOverridableConfig::contrast, defaults.contrast),
            resolvePresetFloat(ctx, &SPresetValues::saturation, &SOverridableConfig::saturation, defaults.saturation)};
    const int64_t t = resolvePresetInt(ctx, &SPresetValues::tintColor, &SOverridableConfig::tintColor);
    g.tint = {static_cast<float>((t >> 24) & 0xFF) / 255.0f, static_cast<float>((t >> 16) & 0xFF) / 255.0f, static_cast<float>((t >> 8) & 0xFF) / 255.0f,
              static_cast<float>(t & 0xFF) / 255.0f};
    const auto ls = m_layerSurface.lock();
    if (ls && g_pGlobalState->layerNamespaceAdaptive.contains(ls->m_namespace) && m_styleStrip && m_styleStrip->isAllocated())
        g.styleTex = m_styleStrip->getTexture()->m_texID;
    return g;
}

// how far the drawn shape has handed over to the card's own outline (0 drawn, 1 settled;
// leaving, back from it at once: the join to the bar is the mix's other side)
double CGlassLayerSurface::handAt(double t, const CBox& rect) const {
    (void)t;
    // by shape, both ways: the card's own outline as soon as the drawn rect is near
    // the card's size (within ~8 %), the analytic box once it is far from it. At rest
    // and landing it is wholly the card's own: nothing to hand over as it settles.
    const CBox& C = m_motionCard;
    if (C.w <= 0.0 || C.h <= 0.0 || rect.w <= 0.0 || rect.h <= 0.0)
        return 0.0;
    // (only an outline measured from the whole card: one measured from a card still
    // fading in, a close clicked during its open, had only its opaque parts, and the
    // weather's AIR / PM2.5 / UV pills each became glass of their own. Without one,
    // the card's own rounded box, which is what these cards are)
    if (!m_fieldTrusted)
        return 0.0;
    const double d = std::max(std::abs(std::log(rect.w / C.w)), std::abs(std::log(rect.h / C.h)));
    return 1.0 - smooth01d(0.08, 0.3, d);
}

bool CGlassLayerSurface::mergeIntoBar(const PHLLS& bar, PHLMONITOR monitor, const CBox& barBox, GlassRenderer::SMaskInfo& mask) {
    if (!m_motionActive || !m_openFromBar || m_popups || !monitor || m_barLayer.lock() != bar)
        return false;
    const auto ls = m_layerSurface.lock();
    const auto box = glassBox(monitor);
    if (!ls || !box)
        return false;
    const float  scale = monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;
    const double t     = motionTime(nowSecs());
    m_motion.advance(t); // (the bar draws before the card: the same moment)
    const SMorphFrame f = morphFrame(t, scale);
    // (let go and apart: the bar is just the bar again; joined by the waist or
    // touching it, the bar draws the union, else its straight edge showed across
    // the card's top as a line)
    const double hand = handAt(t, f.rect);
    if (f.source.w <= 0.0 || (f.waist[3] <= 0.002f && -f.overlap > 4.5 * scale)) {
        return false;
    }
    // the card's quad px -> the bar's
    const Vector2D d = transformedLayerBox(*box, monitor).pos() - barBox.pos();
    const CBox     r = f.rect.copy().translate(d);
    mask.morphBarSide      = true;
    mask.morphRect         = {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.x + r.w), static_cast<float>(r.y + r.h)};
    mask.morphRadius       = f.radius;
    mask.morphNeck         = f.neck;
    // (the bar itself: its own glass box, which the caller set)
    mask.morphSource       = {static_cast<float>(mask.glassBoxOffsetPx.x), static_cast<float>(mask.glassBoxOffsetPx.y),
                              static_cast<float>(mask.glassBoxOffsetPx.x + mask.glassBoxSizePx.x), static_cast<float>(mask.glassBoxOffsetPx.y + mask.glassBoxSizePx.y)};
    mask.morphSourceRadius = 0.0f;
    const bool alongX      = f.waistAlongX;
    mask.morphWaist        = {f.waist[0] + static_cast<float>(alongX ? d.x : d.y), f.waist[1] + static_cast<float>(alongX ? d.y : d.x),
                              f.waist[2] + static_cast<float>(alongX ? d.y : d.x), f.waist[3]};
    mask.morphWaistHalf    = f.waistHalf;
    mask.morphWaistBar     = f.waistBarHalf;
    mask.morphWaistAlongX  = alongX;
    mask.morphWaistHold    = f.waistHold;
    mask.morphWaistSnap    = f.waistSnap;
    mask.morphOverlap      = f.overlap;
    mask.morphHand         = {static_cast<float>(hand), 0.0f};
    return true;
}

float CGlassLayerSurface::popupAlpha() const {
    const auto layerSurface = m_layerSurface.lock();
    if (!m_popups || !layerSurface || !layerSurface->m_popupHead)
        return 1.0f;

    float alpha = 0.0f;
    layerSurface->m_popupHead->breadthfirst(
        [&](SP<Desktop::View::CPopup> popup, void*) {
            if (popup && popup->m_mapped && popup->visible())
                alpha = std::max(alpha, popup->alpha().getTotal());
        },
        nullptr);
    // (a tooltip fades in, glass and text together, instead of appearing whole in
    // one frame: eased over 0.12 s from when it mapped)
    const auto now = std::chrono::steady_clock::now();
    if (const auto box = logicalBox()) {
        if (!m_popupBox) // (Omarchy unmaps its tooltip between items: each one maps fresh)
            m_popupFadeAt = now;
        m_popupBox = box;
    } else
        m_popupBox.reset();
    const float since = std::chrono::duration<float>(now - std::max(m_mapStart, m_popupFadeAt)).count();
    if (since < 0.12f) {
        const float x = std::clamp(since / 0.12f, 0.0f, 1.0f);
        alpha *= x * x * (3.0f - 2.0f * x);
        const_cast<CGlassLayerSurface*>(this)->damageSampleRegion();
    }
    return alpha;
}

bool CGlassLayerSurface::ownsSurface(const SP<CWLSurfaceResource>& surface) const {
    const auto layerSurface = m_layerSurface.lock();
    if (!m_popups || !layerSurface || !layerSurface->m_popupHead || !surface)
        return false;

    bool found = false;
    layerSurface->m_popupHead->breadthfirst(
        [&](SP<Desktop::View::CPopup> popup, void*) {
            if (found || !popup)
                return;
            const auto root = popup->resource();
            if (root && root->findFirstPreorder([&surface](SP<CWLSurfaceResource> c) { return c == surface; }))
                found = true;
        },
        nullptr);
    return found;
}

CGlassLayerSurface::~CGlassLayerSurface() {
    // (its timer calls back into this)
    if (m_owedTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_owedTimer);
    // Damage the area where glass was last drawn so the compositor
    // re-renders it without the glass effect (prevents ghost artifacts).
    if (g_pHyprRenderer && m_lastSize.x > 0 && m_lastSize.y > 0 &&
        std::isfinite(m_lastPosition.x) && std::isfinite(m_lastPosition.y) &&
        std::isfinite(m_lastSize.x) && std::isfinite(m_lastSize.y)) {
        auto box = CBox{m_lastPosition, m_lastSize};
        box.expand(GlassRenderer::SAMPLE_PADDING_PX).noNegativeSize();
        if (box.w > 0.0 && box.h > 0.0)
            g_pHyprRenderer->damageBox(box);
    }
}

bool CGlassLayerSurface::resolveThemeIsDark() const {
    try {
        const auto& config = g_pGlobalState->config;
        const auto theme = readStringConfig(config.defaultTheme);
        if (!theme.empty())
            return theme != "light";
    } catch (...) {}

    return true;
}

std::string CGlassLayerSurface::resolvePresetName() const {
    try {
        // Per-namespace preset override (highest priority)
        const auto layerSurface = m_layerSurface.lock();
        if (layerSurface && m_popups) {
            const auto& popupPresets = g_pGlobalState->layerNamespacePopupPresets;
            if (auto it = popupPresets.find(layerSurface->m_namespace); it != popupPresets.end())
                return it->second;
        }
        if (layerSurface) {
            const auto& nsPresets = g_pGlobalState->layerNamespacePresets;
            auto it = nsPresets.find(layerSurface->m_namespace);
            if (it != nsPresets.end())
                return it->second;
        }

        const auto& config = g_pGlobalState->config;

        // Layer-wide preset override
        const auto layerPreset = readStringConfig(config.layersPreset);
        if (!layerPreset.empty())
            return std::string(layerPreset);

        // Fall back to global default preset
        const auto defaultPreset = readStringConfig(config.defaultPreset);
        if (!defaultPreset.empty())
            return std::string(defaultPreset);
    } catch (...) {}

    return "default";
}

ELayerMaskMode CGlassLayerSurface::resolveMaskMode() const {
    if (const auto layerSurface = m_layerSurface.lock()) {
        const auto& overrides = g_pGlobalState->layerNamespaceMaskModes;
        if (auto it = overrides.find(layerSurface->m_namespace); it != overrides.end())
            return it->second;
    }

    if (auto mode = parseLayerMaskMode(readStringConfig(g_pGlobalState->config.layersMaskMode)))
        return *mode;
    return ELayerMaskMode::AUTO;
}

CGlassLayerSurface::EMaskSource CGlassLayerSurface::resolveMaskSource() const {
    const auto layerSurface = m_layerSurface.lock();
    if (m_popups)
        return layerSurface && g_pGlobalState->layerNamespacePopupRadius.contains(layerSurface->m_namespace) ? EMaskSource::SHAPE_BOX : EMaskSource::CONTOUR;
    if (layerSurface && g_pGlobalState->layerNamespaceShapes.contains(layerSurface->m_namespace))
        return EMaskSource::SHAPE_BOX;
    const auto wlSurface    = layerSurface ? layerSurface->wlSurface() : nullptr; // root surface only
    const bool hasEffect    = wlSurface && wlSurface->m_hasBackgroundEffect;
    const bool regionEmpty  = !wlSurface || wlSurface->m_blurRegion.empty();

    switch (resolveMaskMode()) {
        case ELayerMaskMode::ALPHA:
            return EMaskSource::ALPHA_THRESHOLD;
        case ELayerMaskMode::CONTOUR:
            return EMaskSource::CONTOUR;
        case ELayerMaskMode::REGION:
            return (hasEffect && !regionEmpty) ? EMaskSource::PROTOCOL_REGION : EMaskSource::NONE;
        case ELayerMaskMode::AUTO:
        default:
            if (hasEffect)
                return regionEmpty ? EMaskSource::NONE : EMaskSource::PROTOCOL_REGION;
            return EMaskSource::ALPHA_THRESHOLD;
    }
}

bool CGlassLayerSurface::liveResampleEnabled() const {
    if (const auto layerSurface = m_layerSurface.lock()) {
        const auto& overrides = g_pGlobalState->layerNamespaceLiveResample;
        if (auto it = overrides.find(layerSurface->m_namespace); it != overrides.end())
            return it->second;
    }

    const auto& config = g_pGlobalState->config;
    return config.layersLiveResample && **config.layersLiveResample;
}

PHLLS CGlassLayerSurface::getLayerSurface() const {
    return m_layerSurface.lock();
}

CRegion CGlassLayerSurface::transformedBlurRegion(PHLMONITOR monitor, const CBox& rawBox) const {
    const auto layerSurface = m_layerSurface.lock();
    const auto wlSurface    = layerSurface ? layerSurface->wlSurface() : nullptr;
    if (!layerSurface || !wlSurface || !monitor)
        return {};

    const auto logicalSize = layerSurface->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    // surface-local logical region -> box-local pixels, same
    // scale/translate/transform sequence as transformedLayerBox()
    CRegion region = wlSurface->m_blurRegion.copy();
    region.intersect(0, 0, logicalSize.x, logicalSize.y); // spec: clipped to surface size
    region.scale(static_cast<float>(monitor->m_scale));
    region.translate(rawBox.pos());
    region.intersect(rawBox.x, rawBox.y, rawBox.w, rawBox.h); // defensive
    region.transform(Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform)),
                      monitor->m_transformedSize.x, monitor->m_transformedSize.y);
    return region;
}

std::optional<CBox> CGlassLayerSurface::regionBoundingBoxAbsolute(PHLMONITOR monitor, const CBox& layerBox) const {
    if (!monitor)
        return std::nullopt;

    auto region = transformedBlurRegion(monitor, layerBox);
    if (region.empty())
        return std::nullopt;

    return region.getExtents();
}

std::optional<CBox> CGlassLayerSurface::regionBoundingBoxGlobal() const {
    const auto layerSurface = m_layerSurface.lock();
    const auto wlSurface    = layerSurface ? layerSurface->wlSurface() : nullptr;
    if (!layerSurface || !wlSurface)
        return std::nullopt;

    const auto position    = layerSurface->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    const auto logicalSize = layerSurface->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    // Stays in the layer's own global-logical family (no monitor scale/transform
    // applied): BackgroundDamageObserver's damagedBox is global-logical too.
    CRegion region = wlSurface->m_blurRegion.copy();
    region.intersect(0, 0, logicalSize.x, logicalSize.y);
    region.translate(position);

    if (region.empty())
        return std::nullopt;

    return region.getExtents();
}

void CGlassLayerSurface::damageIfMoved() {
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    const auto currentBox = logicalBox();
    if (!currentBox)
        return;
    const auto currentPosition = currentBox->pos();
    const auto currentSize     = currentBox->size();
    if (currentSize.x <= 0.0 || currentSize.y <= 0.0 ||
        !std::isfinite(currentPosition.x) || !std::isfinite(currentPosition.y) ||
        !std::isfinite(currentSize.x) || !std::isfinite(currentSize.y))
        return;

    const bool isAnimating = layerSurface->positionAnimation()->isBeingAnimated() ||
                             layerSurface->sizeAnimation()->isBeingAnimated() ||
                             layerSurface->alpha()[Desktop::View::LS_ALPHA_FADE]->isBeingAnimated() ||
                             !layerSurface->m_mapped;

    const bool moved = currentPosition != m_lastPosition || currentSize != m_lastSize;

    // keep frames coming for the open animation (the close follows the client's
    // own commits), and for one more frame while a content measurement is in flight
    // (only a contour composite reads it back: under another mask mode, after a
    // config change, a pending one would keep frames coming forever)
    if (m_openLive || (m_probe.pending() && resolveMaskSource() == EMaskSource::CONTOUR))
        damageSampleRegion();
    // a throttled background change still owed
    if (m_trailingDirty)
        markBackgroundDirty();

    if (moved || isAnimating) {
        m_lastPosition  = currentPosition;
        m_lastSize      = currentSize;

        damageSampleRegion();

        if (const auto monitor = layerSurface->m_monitor.lock())
            g_pGlobalState->bumpSceneGeneration(monitor);
    } else if (const auto& config = g_pGlobalState->config;
               config.layersForceLiveResample && **config.layersForceLiveResample) {
        // keep frames flowing so the forced per-frame resample actually runs
        damageSampleRegion();
    }
}

void CGlassLayerSurface::damageSampleRegion() {
    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    const auto monitor = layerSurface->m_monitor.lock();
    const float scale = monitor ? monitor->m_scale : 1.0f;
    const auto logical = logicalBox();
    if (!logical)
        return;
    auto box = *logical;
    box.expand(GlassRenderer::SAMPLE_PADDING_PX / scale).noNegativeSize();
    if (box.w > 0.0 && box.h > 0.0 &&
        std::isfinite(box.x) && std::isfinite(box.y) && std::isfinite(box.w) && std::isfinite(box.h))
        g_pHyprRenderer->damageBox(box);
}

// Too soon for another resample: owed, not dropped (the change might be the last
// one, the end of a wallpaper transition). Paid by one timer when its turn comes;
// damaging now instead drew a frame that asked again, still too soon, and damaged
// again: the whole pane redrawn every frame while it waited.
void CGlassLayerSurface::oweResample(std::chrono::steady_clock::duration wait) {
    m_trailingDirty = true;
    if (m_owedTimer && m_owedTimer->armed())
        return;
    if (!m_owedTimer) {
        m_owedTimer = makeShared<CEventLoopTimer>(std::nullopt, [this](SP<CEventLoopTimer>, void*) {
            if (m_trailingDirty)
                markBackgroundDirty();
        }, nullptr);
        g_pEventLoopManager->addTimer(m_owedTimer);
    }
    m_owedTimer->updateTimeout(std::chrono::duration_cast<Time::steady_dur>(wait) + std::chrono::milliseconds(1));
}

void CGlassLayerSurface::markBackgroundDirty() {
    if (m_backgroundDirty) {
        m_trailingDirty = false; // the resample already pending covers it
        return;
    }

    const auto& config = g_pGlobalState->config;
    int64_t fps = config.layersLiveResampleFps ? **config.layersLiveResampleFps : 0;
    // frosted, what is behind shows as a soft blur: in Low power, 10 updates a second
    // follow it (a card over a scrolling terminal resampled with every line). Not
    // otherwise: a video behind a frosted menu stepped at 10 fps, landing unevenly
    // on a 165 Hz screen, and read as choppy.
    const bool lowPower = config.lowPower && **config.lowPower > 0.5f;
    if (lowPower && m_frost.amount() > 0.6f)
        fps = fps > 0 ? std::min<int64_t>(fps, 10) : 10;
    const auto now = std::chrono::steady_clock::now();
    if (fps > 0 && now - m_lastDirtyMark < std::chrono::nanoseconds(1'000'000'000 / fps)) {
        oweResample(m_lastDirtyMark + std::chrono::nanoseconds(1'000'000'000 / fps) - now);
        return;
    }
    m_lastDirtyMark = now;
    m_trailingDirty = false;

    m_backgroundDirty = true;
    // damage the full sample region: outside the committed area the framebuffer
    // still holds our previous glass output, which must not be re-sampled
    damageSampleRegion();
}

void CGlassLayerSurface::buildStyleStrip(PHLMONITOR monitor, const CBox& transformBox, const SP<Render::IFramebuffer>& callerFramebuffer) {
    const auto sample = m_sampleFramebuffer ? m_sampleFramebuffer->getTexture() : nullptr;
    const bool across = transformBox.w >= transformBox.h;
    if (!sample ||
        !ShapeField::buildStyleStrip(sample->m_texID, m_samplePaddingRatio, across, static_cast<int>(across ? transformBox.w : transformBox.h),
                                     192.0f * (monitor ? monitor->m_scale : 1.0f), m_styleStrip, callerFramebuffer))
        m_styleStrip.reset();
}

void CGlassLayerSurface::sampleAndRedirect(PHLMONITOR monitor, float alpha) {
    auto& shaderManager = g_pGlobalState->shaderManager;
    shaderManager.initializeIfNeeded();

    if (!shaderManager.isInitialized())
        return;

    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    auto source = g_pHyprRenderer->m_renderData.currentFB;
    if (!source)
        return;

    auto layerBox = glassBox(monitor);
    if (!layerBox)
        return;

    CBox transformBox = transformedLayerBox(*layerBox, monitor);

    // PROTOCOL_REGION: glass only ever shows inside the blur region, so blurred
    // data outside it is provably never sampled. m_cachedTransformedRegion is
    // computed once here and reused by compositeAndRestore()'s regionRects
    // upload later this same frame — never widens boundingBox() or the
    // composite draw, which stay full-layer (see their own resolveMaskSource()
    // call sites elsewhere in this file). m_cachedRegionSampleBox is likewise
    // reused by compositeAndRestore() to build the sampleUVOffset/uvScale
    // values that reconcile the (region-shrunk) sample box against the
    // full-layer quad UV — see sampleXform in Shaders.hpp.
    const auto maskSourceNow = resolveMaskSource();
    const bool isRegionMode  = maskSourceNow == EMaskSource::PROTOCOL_REGION;
    m_cachedTransformedRegion = isRegionMode ? transformedBlurRegion(monitor, *layerBox) : CRegion{};
    std::optional<CBox> regionSampleBox = isRegionMode ? regionBoundingBoxAbsolute(monitor, *layerBox) : std::nullopt;
    // a contour layer only needs what is behind its content (a full-screen panel
    // layer holding one card samples just the card's area)
    const bool isContourBox = maskSourceNow == EMaskSource::CONTOUR && m_contentLocal && m_contentLocal->w > 0;
    if (isContourBox) {
        // where the content is now: a slide or popin moves the layer without a commit
        CBox local = *m_contentLocal;
        if (m_motionCard.w > 0.0 && m_motionCard.h > 0.0) {
            // (and the card the motion runs to or from, with its shadow: a motion
            // starting this frame draws it before the next sample)
            CBox c = m_motionCard.copy().expand(SHADOW_REACH * (monitor ? monitor->m_scale : 1.0f) + 4.0);
            const double x0 = std::min(local.x, c.x), y0 = std::min(local.y, c.y);
            local = CBox{x0, y0, std::max(local.x + local.w, c.x + c.w) - x0, std::max(local.y + local.h, c.y + c.h) - y0};
        }
        if (m_motionActive)
            if (auto ext = morphExtent(motionTime(nowSecs()), monitor && monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f, transformBox)) {
                const double x0 = std::min(local.x, ext->x), y0 = std::min(local.y, ext->y);
                local = CBox{x0, y0, std::max(local.x + local.w, ext->x + ext->w) - x0, std::max(local.y + local.h, ext->y + ext->h) - y0};
            }
        const CBox b = local.translate(transformBox.pos()).intersection(transformBox);
        if (b.w > 0 && b.h > 0)
            regionSampleBox = b;
    }
    // A region resize/move is otherwise invisible to backgroundChanged below (a
    // layer's own commit doesn't mark itself dirty) — without this the mask
    // shape would update immediately while the sample stays from the old box.
    // (m_cachedRegionSampleBox is the box of the cached texture: it changes only
    // when a resample actually runs, below)
    const bool regionBoxChanged = (isRegionMode || isContourBox) && regionSampleBox != m_cachedRegionSampleBox;

    // Brackets everything below, including the conditional resample and the
    // temp-FBO redirect/clear that always runs: GL forbids a concurrent
    // GL_TIME_ELAPSED query, so on a cache miss the SampleBackground/
    // BlurBackground brackets those calls open underneath this one just no-op.
    Diagnostics::CScopedStageTimer stageTimer(Diagnostics::EStage::LayerSample);
    const MONITORID monitorId = monitor ? monitor->m_id : -1; // -1 mirrors Hyprland's own MONITOR_INVALID

    // Decide whether we need to re-sample and re-blur the background.
    // When only the layer surface content changed (e.g. waybar clock tick)
    // but no window moved behind us, we reuse the cached blurred background.
    // This skips the most expensive GPU work (blit + 6 blur passes).
    const uint64_t currentGeneration = g_pGlobalState->getSceneGeneration(monitor);
    // A workspace slide or fade moves the whole scene behind us without changing
    // any window's own geometry, so no window decoration update reports it.
    const bool isAnimating = layerSurface->positionAnimation()->isBeingAnimated() ||
                             layerSurface->sizeAnimation()->isBeingAnimated() ||
                             layerSurface->alpha()[Desktop::View::LS_ALPHA_FADE]->isBeingAnimated() ||
                             WorkspaceAnimation::anyWorkspaceAnimating(monitor);
    const auto& config = g_pGlobalState->config;
    const bool forceLive = config.layersForceLiveResample && **config.layersForceLiveResample;
    const bool backgroundChanged = !m_hasCachedSample ||
                                   currentGeneration != m_lastSceneGeneration ||
                                   isAnimating || m_backgroundDirty || forceLive || regionBoxChanged;

    const CBox sampleBox     = regionSampleBox.value_or(transformBox);
    const bool sampleCovered = !backgroundChanged ||
                               GlassRenderer::sampleRegionCovered(sampleBox, source, g_pHyprRenderer->m_renderData.damage);

    if (!layerSurface->m_mapped) {
        // During fade-out, re-sampling captures stale pixels. Reuse cached sample.
        if (!m_hasCachedSample)
            return;
        Diagnostics::recordLayerCacheHit(monitorId);
    } else if (!sampleCovered) {
        // the work buffer is cleared outside this frame's damage: sampling now would
        // cache black. Without a cache, no redirect: compositeAndRestore() bails.
        m_backgroundDirty = true;
        damageSampleRegion();
        Diagnostics::recordLayerDeferredResample(monitorId);
        if (!m_hasCachedSample)
            return;
    } else if (backgroundChanged) {
        Diagnostics::recordLayerCacheMiss(monitorId);

        const bool isDark          = resolveThemeIsDark();
        const std::string preset   = resolvePresetName();
        const SResolveContext ctx  = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

        float blurStrength   = resolvePresetFloat(ctx, &SPresetValues::blurStrength, &SOverridableConfig::blurStrength);
        int downscale        = GlassRenderer::sampleDownscale(blurStrength);

        GlassRenderer::sampleBackground(m_sampleFramebuffer, source, regionSampleBox.value_or(transformBox), m_samplePaddingRatio, downscale);
        m_cachedRegionSampleBox = regionSampleBox;

        float blurRadius     = blurStrength * 12.0f / downscale;
        int blurIterations   = std::clamp(static_cast<int>(resolvePresetInt(ctx, &SPresetValues::blurIterations, &SOverridableConfig::blurIterations)), 1, 5);

        if (ctx.config.blurFold && **ctx.config.blurFold) {
            const GlassRenderer::SFoldedBlur folded = GlassRenderer::foldBlurPasses(blurRadius, blurIterations);
            blurRadius     = folded.radius;
            blurIterations = folded.iterations;
        }

        GlassRenderer::blurBackground(m_sampleFramebuffer, blurRadius, blurIterations, source);
        m_frost.sampleChanged();

        // an adaptive bar's brightness along its length, once per resample
        if (maskSourceNow == EMaskSource::SHAPE_BOX && g_pGlobalState->layerNamespaceAdaptive.contains(layerSurface->m_namespace))
            buildStyleStrip(monitor, transformBox, source);
        else
            m_styleStrip.reset(); // it would describe an older backdrop

        m_hasCachedSample      = true;
        m_lastSceneGeneration  = currentGeneration;
        m_backgroundDirty      = false;
    } else {
        // background unchanged, reuse cached blur — skip 7 GPU operations
        Diagnostics::recordLayerCacheHit(monitorId);
    }

    // Redirect surface rendering to a temp FBO cleared to transparent.
    // The original renderLayer (called between pre/post elements) will render
    // the surface into this FBO. compositeAndRestore uses its alpha as a mask.
    // Size from the source FB, not the monitor: m_transformedSize is swapped
    // relative to the framebuffer's native orientation on 90°/270° monitors (#41).
    int monitorWidth  = static_cast<int>(source->m_size.x);
    int monitorHeight = static_cast<int>(source->m_size.y);

    // In FP16/HDR mode, the source FB uses RGBA16F which has full alpha precision.
    // Use the source format to avoid clipping HDR color values.
    // In SDR mode, force ARGB8888 because monitor FBOs (XRGB2101010 etc.) have
    // limited/no alpha, which would quantize mask values and break the discard.
    DRMFormat tempFormat = (monitor->useFP16()) ? source->m_drmFormat : DRM_FORMAT_ARGB8888;

    if (!m_surfaceTempFramebuffer)
        m_surfaceTempFramebuffer = g_pHyprRenderer->createFB("hyprglass-layer-temp");

    if (m_surfaceTempFramebuffer->m_size.x != monitorWidth || m_surfaceTempFramebuffer->m_size.y != monitorHeight ||
        m_surfaceTempFramebuffer->m_drmFormat != tempFormat)
        m_surfaceTempFramebuffer->alloc(monitorWidth, monitorHeight, tempFormat);

    m_savedCurrentFB = source;

    g_pHyprRenderer->m_renderData.currentFB = m_surfaceTempFramebuffer;
    glBindFramebuffer(GL_FRAMEBUFFER, dynamic_cast<Render::GL::CGLFramebuffer*>(m_surfaceTempFramebuffer.get())->getFBID());

    // Unpadded: the composite quad only ever reads transformBox (rawBox, never
    // padded), so the SAMPLE_PADDING_PX-expanded part of a padded clear is never
    // sampled — true regardless of mask mode, not the region-sized clear a
    // literal reading of "region-aware" would suggest (that would under-clear
    // real surface area outside the region that the composite quad does read).
    CBox clearBox = transformBox.intersection(CBox{0.0, 0.0, static_cast<double>(monitorWidth), static_cast<double>(monitorHeight)}).noNegativeSize().round();

    if (std::isfinite(clearBox.x) && std::isfinite(clearBox.y) && std::isfinite(clearBox.w) && std::isfinite(clearBox.h) &&
        clearBox.w > 0.0 && clearBox.h > 0.0) {
        g_pHyprOpenGL->scissor(clearBox, false);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        g_pHyprOpenGL->scissor(nullptr);
    }

    m_redirectedThisFrame = true;
}

void CGlassLayerSurface::compositeAndRestore(PHLMONITOR monitor, float alpha, EMaskSource maskSource) {
    // Restore the original currentFB before compositing
    if (m_savedCurrentFB) {
        g_pHyprRenderer->m_renderData.currentFB = m_savedCurrentFB;
        glBindFramebuffer(GL_FRAMEBUFFER, dynamic_cast<Render::GL::CGLFramebuffer*>(m_savedCurrentFB.get())->getFBID());
        m_savedCurrentFB.reset();
    }

    // The render pass can discard the pre-surface element without discarding
    // this one (its bounding box is evaluated first, against a superset of the
    // damage the pre-surface element sees — see disableSimplification() in
    // GlassLayerPassElement.cpp). Without this flag we'd mask/composite against
    // whatever m_surfaceTempFramebuffer held from an earlier frame.
    if (!m_redirectedThisFrame)
        return;
    m_redirectedThisFrame = false;

    auto& shaderManager = g_pGlobalState->shaderManager;
    if (!shaderManager.isInitialized() || !m_hasCachedSample)
        return;

    const auto layerSurface = m_layerSurface.lock();
    if (!layerSurface)
        return;

    auto target = g_pHyprRenderer->m_renderData.currentFB;
    if (!target)
        return;

    auto layerBox = glassBox(monitor);
    if (!layerBox)
        return;

    // Brackets mask setup + the applyGlassEffect call below; that call's own
    // ApplyGlassEffect bracket sees this one already open and no-ops instead
    // of nesting (GL forbids concurrent GL_TIME_ELAPSED queries).
    Diagnostics::CScopedStageTimer stageTimer(Diagnostics::EStage::LayerComposite);
    if (monitor)
        Diagnostics::recordLayerGlassDraw(monitor->m_id);

    CBox rawBox       = *layerBox;
    CBox transformBox = transformedLayerBox(rawBox, monitor);
    m_quadPos         = transformBox.pos();
    if (!m_ghostLive)
        m_lastLogical = logicalBox();

    const bool isDark          = resolveThemeIsDark();
    const std::string preset   = resolvePresetName();
    const SResolveContext ctx  = {preset, isDark, g_pGlobalState->config, g_pGlobalState->customPresets};

    float cornerRadius  = 0.0f;
    float roundingPower = 2.0f;

    // Use the temp FBO's rendered alpha as a mask: glass only where the surface
    // has visible content (alpha > 0). The temp FBO is in monitor coordinates,
    // so we map from the glass quad UV to monitor UV.
    int monitorWidth  = static_cast<int>(m_surfaceTempFramebuffer->m_size.x);
    int monitorHeight = static_cast<int>(m_surfaceTempFramebuffer->m_size.y);

    GlassRenderer::SMaskInfo maskInfo{
        .textureId = m_surfaceTempFramebuffer->getTexture()->m_texID,
        .target    = GL_TEXTURE_2D,
        .uvOffset  = {transformBox.x / monitorWidth, transformBox.y / monitorHeight},
        .uvScale   = {transformBox.w / monitorWidth, transformBox.h / monitorHeight},
    };

    switch (maskSource) {
        case EMaskSource::ALPHA_THRESHOLD: {
            float maskThreshold = 0.001f;
            auto threshIt = g_pGlobalState->layerNamespaceMaskThresholds.find(layerSurface->m_namespace);
            if (threshIt != g_pGlobalState->layerNamespaceMaskThresholds.end())
                maskThreshold = threshIt->second;

            // The temp FBO stores the layer after Hyprland applies fade alpha. Keep
            // mask_threshold relative to the layer's content alpha, otherwise fade-out
            // makes the mask fall below threshold early and the glass blinks off.
            maskInfo.maskMode       = 0;
            maskInfo.alphaThreshold = maskThreshold * std::clamp(alpha, 0.0f, 1.0f);
            break;
        }
        case EMaskSource::PROTOCOL_REGION: {
            maskInfo.maskMode       = 1;
            maskInfo.alphaThreshold = 0.0f; // unused in region mode

            // Reuses sampleAndRedirect()'s computation from this same frame
            // (guarded by m_redirectedThisFrame above, so it did run); recomputes
            // as a defensive fallback only if that's somehow empty here.
            if (m_cachedTransformedRegion.empty())
                m_cachedTransformedRegion = transformedBlurRegion(monitor, rawBox);
            CRegion& region = m_cachedTransformedRegion;

            const auto rects = region.getRects();
            if (rects.size() <= static_cast<size_t>(GlassRenderer::MAX_REGION_RECTS)) {
                maskInfo.regionRectCount = static_cast<int>(rects.size());
                for (size_t i = 0; i < rects.size(); i++) {
                    const auto& r = rects[i];
                    maskInfo.regionRects[i] = {static_cast<float>(r.x1 - transformBox.x), static_cast<float>(r.y1 - transformBox.y),
                                                static_cast<float>(r.x2 - r.x1), static_cast<float>(r.y2 - r.y1)};
                }
            } else {
                // Overflow: one bounding rect rather than dropping rects (a hole
                // reads as more broken than a few extra glassed pixels at concave corners).
                const auto extents = region.getExtents();
                maskInfo.regionRectCount = 1;
                maskInfo.regionRects[0]  = {static_cast<float>(extents.x - transformBox.x), static_cast<float>(extents.y - transformBox.y),
                                             static_cast<float>(extents.w), static_cast<float>(extents.h)};
            }

            // sampleAndRedirect() gave sampleBackground() the region's bounding
            // box, smaller than the full-layer transformBox this quad draws —
            // reconcile the quad's own UV into that smaller sample texture's
            // normalized space before its padding applies (sampleXform in
            // Shaders.hpp). Same defensive-recompute fallback as
            // m_cachedTransformedRegion above, guarded against a degenerate
            // (zero-size) transformBox since it is the UV remap's denominator.
            if (transformBox.w > 0.0 && transformBox.h > 0.0) {
                const CBox sampleBox = m_cachedRegionSampleBox.value_or(regionBoundingBoxAbsolute(monitor, rawBox).value_or(transformBox));
                maskInfo.sampleUVOffset = Vector2D((sampleBox.x - transformBox.x) / transformBox.w,
                                                    (sampleBox.y - transformBox.y) / transformBox.h);
                maskInfo.sampleUVScale  = Vector2D(sampleBox.w / transformBox.w,
                                                    sampleBox.h / transformBox.h);
            }
            break;
        }
        case EMaskSource::CONTOUR: {
            float maskThreshold = 0.02f;
            if (auto t = g_pGlobalState->layerNamespaceMaskThresholds.find(layerSurface->m_namespace);
                t != g_pGlobalState->layerNamespaceMaskThresholds.end())
                maskThreshold = t->second;
            const float fade = std::clamp(alpha, 0.0f, 1.0f);
            maskInfo.maskMode       = 0;
            maskInfo.alphaThreshold = maskThreshold * fade;

            // the rim reaches as far in as the bezel; deeper texels are flat glass
            const float scale  = monitor && monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;
            const float bezelW = resolvePresetFloat(ctx, &SPresetValues::bezelWidth, &SOverridableConfig::bezelWidth, 0.0f);
            const float edgeT  = resolvePresetFloat(ctx, &SPresetValues::edgeThickness, &SOverridableConfig::edgeThickness);
            const float reach  = std::max(bezelW > 0.0f ? bezelW * scale : edgeT * static_cast<float>(std::min(transformBox.w, transformBox.h)),
                                          SHADOW_REACH * scale) + 4.0f;

            // Where the content is and how visible it is: a tiny pass read back
            // asynchronously, requested only when the glassed surface committed
            // (or the box changed size), applied when the GPU has it.
            // The work buffer holds the surface only where this frame is damaged:
            // measure and build the field only from a frame that drew all of it.
            const bool wholeFrame = GlassRenderer::sampleRegionCovered(transformBox, target, g_pHyprRenderer->m_renderData.damage);
            // An open card whose content changed (typing in a search, new results) is
            // measured in this same frame: read back a frame or two later, the glass
            // kept the old outline that long, a frame of results without glass or of
            // glass without results. (Up to four in 150 ms, as the menu draws its new
            // text and then its new size: a list scrolled every frame is measured as
            // before. Opening and closing stay asynchronous.)
            {
                const auto nowS = std::chrono::steady_clock::now();
                if (m_contentDirty && wholeFrame && !m_probe.pending() && m_contentLocal && !m_motionActive && !m_closing && !m_ghostLive &&
                    !m_openLive && (nowS - m_syncMeasureAt > std::chrono::milliseconds(150) || m_syncBurst < 4)) {
                    m_syncBurst = nowS - m_syncMeasureAt > std::chrono::milliseconds(150) ? 1 : m_syncBurst + 1;
                    m_lastFieldBoxSize = {transformBox.w, transformBox.h};
                    m_probe.request(maskInfo.textureId, maskInfo.uvOffset, maskInfo.uvScale, static_cast<int>(transformBox.w),
                                    static_cast<int>(transformBox.h), 0.004f, target);
                    m_probe.finish(8);
                    m_contentDirty  = false;
                    if (m_syncBurst == 1)
                        m_syncMeasureAt = nowS;
                }
            }
            if (auto measured = wholeFrame ? m_probe.poll() : std::nullopt) {
                const auto& drawn = *measured;
                const float presence = drawn ? drawn->presence : 0.0f;
                // Drawn over the whole screen (a theme's dim behind its menu, where
                // Glass couldn't turn the dim off): no card. Made one, the whole screen
                // turned to glass and the menu vanished in it. From now until the layer
                // maps again Hyprland draws it as it would without Glass.
                if (drawn && !m_popups && monitor && transformBox.w * transformBox.h >= 0.8 * monitor->m_pixelSize.x * monitor->m_pixelSize.y &&
                    drawn->box.w >= 0.97 * transformBox.w && drawn->box.h >= 0.97 * transformBox.h) {
                    m_plainCover   = true;
                    m_motionActive = false;
                    m_openLive     = false;
                    m_heldValid    = false;
                    forgetClose();
                    maskInfo.contentBox = CBox{0.0, 0.0, 0.0, 1.0}; // (this frame: the surface as it is)
                    damageSampleRegion();
                    break;
                }
                // A settled, held card that collapses in one step is about to close:
                // the audio panel empties its device lists first (a 512 px card became
                // 218 px for five frames, then faded and closed from there). It closes
                // from the card as it was, held; a collapse that isn't followed by a
                // fade within COLLAPSE_WAIT was a real resize: the card comes back as
                // it is laid out now.
                // (cancelled: the card comes back to its new layout, read below even
                // though the motion is still turning)
                bool collapseCancelled = false;
                {
                    const auto nowC = std::chrono::steady_clock::now();
                    if (!m_closing)
                        m_collapseAt.reset();
                    else if (m_collapseAt && presence >= m_collapsePresence - 0.02f && nowC - *m_collapseAt > COLLAPSE_WAIT) {
                        releaseCollapseHold();
                        collapseCancelled  = true;
                    } else if (m_collapseAt && m_hideDeferredUntil && presence < m_collapsePresence - 0.05f && monitor) {
                        // (it fades: the close runs from the card as it was)
                        collapseFades(motionTime(nowSecs()), scale);
                    }
                    // (only things going away, nothing new in their place, and not while the
                    // user types: a submenu draws new rows, a search narrowing its results
                    // follows a key, and either froze or re-opened as a collapse)
                    if (drawn && !m_closing && m_heldValid && !m_motionActive && !m_openLive && m_drawnLocal.w > 0 && m_contentLocal &&
                        !drawn->fresh && !g_pGlobalState->contentInputRecent() && !g_pGlobalState->hoverRecent() && !collapseCancelled && nowC >= m_collapseQuietUntil &&
                        !g_pGlobalState->layerNamespaceNoHeldClose.contains(layerSurface->m_namespace) &&
                        m_heldRect == *m_contentLocal && presence > 0.5f && drawn->box.w * drawn->box.h < 0.7 * m_drawnLocal.w * m_drawnLocal.h) {
                        collapseSeen(presence);
                    }
                }
                CBox local{0.0, 0.0, 0.0, 0.0};
                if (drawn) {
                    // room for the drop shadow and the rim around what is drawn
                    local = drawn->box;
                    if (!m_closing)
                        m_drawnLocal = drawn->box; // the card itself (kept while closing, like its outline)
                    if (!m_closing && (!m_motionActive || m_motion.shown() || collapseCancelled) && !m_cardExactValid)
                        m_motionCard = drawn->box; // (opening or open: the card the motion runs to, until its exact edges are read)
                    local.expand(SHADOW_REACH * scale + 4.0);
                    local = local.intersection(CBox{0.0, 0.0, transformBox.w, transformBox.h}).round();
                }

                // A card coming in (from nothing) opens; one going out (its fade,
                // drawn by the client) closes back toward where it opened.
                if (m_presence < 0.05f && presence >= 0.05f && monitor) {
                    startOpen(monitor, transformBox, local);
                    m_probe.keepReference(); // (this card's own cells: the last one's mean nothing to it)
                }
                // Closing is a fade: visibility dropping twice in a row. One drop can be
                // a content change (the most opaque icon gone), which must not freeze
                // the outline or shrink the card.
                // Once closing it stays closing until visibility rises again (a lagging
                // measurement that shows no new drop must not snap the glass back).
                // (and what stays dims with it: a fade dims every cell; the brightest row
                // going away, a search's results changing, lowers the strongest cell alone)
                // (coming in, the strongest cell only rises: one well below its peak is the
                // card going, even while its layout still settles and cells still appear)
                const bool openFall = m_openLive && !m_closing && presence < 0.75f * m_openPeak;
                const bool dropped  = presence < m_presence - 0.01f && presence < 0.98f && (!drawn || drawn->fadeLeft < 0.99f || openFall);
                // (a drop to nothing is a close at once: the shell sometimes hides the
                // content without a fade, and waiting for a second drop left the glass
                // lingering alone on the held outline)
                // the frame the fade began in (the close runs from there: the CPU
                // sees it two measurements later, and the held card sat still meanwhile)
                if (dropped && !m_lastDropped)
                    m_firstDropAt = m_probe.resultFrameAt();
                // (and only a clear rise is a reopen: the shell's fade is never perfectly
                // monotonic when the card itself changes, e.g. the audio panel's level
                // meter, and a 1 % tick turned a close back into a full card)
                const bool reopened = presence > m_closeLow + 0.2f;
                // (with the settled card held, one clear drop is the close: waiting for a
                // second one left the held copy and the fading card swapping on screen
                // for ~100 ms on a real monitor)
                // (or every cell a tenth dimmer than at its brightest: nothing but a fade
                // does that, settled or still coming in)
                const bool clearDrop = (m_heldValid && presence < 0.9f) || (drawn && drawn->fadeLeft < 0.9f) || openFall; // (presence is the content's visibility, 0..1)
                const bool fading = dropped && (m_lastDropped || presence < 0.05f || clearDrop);
                if (!m_closing && fading)
                    closeSeen(presence);
                else if (m_closing && reopened && !fading)
                    reopenSeen();
                if (m_closing)
                    m_closeLow = std::min(m_closeLow, presence);
                // (a measurement of the same frame again, at 165 Hz more often than the
                // client draws, is no rise: it kept resetting the run of drops)
                if (dropped || presence > m_presence + 0.01f)
                    m_lastDropped = dropped;
                // the fade's pace, to carry the glass smoothly between measurements
                // (they land every one to three frames)
                const auto now = std::chrono::steady_clock::now();
                const float dt = std::chrono::duration<float>(now - m_presenceAt).count();
                m_presenceRate = m_closing && dt > 0.008f && dt < 0.25f ? std::clamp((presence - m_presence) / dt, -20.0f, 0.0f) : 0.0f;
                m_presenceAt   = now;
                m_presence     = presence;
                if (m_openLive && !m_closing)
                    m_openPeak = std::max(m_openPeak, presence);
                // what "fully visible" looks like for this card (frozen once a fade starts)
                // (and its cells, for the cell-by-cell fade: from a frame with nothing
                // fading, or one that has looked the same for FADE_SETTLE, a change)
                const auto nowF     = std::chrono::steady_clock::now();
                const bool partFade = drawn && drawn->fadeLeft < 0.97f;
                if (!partFade)
                    m_partFadeSince.reset();
                else if (!m_partFadeSince)
                    m_partFadeSince = nowF;
                const bool settledChange = m_partFadeSince && nowF - *m_partFadeSince > FADE_SETTLE;
                if (drawn && !m_closing && !dropped && !m_openLive) {
                    m_presenceRef = drawn->coverage;
                    m_peakRef     = drawn->presence;
                    m_refValid    = true;
                    if (!partFade || settledChange) {
                        m_probe.keepReference();
                        m_partFadeSince.reset();
                    }
                } else if (drawn && !m_closing && !dropped)
                    m_probe.keepBrightest(); // (coming in)

                // The field: over the content's area only, rebuilt on every change
                // except while the card closes: its outline holds and the glass
                // dissolves on it (rebuilt from a fading frame, the faint fill drops
                // under the threshold first and every glyph became its own lens).
                // (nor from any frame of a settled card that is already fading: a card
                // whose own fill is nearly clear, the weather, kept only its opaque pills
                // above the threshold, and the close drew them as separate glass pieces)
                const bool fadingFrame = m_heldValid && m_refValid && !m_openLive && !m_motionActive &&
                                         (presence < 0.97f * m_peakRef || (partFade && !settledChange));
                // (opening, once the card is wholly in: its outline is final, and kept. Built
                // again from a later frame, the one a close had just begun to fade (the
                // measurement trails the frame it reads), the faint fill had dropped under
                // the threshold and only the opaque pills were left: the close ran on
                // pill-shaped glass. Only a new layout, a box of another size, rebuilds it.)
                const bool inAndSame = m_openLive && !m_closing && m_fieldTrusted && m_fieldPresence >= 0.98f && m_contentLocal &&
                                       local.w > 0 && std::abs(local.w - m_contentLocal->w) < 1.0 && std::abs(local.h - m_contentLocal->h) < 1.0;
                const bool holdShape = (dropped || m_closing || fadingFrame || inAndSame) && m_field && m_field->isAllocated() && m_contentLocal;
                if (holdShape) {
                    // box and field stay as they were; measured again next frame while
                    // anything is still visible, so a drop that doesn't continue gets its
                    // outline rebuilt (and a closed card stops asking)
                    if (presence > 0.01f)
                        m_contentDirty = true;
                } else if (local.w > 0 && local.h > 0) {
                    const Vector2D sub{local.x / transformBox.w, local.y / transformBox.h};
                    const Vector2D subScale{local.w / transformBox.w, local.h / transformBox.h};
                    m_fieldPresence = presence;
                    const bool built    = [&] {
                              // (the threshold relative to this same frame's strongest content,
                              // read on the GPU; the CPU's reading as a fallback)
                              const GLuint peak = m_probe.drawPeak(maskInfo.textureId, maskInfo.uvOffset, maskInfo.uvScale, static_cast<int>(transformBox.w),
                                                                   static_cast<int>(transformBox.h), m_drawnLocal.w > 0 ? m_drawnLocal : local, target);
                              return ShapeField::build(maskInfo.textureId,
                                           {maskInfo.uvOffset.x + sub.x * maskInfo.uvScale.x, maskInfo.uvOffset.y + sub.y * maskInfo.uvScale.y},
                                           {maskInfo.uvScale.x * subScale.x, maskInfo.uvScale.y * subScale.y},
                                           static_cast<int>(local.w), static_cast<int>(local.h),
                                           peak ? maskThreshold : maskThreshold * std::max(presence, 0.05f), std::min(reach, 256.0f),
                                           (bezelW > 0.0f ? bezelW * scale : reach) * 0.3f, m_fieldA, m_fieldB, m_field, target, peak);
                          }();
                    m_fieldTrusted = built && presence >= 0.98f * std::max(m_peakRef, m_openLive ? m_openPeak : 0.0f);
                    if (!built)
                        m_field.reset(); // no half-float targets: plain alpha-masked glass
                    // the card's exact edges, for the motion to land on (the probe's box is
                    // in whole cells)
                    else if (!m_closing && (!m_motionActive || m_motion.shown() || collapseCancelled)) {
                        CBox exact;
                        if (ShapeField::exactBox(m_field, exact, target)) {
                            exact.translate(local.pos());
                            // (settled and still there: a new layout, not an open or a close)
                            const bool settledCard = m_cardExactValid && !m_motionActive && !m_openLive && m_refValid && presence > 0.9f;
                            const auto oldE = edgesOf(m_cardExact), newE = edgesOf(exact);
                            double moved = 0.0;
                            for (int k = 0; k < 4; k++)
                                moved = std::max(moved, std::abs(oldE[k] - newE[k]));
                            // (another panel in the same layer, the shell's panels share one: the
                            // calendar open and the audio clicked. Not a resize: the new card
                            // opens from its own item, as a fresh drop; gliding there, it grew
                            // out of the panel that was open)
                            const CBox   ov       = exact.intersection(m_cardExact);
                            const double overlap  = ov.w * ov.h / std::max(1.0, std::min(exact.w * exact.h, m_cardExact.w * m_cardExact.h));
                            // (along the bar only: a card growing or shrinking away from the bar,
                            // a search's results, moves its middle too, and opened again)
                            const bool   alongX   = m_nearSide <= 1;
                            const double shift    = std::abs(alongX ? exact.middle().x - m_cardExact.middle().x : exact.middle().y - m_cardExact.middle().y);
                            const double across   = alongX ? std::max(exact.w, m_cardExact.w) : std::max(exact.h, m_cardExact.h);
                            const bool   otherOne = settledCard && moved > 2.0 && (overlap < 0.5 && shift > 0.35 * across);
                            if (otherOne) {
                                m_cardExact      = exact;
                                m_motionCard     = exact;
                                m_drawnLocal     = drawn->box;
                                if (monitor)
                                    startOpen(monitor, transformBox, local);
                            }
                            m_cardExact      = exact;
                            m_cardExactValid = true;
                            m_motionCard     = m_cardExact;
                        }
                    }
                }
                if (!holdShape) {
                    if (!m_contentLocal || *m_contentLocal != local)
                        m_contentChangedAt = std::chrono::steady_clock::now();
                    m_contentLocal = local;
                }
            }
            // A new measurement once the last one is in: replacing an unfinished one
            // would starve a client that commits every frame (a fading card).
            const bool boxResized = m_lastFieldBoxSize != Vector2D{transformBox.w, transformBox.h};
            if ((m_contentDirty || boxResized) && !m_probe.pending()) {
                if (wholeFrame) {
                    m_lastFieldBoxSize = {transformBox.w, transformBox.h};
                    m_probe.request(maskInfo.textureId, maskInfo.uvOffset, maskInfo.uvScale,
                                    static_cast<int>(transformBox.w), static_cast<int>(transformBox.h), 0.004f, target);
                    m_contentDirty = false;
                }
                // the next frame either reads this one back or draws the whole surface
                // (damage added while the pass draws schedules it; nothing else might)
                damageSampleRegion();
            }
            if (!m_contentLocal) {
                // not measured yet (the first frames of a new layer): hidden, as
                // its open starts from the moment it appeared; past MAP_HIDE (a
                // measurement that never came) the surface as it is, no glass
                maskInfo.contentBox = CBox{0.0, 0.0, 0.0, std::chrono::steady_clock::now() - m_mapStart < MAP_HIDE ? 2.0 : 1.0};
                damageSampleRegion();
                break;
            }
            if (m_field && m_field->isAllocated()) {
                maskInfo.sdfTextureId = m_field->getTexture()->m_texID;
                maskInfo.sdfRect      = *m_contentLocal;
            }

            maskInfo.contentBox = m_contentLocal->w > 0 ? *m_contentLocal : CBox{0.0, 0.0, 0.001, 0.001};
            if (m_cachedRegionSampleBox && transformBox.w > 0.0 && transformBox.h > 0.0) {
                const CBox& sb = *m_cachedRegionSampleBox;
                maskInfo.sampleUVOffset = Vector2D((sb.x - transformBox.x) / transformBox.w, (sb.y - transformBox.y) / transformBox.h);
                maskInfo.sampleUVScale  = Vector2D(sb.w / transformBox.w, sb.h / transformBox.h);
            }
            break;
        }
        case EMaskSource::SHAPE_BOX: {
            // glass over the whole inset shape, the surface composited on top
            maskInfo.maskMode       = 2;
            maskInfo.alphaThreshold = 0.0f;
            const bool adaptive     = g_pGlobalState->layerNamespaceAdaptive.contains(layerSurface->m_namespace);
            // (turned on since the last resample, which a config reload doesn't
            // force: built from the cached sample instead)
            if (adaptive && !(m_styleStrip && m_styleStrip->isAllocated()))
                buildStyleStrip(monitor, transformBox, target);
            maskInfo.adaptiveStyle = adaptive && m_styleStrip && m_styleStrip->isAllocated();
            if (maskInfo.adaptiveStyle)
                maskInfo.styleTextureId = m_styleStrip->getTexture()->m_texID;

            SLayerShape shape;
            if (m_popups) {
                for (float& side : shape.inset)
                    side = 0.0f;
                shape.radius = g_pGlobalState->layerNamespacePopupRadius.at(layerSurface->m_namespace);
            } else
                shape = g_pGlobalState->layerNamespaceShapes.at(layerSurface->m_namespace);
            const auto  logical = logicalBox();
            if (logical && monitor) {
                // inset in logical space, then the same pixel/transform path as the layer box
                CBox inner{logical->x + shape.inset[3], logical->y + shape.inset[0],
                           logical->w - shape.inset[1] - shape.inset[3], logical->h - shape.inset[0] - shape.inset[2]};
                inner.translate(-monitor->m_position);
                inner.scale(monitor->m_scale).round().noNegativeSize();
                const CBox innerT = transformedLayerBox(inner, monitor);
                if (innerT.w > 0.0 && innerT.h > 0.0) {
                    maskInfo.glassBoxOffsetPx = {innerT.x - transformBox.x, innerT.y - transformBox.y};
                    maskInfo.glassBoxSizePx   = {innerT.w, innerT.h};
                    // (never past half the short side: a Qt rectangle clamps it so too)
                    cornerRadius = shape.radius < 0.0f ? static_cast<float>(std::min(innerT.w, innerT.h)) * 0.5f
                                                       : std::min(shape.radius * monitor->m_scale, static_cast<float>(std::min(innerT.w, innerT.h)) * 0.5f);
                    // a card coming out of this bar: the bar draws its part of the one
                    // liquid, its edge flowing into the waist (and damaged each frame
                    // by the card's motion, which redraws the whole panel layer)
                    if (!m_popups) {
                        for (const auto& [raw, state] : g_pGlobalState->layerSurfaces)
                            if (state && state.get() != this)
                                state->offerItemSpan(layerSurface, transformBox, m_surfaceTempFramebuffer, target);
                        for (const auto& [raw, state] : g_pGlobalState->layerSurfaces)
                            if (state && state.get() != this && state->mergeIntoBar(layerSurface, monitor, transformBox, maskInfo))
                                break;
                    }
                }
            }
            break;
        }
        case EMaskSource::NONE:
            // hkRenderLayer takes the plain-renderLayer path for NONE; never reaches here.
            break;
    }

    // A soft shadow under the glass, pushed down by the light above (logical px)
    const auto& shadows = m_popups ? g_pGlobalState->layerNamespacePopupShadow : g_pGlobalState->layerNamespaceShadow;
    if (auto sh = shadows.find(layerSurface->m_namespace);
        sh != shadows.end() &&
        (maskSource == EMaskSource::CONTOUR || maskSource == EMaskSource::SHAPE_BOX)) {
        const float scale        = monitor && monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;
        maskInfo.shadowOpacity   = sh->second;
        maskInfo.shadowRangePx   = SHADOW_RANGE * scale;
        maskInfo.shadowOffsetPx  = SHADOW_OFFSET * scale;
    }

    // (the content's visibility now, relative to when its outline was built: a card
    // fading before the live reading exists, a close clicked during the open, kept its
    // text backing at full under fading text, dark ghosts of the pills and labels)
    // (the held copy's own visibility when it stands in: one grabbed from a frame
    // already fading, a close during the open, has faint text under a full backing)
    // (open: full, 2 marks it; only a close fades it, with the text it backs)
    const bool cardOpen  = !m_closing && !m_ghostLive && !(m_motionActive && !m_motion.shown());
    maskInfo.textBacking = cardOpen ? 2.0f : m_ghostLive ? m_heldShown : std::clamp(m_presence / std::max(m_fieldPresence, 0.05f), 0.0f, 1.0f);
    if (m_popups) {
        if (auto pk = g_pGlobalState->layerNamespacePopupFillKey.find(layerSurface->m_namespace); pk != g_pGlobalState->layerNamespacePopupFillKey.end())
            maskInfo.fillKey = pk->second;
    } else if (auto fk = g_pGlobalState->layerNamespaceFillKey.find(layerSurface->m_namespace); fk != g_pGlobalState->layerNamespaceFillKey.end())
        maskInfo.fillKey = fk->second;
    // (which edge of the bar's box is its outer one, against the screen's edge, in the
    // buffer: on the rotated tall screen the bar's box runs down the buffer's left)
    // (the bar itself only: its popups, the tooltips, have no such line, and
    // mirroring their own fill drew a flipped copy of their text along an edge)
    if (!m_popups && g_pGlobalState->layerNamespaceFillKeyTop.contains(layerSurface->m_namespace))
        maskInfo.fillKeyTop = transformBox.w >= transformBox.h ? (transformBox.y <= 1.0 ? 1 : 2) : (transformBox.x <= 1.0 ? 3 : 4);

    // A settled card's glass follows its content's visibility in the very frame
    // (presence.frag into a 1x1 texture), so a fade-out needs no detecting: the
    // glass fades and shrinks with it from the first frame. Redrawn only on the
    // card's own commits; otherwise the texture keeps its last value. (Gating it
    // on a detected close left the glass full for the first frames of the fade,
    // then it jumped down to where the content had got to.)
    const bool live = maskSource == EMaskSource::CONTOUR && !m_openLive && m_contentLocal && m_contentLocal->w > 0 && maskInfo.textureId != 0;
    // (only the card itself has to be in this frame's damage: the shell's fade
    // redraws just the card, never the whole full-screen panel layer)
    const bool cardDrawn = live && m_drawnLocal.w > 0 &&
                           GlassRenderer::boxCovered(m_drawnLocal.copy().translate(transformBox.pos()), g_pHyprRenderer->m_renderData.damage);
    // A settled card is held for its close as soon as it has stood still: a client
    // that commits nothing more after its own fade-in (the calendar) was never held
    // on a real monitor, which redraws only what changed, and every close then began
    // from whatever was on screen.
    // (and again once a card that grew or shrank has stood still: held at its old
    // size, it was never taken again, and a close then refused the stale copy and
    // the card vanished in one frame: Bluetooth, which grows as it finds devices)
    // (and again when the settled card has filled in further since it was held: the
    // weather draws its text and bars a moment after its box settles, and on the tall
    // screen the copy was taken before them, so its close began from a card without
    // them. Only ever on a rise: a fading frame is never taken)
    // (and every HELD_REFRESH while the card redraws, fully shown, nothing in it
    // fading: a card that keeps drawing as it stands, the weather's radar map, was
    // kept from its first moments, a thinner map, and closed from that)
    const auto nowH       = std::chrono::steady_clock::now();
    const bool refreshDue = m_heldValid && cardDrawn && !m_openLive && !m_partFadeSince && m_presence >= 0.99f &&
                            nowH - m_heldAt >= HELD_REFRESH;
    const bool heldStale = !m_heldValid || (m_contentLocal && m_heldRect != *m_contentLocal) ||
        (m_refValid && !m_closing && m_presenceRef > m_heldCoverage * 1.02f) || refreshDue;
    if (live && heldStale && m_refValid && !m_closing && !m_ghostLive && !m_motionActive &&
        std::chrono::steady_clock::now() - m_contentChangedAt >= STABLE_CARD && !m_liveDirty) {
        m_liveDirty = true;
        damageSampleRegion();
    }
    // (and measured on every frame a settled, held card draws: the glass reads this
    // frame's presence to swap in the held card from the client's first fading
    // frame; measured only when it opened, the reading was stale and that first
    // frame showed, dimmed, before the swap: a one-frame dip)
    if (live && m_heldValid && m_refValid && !m_closing && !m_ghostLive && !m_motionActive && !m_openLive && cardDrawn)
        m_liveDirty = true;
    if (live && m_liveDirty && cardDrawn &&
        m_probe.drawLive(maskInfo.textureId, maskInfo.uvOffset, maskInfo.uvScale, static_cast<int>(transformBox.w),
                         static_cast<int>(transformBox.h), 0.004f, m_drawnLocal, target)) {
        m_liveDirty = false;
        m_liveValid = true;
        // the settled card, kept for its close (the pass itself skips a frame that
        // has started to fade, read from the live value it just drew)
        // (only a card whose box has stood still a moment: a client that lays its
        // card out again as it starts to close, the audio panel clearing its device
        // lists, would otherwise close from that last-moment layout)
        // (the notification stack too: it doesn't close from it while it stays, but
        // when its window goes the copy fades out in its place)
        // (only when there is none for this layout yet: refreshed on every frame, a
        // card that redraws while it stands, the weather, could be kept from a frame
        // that showed it part-drawn, and its close began from that)
        if (heldStale && m_refValid && !m_closing && !m_ghostLive && std::chrono::steady_clock::now() - m_contentChangedAt >= STABLE_CARD) {
            const CBox& c = *m_contentLocal;
            const Vector2D off{maskInfo.uvOffset.x + c.x / transformBox.w * maskInfo.uvScale.x,
                               maskInfo.uvOffset.y + c.y / transformBox.h * maskInfo.uvScale.y};
            const Vector2D sc{maskInfo.uvScale.x * c.w / transformBox.w, maskInfo.uvScale.y * c.h / transformBox.h};
            if (ShapeField::hold(maskInfo.textureId, off, sc, static_cast<int>(c.w), static_cast<int>(c.h), m_probe.liveTexture(),
                                 std::max(m_peakRef, 0.002f), !m_heldValid || m_heldRect != c, m_held, target)) {
                m_heldValid    = true;
                m_heldRect     = c;
                m_heldCoverage = m_presenceRef;
                m_heldShown    = 1.0f; // (only ever taken fully shown)
                m_heldPartial  = false;
                m_heldAt       = std::chrono::steady_clock::now();
            }
        }
    }
    if (live && m_liveValid && m_refValid && m_probe.liveTexture() != 0 && !g_pGlobalState->contentInputRecent()) {
        maskInfo.presenceTextureId = m_probe.liveTexture();
        maskInfo.presenceRef       = std::max(m_presenceRef, 0.002f);
        // and shrinks with it from the first frame (the shader mixes from here to
        // full size by the live value; waiting for the close to be detected made the
        // card jump to its shrunk size mid-fade)
        maskInfo.openAnchorPx      = m_openAnchorPx;
        maskInfo.openScale         = OPEN_FROM;
    }

    const bool heldCloseNs = maskSource == EMaskSource::CONTOUR && !g_pGlobalState->layerNamespaceNoHeldClose.contains(layerSurface->m_namespace);

    // A layer without a held close (the notification stack: one notification
    // leaving is not the stack closing) follows its client's fade.
    if (!heldCloseNs && !m_motionActive && m_closing && maskInfo.presenceTextureId == 0) {
        // where the fade has got to by now: a measurement describes a frame drawn
        // one or two frames before it lands (~40 ms readback), and more pass until the
        // next, so the glass trailed the content and caught up in one jump at the end
        const float since     = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_presenceAt).count() + PRESENCE_LATENCY;
        // (and only ever down while closing: a fresh measurement above the estimate
        // would pull the glass back up, a flicker)
        const float est       = std::clamp(m_presence + m_presenceRate * std::min(since, 0.15f), 0.0f, std::clamp(m_presence, 0.0f, 1.0f));
        m_closeForm           = std::min(m_closeForm, est);
        const float form      = m_closeForm;
        if (m_presenceRate < 0.0f && form > 0.0f)
            damageSampleRegion(); // the next frame of the fade
        maskInfo.openAnchorPx = m_openAnchorPx;
        maskInfo.openScale    = OPEN_FROM + (1.0f - OPEN_FROM) * form;
        maskInfo.form         = form;
    }

    const bool closingOwn = m_motionActive && !m_motion.shown();
    // It came back (visible again while closing): the live card, the motion turning
    // back from where it has the card.
    if (heldCloseNs && !m_closing && (closingOwn || m_ghostDone))
        cameBack(closingOwn);

    // (the held copy is read over the field's rect, so both must be there)
    const bool fieldReady = m_field && m_field->isAllocated() && m_contentLocal && m_contentLocal->w > 0;
    const bool heldReady  = fieldReady && m_heldValid && m_held && m_held->isAllocated() && m_heldRect == *m_contentLocal;

    // A close seen: it always runs the card's motion. From the settled card held
    // while it was fully visible, or (closed before it settled: a quick second click)
    // from what is on screen now, held this frame.
    if (heldCloseNs && m_closing && !closingOwn && !m_ghostDone && !m_hideDeferredUntil) {
        // (a close that cuts an open short: its settled copy, if any, is an older card's)
        bool held = heldReady && !m_openLive;
        if (!held && fieldReady && maskInfo.textureId != 0) {
            const CBox& c = *m_contentLocal;
            const Vector2D off{maskInfo.uvOffset.x + c.x / transformBox.w * maskInfo.uvScale.x,
                               maskInfo.uvOffset.y + c.y / transformBox.h * maskInfo.uvScale.y};
            const Vector2D sc{maskInfo.uvScale.x * c.w / transformBox.w, maskInfo.uvScale.y * c.h / transformBox.h};
            held = ShapeField::hold(maskInfo.textureId, off, sc, static_cast<int>(c.w), static_cast<int>(c.h), 0, 0.0f, true, m_held, target);
            if (held) {
                m_heldValid = true;
                m_heldRect  = c;
                // (taken from a frame already fading: its content is that faint)
                m_heldShown   = std::clamp(m_presence / std::max(m_fieldPresence, 0.05f), 0.0f, 1.0f);
                m_heldPartial = true;
            }
        }
        // (from what is on screen: a client that lays its card out again as it
        // starts to close is closed from that layout, as it showed it)
        m_motionCard = m_cardExactValid ? m_cardExact : m_drawnLocal;
        // (gone without a fade of its own, a card hanging from the bar: the shell
        // swapping menus; held for the next one)
        // (a sudden collapse: held as it was, still, until a fade confirms the close;
        // closing at once, a search narrowing its results closed and opened again)
        startClose(monitor, transformBox, held, (m_unmapAt && m_openFromBar && !m_popups) || m_collapseAt.has_value());
    }
    if (m_hideDeferredUntil && std::chrono::steady_clock::now() >= *m_hideDeferredUntil) {
        if (m_collapseAt)
            releaseCollapseHold(); // (no measurement decided: the card stays, as it is now)
        else
            hideNow(motionTime(nowSecs()), monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f);
        damageSampleRegion();
    }

    // The card's motion, whichever way it goes (and however often it turns):
    // its scales about the anchor, its content and its glass, from one Presence.
    if (m_motionActive) {
        const double t     = motionTime(nowSecs());
        const float  scale = monitor && monitor->m_scale > 0.0f ? monitor->m_scale : 1.0f;
        m_motion.advance(t);
        const SMorphFrame f = morphFrame(t, scale);
        // its shape as it is now (never the settled one stretched), the content evenly
        // scaled, faded and blurred inside it
        maskInfo.morph             = true;
        maskInfo.morphRect         = {static_cast<float>(f.rect.x), static_cast<float>(f.rect.y), static_cast<float>(f.rect.x + f.rect.w),
                                      static_cast<float>(f.rect.y + f.rect.h)};
        maskInfo.morphCard         = {static_cast<float>(m_motionCard.x), static_cast<float>(m_motionCard.y), static_cast<float>(m_motionCard.x + m_motionCard.w),
                                      static_cast<float>(m_motionCard.y + m_motionCard.h)};
        maskInfo.morphSource       = {static_cast<float>(f.source.x), static_cast<float>(f.source.y), static_cast<float>(f.source.x + f.source.w),
                                      static_cast<float>(f.source.y + f.source.h)};
        maskInfo.morphRadius       = f.radius;
        maskInfo.morphSourceRadius = f.sourceRadius;
        maskInfo.morphNeck         = f.neck;
        maskInfo.morphBar          = f.source.w > 0.0; // (the bar draws itself: nothing of the card inside it)
        maskInfo.morphWaist        = f.waist;
        maskInfo.morphWaistHalf    = f.waistHalf;
        maskInfo.morphWaistBar     = f.waistBarHalf;
        maskInfo.morphWaistAlongX  = f.waistAlongX;
        maskInfo.morphWaistHold    = f.waistHold;
        maskInfo.morphWaistSnap    = f.waistSnap;
        maskInfo.morphOverlap      = f.overlap;
        // landing: the drawn shape hands over to the card's own field over the last
        // stretch of the open; leaving, back from it at once (motion time)
        maskInfo.morphHand = {static_cast<float>(handAt(t, f.rect)), f.source.w > 0.0 ? std::clamp(f.waist[3], 0.0f, 1.0f) : 0.0f};
        // (at the seam with the bar, its lens: one glass across the join)
        if (f.source.w > 0.0)
            if (const auto barLs = m_barLayer.lock())
                if (const auto it = g_pGlobalState->layerSurfaces.find(barLs.get()); it != g_pGlobalState->layerSurfaces.end() && it->second) {
                    const auto [th, rim] = it->second->lensPx(monitor);
                    maskInfo.morphBarLens = {th, rim};
                    // and its material: the seam is graded as the bar is, so where the
                    // two meet there is no line, one glass
                    const auto g = it->second->grade();
                    maskInfo.barGrade1   = g.g1;
                    maskInfo.barTint     = g.tint;
                    maskInfo.barGradeOn  = true;
                    if (g.styleTex != 0 && !maskInfo.adaptiveStyle) {
                        maskInfo.styleTextureId = g.styleTex;
                        // (the strip spans the bar's quad: this quad's px -> its u)
                        if (const auto bq = LayerGeometry::computeLayerBox(barLs, monitor)) {
                            const CBox b = transformedLayerBox(*bq, monitor);
                            maskInfo.barStyleMap = {static_cast<float>(transformBox.x - b.x), static_cast<float>(1.0 / std::max(b.w, 1.0)),
                                                    static_cast<float>(transformBox.y - b.y), static_cast<float>(1.0 / std::max(b.h, 1.0))};
                            maskInfo.barStyleOn  = true;
                        }
                    }
                }
        maskInfo.contentAnchor     = f.contentAnchor;
        maskInfo.screenAnchor      = f.screenAnchor;
        // (at full size the content moves by whole pixels: a sub-pixel offset
        // resampled fine patterns, a radar's dither dots, into a shimmer each frame)
        if (std::abs(f.scale - 1.0f) < 0.004f) {
            const Vector2D d = f.screenAnchor - f.contentAnchor;
            maskInfo.screenAnchor = f.contentAnchor + Vector2D{std::round(d.x), std::round(d.y)};
        }
        maskInfo.contentScale      = f.scale;
        maskInfo.contentAlpha      = f.alpha;
        // (the shadow by how much of the card is out, squared: full at rest, light
        // under the drop while it moves; at full under the small moving shape it read
        // as a dark smudge travelling with it)
        {
            const double cardA = std::max(m_motionCard.w * m_motionCard.h, 1.0);
            const double a     = std::clamp(f.rect.w * f.rect.h / cardA, 0.0, 1.0);
            maskInfo.shadowOpacity *= static_cast<float>(a * a);
        }
        maskInfo.contentBlurPx     = f.blurPx;
        // (the rim: as wide as the shape now allows, so a thin drop is a low dome)
        maskInfo.glassBoxOffsetPx  = f.rect.pos();
        maskInfo.glassBoxSizePx    = {std::max(f.rect.w, 1.0), std::max(f.rect.h, 1.0)};
        if (auto ext = morphExtent(t, scale, transformBox))
            maskInfo.contentBox = *ext;
        // (the live presence would fade it on its own: the motion owns it)
        maskInfo.presenceTextureId = 0;
        maskInfo.form              = f.glass;
        // The content in a close: the held card, shrinking and fading with the shape
        // on the motion's own timing, as the open brings it in (the client's own
        // fade, ~0.14 s, emptied the glass at once: a close that hardly showed).
        // Without a held copy, the client's own fading frames.
        const auto lsNow = m_layerSurface.lock();
        if (m_ghostLive && m_held && m_held->isAllocated()) {
            maskInfo.heldTextureId = m_held->getTexture()->m_texID;
            maskInfo.ghost         = {2.0f, 1.0f, 1.0f};
            maskInfo.sdfRect       = m_heldRect; // (where the held copy was taken)
        } else if (!m_motion.shown())
            maskInfo.contentAlpha = lsNow && lsNow->m_mapped ? 1.0f : 0.0f;
        damageSampleRegion(); // the next frame
        // (opening: measured again until the content is fully there, so the outline
        // the card lands on is final before it lands; built from the client's own
        // fade-in, it was a hair larger and was rebuilt just after landing, a flicker
        // round the corners)
        if (m_motion.shown() && m_fieldPresence < 0.98f)
            m_contentDirty = true;
        // (held for what comes next, the card waits where it is: not settled, its
        // held content stays drawn; settled, it showed its glass alone for the hold)
        if (m_motion.settled(t) && !m_hideDeferredUntil)
            motionSettled();
    } else if (m_ghostDone && m_held && m_held->isAllocated()) {
        // ran to its end on a layer that stays mapped: nothing until it shows again
        maskInfo.heldTextureId = m_held->getTexture()->m_texID;
        maskInfo.ghost         = {2.0f, 0.0f, 0.0f};
    } else if (heldCloseNs && heldReady && !m_openLive && m_contentLocal && m_heldRect == *m_contentLocal &&
               !g_pGlobalState->contentInputRecent()) {
        // (not while the user scrolls or types: a scrolled list looks fainter for a
        // frame and the held card, the list as it was, stood in for it)
        // settled: armed, so the held card stands in, whole, from the very frame the
        // client starts to fade or empties itself, until the close takes it over
        // (following the client's fade instead, the two swapped a frame or two either
        // way and flickered)
        maskInfo.heldTextureId = m_held->getTexture()->m_texID;
        maskInfo.ghost         = {1.0f, 1.0f, 1.0f};
    }

    if (m_fadeGhost) {
        static constexpr float FADE_GHOST = 0.18f; // s (the shell's own toast fades are ~0.15-0.2 s)
        const float a = 1.0f - std::clamp(std::chrono::duration<float>(std::chrono::steady_clock::now() - m_fadeGhostAt).count() / FADE_GHOST, 0.0f, 1.0f);
        if (m_held && m_held->isAllocated()) {
            maskInfo.heldTextureId     = m_held->getTexture()->m_texID;
            // (eased: the glass a touch behind the text, never text without it)
            maskInfo.ghost             = {2.0f, a * a, std::sqrt(a)};
            maskInfo.sdfRect           = m_heldRect;
            maskInfo.presenceTextureId = 0;
            maskInfo.openScale         = 1.0f;
        }
        if (a <= 0.0f)
            fadeEnded();
        else
            damageSampleRegion();
    }

    // A card out of the bar never shows the bar's own labels in its glass: what its
    // rim bends or mirrors from inside the bar is taken from just below it instead,
    // while it moves and once it has landed alike
    if (maskSource == EMaskSource::CONTOUR && !m_popups && m_openFromBar && m_barPx.w > 0.0 && m_barPx.h > 0.0) {
        maskInfo.foldRect = {static_cast<float>(m_barPx.x), static_cast<float>(m_barPx.y), static_cast<float>(m_barPx.x + m_barPx.w),
                             static_cast<float>(m_barPx.y + m_barPx.h)};
        maskInfo.foldInfo = {m_nearSide >= 2 ? 1.0f : 0.0f, (m_nearSide == 0 || m_nearSide == 2) ? 1.0f : 0.0f};
    }

    // A card frosts by how much of it lies over windows, as frost.cards sets (held
    // while it closes: what is behind didn't change because it is leaving). A bar's
    // popups (tooltips) are menus too: the same frost, over their whole box.
    Frost::SFrost frost;
    const bool cardFrost  = maskSource == EMaskSource::CONTOUR && m_drawnLocal.w > 0;
    const bool popupFrost = maskSource == EMaskSource::SHAPE_BOX && m_popups && transformBox.w > 0;
    if ((cardFrost || popupFrost) && monitor) {
        const auto& config      = g_pGlobalState->config;
        const float setting     = config.frostCards ? static_cast<float>(**config.frostCards) : 0.0f;
        const CBox  over        = cardFrost ? m_drawnLocal.copy().translate(transformBox.pos()) : transformBox;
        const float frostTarget = setting <= 0.0f ? 0.0f :
                                  m_ghostLive || m_ghostDone || (m_motionActive && !m_motion.shown()) ? m_frost.amount() :
                                  Frost::amountFor(setting, Frost::coveredFraction(monitor, over, nullptr));
        if (m_frost.step(frostTarget))
            damageSampleRegion();
        m_frost.prepare(m_sampleFramebuffer, monitor->m_scale, target, frost);
    }

    // the card's own box, for a deep pane's lens: as it landed, kept while it stays
    // open (its content changing size moved the lens's centre, and with it the whole
    // view through the glass, at every change of a search's results)
    if (maskSource == EMaskSource::CONTOUR) {
        if (!m_motionActive && m_cardExactValid && !m_paneAnchor)
            m_paneAnchor = m_cardExact;
        const CBox& pb = m_paneAnchor ? *m_paneAnchor : m_cardExactValid ? m_cardExact : m_drawnLocal;
        if (pb.w > 0.0 && pb.h > 0.0)
            maskInfo.paneRect = {static_cast<float>(pb.x), static_cast<float>(pb.y), static_cast<float>(pb.x + pb.w), static_cast<float>(pb.y + pb.h)};
    }

    // (a bar's popup fading in: its text with its glass)
    if (m_popups)
        maskInfo.outFade = std::clamp(alpha, 0.0f, 1.0f);

    // The glass shader composites both the glass effect and the surface content
    // in a single pass: glass behind, surface on top, using the temp FBO alpha.
    GlassRenderer::applyGlassEffect(m_sampleFramebuffer, target,
                                     rawBox, transformBox, alpha,
                                     std::array<float, 4>{cornerRadius, cornerRadius, cornerRadius, cornerRadius},
                                     roundingPower, m_samplePaddingRatio, ctx,
                                     &maskInfo, &frost);
}
