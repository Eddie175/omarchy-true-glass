#pragma once

#include <algorithm>

#include "motion/motion.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#include "GlassRenderer.hpp"
#include "PluginConfig.hpp"
#include "ShapeField.hpp"

#include <chrono>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprutils/math/Region.hpp>

class CGlassLayerSurface {
  public:
    // popups: this state glasses the layer's popups (their bounding box), not the layer itself
    explicit CGlassLayerSurface(PHLLS layerSurface, bool popups = false);
    ~CGlassLayerSurface();

    // Where the glass mask for this layer comes from this frame.
    // CONTOUR: the surface's own alpha, with its rim taken from a distance field
    // of that shape. SHAPE_BOX: a fixed rounded box inside the layer (inset/radius).
    enum class EMaskSource { ALPHA_THRESHOLD, PROTOCOL_REGION, NONE, CONTOUR, SHAPE_BOX };

    // Phase 1 (pre-surface): sample+blur background, redirect currentFB → temp FBO
    void sampleAndRedirect(PHLMONITOR monitor, float alpha);

    // Phase 2 (post-surface): restore currentFB, apply glass masked by temp FBO, blit surface
    void compositeAndRestore(PHLMONITOR monitor, float alpha, EMaskSource maskSource);

    void damageIfMoved();

    // Content below committed damage in our sample region — resample next frame
    void markBackgroundDirty();
    void oweResample(std::chrono::steady_clock::duration wait);

    [[nodiscard]] bool liveResampleEnabled() const;

    // Decides ALPHA_THRESHOLD vs PROTOCOL_REGION vs NONE for this layer, based on
    // mask_mode and the root surface's ext-background-effect-v1 state.
    [[nodiscard]] EMaskSource resolveMaskSource() const;

    [[nodiscard]] PHLLS getLayerSurface() const;

    // What this state draws glass over, in global logical coordinates: the layer,
    // or the union of its mapped popups.
    [[nodiscard]] std::optional<CBox> logicalBox() const;
    // logicalBox() in monitor-local framebuffer pixels (LayerGeometry::computeLayerBox's space)
    [[nodiscard]] std::optional<CBox> glassBox(PHLMONITOR monitor) const;
    // glassBox() back in monitor-local logical coordinates, padded (pass element bounding boxes)
    [[nodiscard]] std::optional<CBox> paddedLogicalBox(PHLMONITOR monitor, float paddingPx) const;
    // Fade of the popups (1 for a layer state): drives their materialization
    [[nodiscard]] float popupAlpha() const;
    [[nodiscard]] bool  hasSample() const { return m_hasCachedSample; }
    // (a bar's tooltip popup stays mapped as it moves from item to item: each new
    // place fades in again)
    mutable std::optional<CBox>                   m_popupBox;
    mutable std::chrono::steady_clock::time_point m_popupFadeAt{};
    // A commit from this surface is the glassed content itself, not its background
    [[nodiscard]] bool ownsSurface(const SP<CWLSurfaceResource>& surface) const;
    [[nodiscard]] bool isPopups() const { return m_popups; }

    // The glassed content committed: a contour layer measures it again and
    // rebuilds its distance field once that measurement is in (and only then)
    // a commit of the surface: measure it again and redraw its live visibility; and
    // the whole layer is redrawn this frame, so the visibility read on the GPU and
    // the picture always come from the same frame (a client fading only part of its
    // card each frame left them apart: the held copy and the fading card swapped
    // back and forth as the close began, a flicker on a real monitor only)
    void markContentDirty() {
        m_contentDirty = true;
        m_liveDirty    = true;
        if (m_heldValid || m_motionActive)
            damageSampleRegion();
    }

    // Called every render with the layer's mapped state: a fresh map is
    // measured from nothing, so its content opens (startOpen) as it appears.
    void noteMapped(bool mapped);
    // drawn over the whole screen: no glass for it this mapping (hkRenderLayer draws it plain)
    [[nodiscard]] bool plainCover() const { return m_plainCover; }

    // The close the compositor draws itself (from the held settled card) is still
    // running: it outlives the client's own fade, and a popup's unmap
    [[nodiscard]] bool ghostLive() const { return m_ghostLive; }
    // its window went without a fade of its own: close it from the held copy (true: drawn from now)
    bool beginUnmapClose();
    // A close still running ends now, as if done (a screenshot is about to capture
    // the screen: the half-closed card was frozen into it, and stayed on screen
    // while the region was picked)
    void finishCloseNow();
    // Another card of the same kind opened as this one closes (the bar's panels:
    // Wi-Fi, then Bluetooth): this one fades where it is instead of draining into
    // its item, a strip across the new card that made the new one look as if it
    // came out of the old one's item.
    void switchAway();
    // (the other cards of this kind: one closing a moment ago fades where it is)
    void switchOthersAway() const;
    std::optional<CBox> switchSeedFrom(PHLMONITOR monitor, const CBox& transformBox) const;
    void rebaseOntoBar(double t, float monitorScale);
    void hideNow(double t, float monitorScale);
    void releaseCollapseHold();
    std::chrono::steady_clock::time_point m_collapseQuietUntil{};
    float m_peakRef = 1.0f; // the open card's strongest cell (the held copy's gate)
    float m_openPeak = 0.0f; // coming in: its strongest cell so far (falling well below it is a close)
    std::optional<CBox> m_paneAnchor; // the card as it landed: a deep pane's lens centre while it stays open
    std::chrono::steady_clock::time_point m_syncMeasureAt{}; // the last same-frame measurement
    int m_syncBurst = 0;
    // the bar this card came out of draws now: read the clicked item's extent from it
    void offerItemSpan(const PHLLS& bar, const CBox& barBox, const SP<Render::IFramebuffer>& barFb, const SP<Render::IFramebuffer>& target);
    // gone without a close to run: let the layer go
    void dropHold() {
        if (!m_ghostLive)
            m_ghostHold.reset();
    }
    // An unmap close that hasn't started in time (the screen locked or off, its
    // monitor gone) or is still holding the layer long after any close would have
    // ended: let it go. True when it was let go.
    bool expireUnmap() {
        if (!m_unmapAt)
            return false;
        const auto since = std::chrono::steady_clock::now() - *m_unmapAt;
        const bool late  = !m_ghostLive && since > std::chrono::milliseconds(300);
        if (!late && since < std::chrono::milliseconds(1500))
            return false;
        m_unmapAt.reset();
        m_ghostLive = false;
        m_fadeGhost = false;
        m_ghostDone = true;
        m_ghostHold.reset();
        return true;
    }
    // A card coming out of (or going back into) this bar: the bar draws its part of
    // the one liquid (bar, waist, card) with the same shape, so its edge flows into
    // the waist instead of crossing it. Fills mask's morph shape (in the bar's quad
    // px, barBox) when this card is moving out of `bar`; false otherwise.
    bool mergeIntoBar(const PHLLS& bar, PHLMONITOR monitor, const CBox& barBox, GlassRenderer::SMaskInfo& mask);
    // this surface's glass height and rim, framebuffer px (its preset)
    std::pair<float, float> lensPx(PHLMONITOR monitor) const;
    // this surface's grading (brightness, adaptive dim, contrast, saturation; tint
    // rgb + alpha) and its style strip (0: none), for a card flowing out of it
    struct SGrade {
        std::array<float, 4> g1{1.0f, 0.0f, 1.0f, 1.0f}, tint{0.0f, 0.0f, 0.0f, 0.0f};
        GLuint               styleTex = 0;
    };
    SGrade grade() const;

    // PROTOCOL_REGION only: the blur region's bounding box in the same transformed
    // monitor-pixel space as transformBox/regionRects (matches transformedBlurRegion()
    // below). layerBox is the caller's already-computed LayerGeometry::computeLayerBox()
    // result (both call sites have one in hand; avoids recomputing it here). Used to
    // shrink the sample blit box, never the composite draw or boundingBox() (both stay
    // full-layer — see GlassLayerSurface.cpp). Nullopt when the transformed region is empty.
    [[nodiscard]] std::optional<CBox> regionBoundingBoxAbsolute(PHLMONITOR monitor, const CBox& layerBox) const;

    // PROTOCOL_REGION only: the blur region's bounding box in global-logical
    // coordinates — the family BackgroundDamageObserver's damagedBox uses (no
    // monitor scale/transform: logical space needs none). Deliberately a
    // separate coordinate family from regionBoundingBoxAbsolute() above; reusing
    // that monitor-pixel-space box against damagedBox would be silently wrong at
    // any scale != 1 or monitor position != (0,0).
    [[nodiscard]] std::optional<CBox> regionBoundingBoxGlobal() const;

  private:
    PHLLSREF     m_layerSurface;
    bool         m_popups = false;

    // CONTOUR: jump-flood ping/pong and the resolved distance field (sized to m_contentLocal)
    SP<Render::IFramebuffer> m_fieldA, m_fieldB, m_field;
    SP<Render::IFramebuffer> m_styleStrip; // adaptive bars: backdrop brightness along the bar
    bool                     m_contentDirty = true;
    // where the content is (refreshed with the field): sampling and drawing stay there
    ShapeField::CContentProbe m_probe;
    std::optional<CBox>      m_contentLocal; // box-local px of the glass quad

    // open/close animation
    bool                                  m_seenMapped = false;
    bool                                  m_openLive   = false;
    bool                                  m_closing    = false;
    bool                                  m_lastDropped = false; // the last measurement's visibility fell
    float                                 m_presenceRate = 0.0f; // closing: visibility per second (<= 0)
    float                                 m_closeForm    = 1.0f; // closing: the glass so far (only falls)
    float                                 m_closeLow     = 1.0f; // closing: the least visible it has been (a reopen rises clearly above it)
    bool                                  m_liveDirty    = true;  // own commit since the live presence was last drawn
    bool                                  m_liveValid    = false; // the live texture describes the current content
    CBox                                  m_drawnLocal;            // the card's own box (content box without the shadow margin)
    float                                 m_presenceRef  = 1.0f;  // settled coverage: the live value is read relative to it
    bool                                  m_refValid     = false; // m_presenceRef was measured since the last open
    std::chrono::steady_clock::time_point m_presenceAt{};        // when m_presence was measured
    Vector2D                              m_openAnchorPx;   // quad px
    float                                 m_presence = 0.0f; // how visible the content is (0..1)
    Vector2D                              m_lastFieldBoxSize; // box size of the last measurement
    void startOpen(PHLMONITOR monitor, const CBox& transformBox, const CBox& contentLocal);

    // closing, drawn by the compositor: the settled card (m_contentLocal's px, kept by
    // the hold pass while it is fully visible) goes away on its own timeline
    SP<Render::IFramebuffer>              m_held;
    bool                                  m_heldValid = false;
    CBox                                  m_heldRect;              // the m_contentLocal it was held over
    double                                m_hideAt = 0.0;          // motion time the last close began
    CBox                                  m_cardExact;             // the card's exact edges, from its field (quad px)
    bool                                  m_cardExactValid = false;
    CBox                                  m_motionCard;            // the card the morph runs to (opening) or from (closing)
    std::chrono::steady_clock::time_point m_contentChangedAt{};    // when m_contentLocal last changed
    bool                                  m_ghostLive = false;
    bool                                  m_ghostDone = false;     // ran to its end; nothing left to draw
    std::chrono::steady_clock::time_point m_mapStart{};            // when this mapping began (the open's clock)
    std::chrono::steady_clock::time_point m_firstDropAt{};         // the frame a fade began in (the close's clock)
    std::optional<std::chrono::steady_clock::time_point> m_collapseAt; // a held card that collapsed in one step: closing from it since
    float                                 m_collapsePresence = 1.0f;   // ... and how visible it was then (back to that: it was a resize)
    // a layer without a held close (the notification stack) whose window went: its
    // held copy fades out where it is, glass and text together
    bool                                  m_fadeGhost = false;
    float                                 m_fieldPresence = 0.0f;
    // cards (by size, 8 px cells) seen to empty themselves before they fade (the audio
    // panel clears its lists): for those the held copy stands in from the first frame
      // how visible the content was when the field was last built
    // A settled card whose layout grows or shrinks (a list filling in): its glass
    // glides to the new size on springs, one per edge, instead of jumping there
    // the bar item it opened from, along the bar (quad px), read from the bar's own
    // underline for the open item: the drop comes out of that item, as wide as it
    bool                                  m_spanWant = false, m_spanValid = false;
    int                                   m_spanTries = 0;
    double                                m_spanLo = 0.0, m_spanHi = 0.0;
    std::chrono::steady_clock::time_point m_spanAt{};              // found then: eased into over 80 ms (it arrives a frame or two into the open)
    bool                                  m_switchedAway = false, m_switchAwayWanted = false;
    float                                 m_heldCoverage = 0.0f;
    float                                 m_heldShown = 1.0f; // how visible the content was when the held copy was taken
    bool                                  m_heldPartial = false; // held from what was on screen as a close cut an open short (that close's only)
    bool                                  m_fieldTrusted = false; // the field was measured from the whole card (handAt)
    std::chrono::steady_clock::time_point m_heldAt{};     // when the held copy was taken
    static constexpr auto                 HELD_REFRESH = std::chrono::milliseconds(500);
    // a frame fading part of the card (cells dimming, none appearing): since when
    std::optional<std::chrono::steady_clock::time_point> m_partFadeSince;
    static constexpr auto                 FADE_SETTLE = std::chrono::milliseconds(300); // (the shell's fades take ~0.15 s) // the settled card's coverage when it was held
    bool                                  m_openClamp = false; // an open still under way: the drop keeps its proportions (morphFrame)
    std::optional<CBox>                   m_switchSeed;        // opened straight from another card: its shape then, quad px (the morph starts there)
    std::optional<std::chrono::steady_clock::time_point> m_hideDeferredUntil; // went without a fade: held where it is, for a card that takes over
    std::chrono::steady_clock::time_point m_openStartAt{}, m_closeStartAt{};
    // out of the bar and back into it as a droplet at the item's middle, as wide as
    // a small item and no wider than DROP_MAX (a wide item's whole width, the
    // clock's, came out and went back as a slab)
    static constexpr double               DROP_MAX      = 44.0; // logical px
    [[nodiscard]] double dropItem(float monitorScale) const {
        const double item = std::max(m_spanHi - m_spanLo, 16.0 * monitorScale);
        return std::min(item, DROP_MAX * monitorScale);
    }
    [[nodiscard]] double spanBlend() const {
        if (!m_spanValid)
            return 0.0;
        const double x = std::clamp(std::chrono::duration<double>(std::chrono::steady_clock::now() - m_spanAt).count() / 0.08, 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    }
    Vector2D                              m_quadPos;               // this layer's quad in the work buffer (px)
    std::optional<std::chrono::steady_clock::time_point> m_unmapAt; // its window went and its close is pending or running
    std::chrono::steady_clock::time_point m_fadeGhostAt{};
    // how the card comes and goes: one interruptible motion (motion::Morph, the
    // island choreography): its shape morphs out of the bar item (or where it is),
    // its content scales evenly, fades and unblurs; see morphFrame()
    motion::Morph                         m_motion;
    motion::tokens::MorphGeometry                 m_geo{};
    bool                                  m_motionActive = false;  // m_motion is moving, or about to
    // One frame of the morph, quad px: the card's rounded rect, the bar it comes out
    // of and the neck between them, the content's even transform and its fade.
    struct SMorphFrame {
        CBox     rect, source;
        float    radius = 0.0f, sourceRadius = 0.0f, neck = 0.0f;
        Vector2D contentAnchor, screenAnchor;
        float    scale = 1.0f, alpha = 1.0f, blurPx = 0.0f, glass = 1.0f;
        // the waist to the bar: along-centre, from (in the bar) and to (in the card)
        // across, how open (0 snapped .. 1 widest); its widest half width
        std::array<float, 4> waist{0.0f, 0.0f, 0.0f, 0.0f};
        float    waistHalf = 0.0f, waistN = 0.0f, waistBarHalf = 0.0f; // (bar: its half width at the bar, the item's)
        bool     waistAlongX = true;
        float    overlap = 0.0f; // how far the card's near edge is inside the bar, px (< 0: below it)
        float    waistHold = 1.0f, waistSnap = 0.0f; // how long it keeps its width; the open value it breaks at (0: melts)
    };
    SMorphFrame         morphFrame(double t, float monitorScale) const;
    // everything the morph may draw from t on (its shadow included), quad px
    std::optional<CBox> morphExtent(double t, float monitorScale, const CBox& quad) const;
    // the seed it grows from and the bar it comes out of (empty: none), quad px
    void                morphSeed(float monitorScale, CBox& seed, CBox& bar) const;
    double                                m_warpBase = 0.0, m_warpLag = 0.0, m_warpAt = 0.0; // events seen late, being made up
    double motionTime(double now) const;
    void   catchUp(double now, double lag);
    mutable bool                          m_barAlongX = true;      // the bar runs across (x), else down the side (set by barAnchor)
    mutable CBox                          m_barPx;                 // the bar, quad px (set by barAnchor)
    mutable PHLLSREF                      m_barLayer;              // the bar's layer (set by barAnchor; not for popups)
    mutable int                           m_nearSide = 0;          // the card's edge facing the bar: 0 top, 1 bottom, 2 left, 3 right
    std::optional<CBox>                   m_lastLogical;           // the last box drawn while mapped
    std::optional<CBox>                   m_ghostLogical;          // popups: kept while the close outlives them
    // a layer unmapped by its client may be freed at once (Hyprland keeps only a
    // snapshot): held until its close has run, which draws in the fade-out stage
    PHLLS                                 m_ghostHold;
    void startClose(PHLMONITOR monitor, const CBox& transformBox, bool held, bool deferHide = false);
    // The card's life, one state at a time (derived: the flags below are only ever
    // changed by the transitions that follow, and by startOpen/startClose/hideNow/
    // beginUnmapClose/finishCloseNow/releaseCollapseHold)
    enum class ECard : uint8_t {
        Hidden,  // nothing shown
        Opening, // coming in
        Open,    // landed
        Closing, // its close seen, or under way (from the held copy when m_ghostLive)
        Held,    // gone without a fade (or collapsed): held where it is for what comes next
        Fading,  // a stack card fading in place
        Done,    // its close ran out on a layer still mapped: nothing until it shows again
    };
    [[nodiscard]] ECard phase() const;
    void                forgetClose();
    void                closeSeen(float presence);
    void                reopenSeen();
    void                collapseSeen(float presence);
    void                collapseFades(double t, float monitorScale);
    void                cameBack(bool turning);
    void                motionSettled();
    void                fadeEnded();
    void                dropPartialHeld();
    [[nodiscard]] double handAt(double t, const CBox& rect) const;
    // the bar this card hangs from: the point of the card's edge facing it (nearest
    // `near` along it) and the scale the card has at the bar (along, away); false
    // when the card doesn't hang from a bar
    bool barAnchor(PHLMONITOR monitor, const CBox& transformBox, const CBox& card, const Vector2D& near, Vector2D& anchor) const;
    bool                                  m_openFromBar = false;   // opening out of the bar (springs with a little life)
    // the content covers the whole screen (a theme's dim behind a menu, left on):
    // no card can be made of it. Drawn as Hyprland draws it until the layer maps again
    bool m_plainCover = false;
    Vector2D                              m_openBarAnchorPx;
    // m_styleStrip from the cached sample (reset when it can't be built)
    void buildStyleStrip(PHLMONITOR monitor, const CBox& transformBox, const SP<Render::IFramebuffer>& callerFramebuffer);
    SP<Render::IFramebuffer> m_sampleFramebuffer;
    Frost::CState            m_frost; // cards: frosted where windows are behind (frost.cards)
    SP<Render::IFramebuffer> m_surfaceTempFramebuffer;
    Vector2D     m_samplePaddingRatio;
    bool         m_hasCachedSample = false;
    bool         m_backgroundDirty = false;
    std::chrono::steady_clock::time_point m_lastDirtyMark{};
    bool                                  m_trailingDirty = false; // a throttled change, still owed
    SP<CEventLoopTimer>                   m_owedTimer;             // pays a throttled change when its turn comes (markBackgroundDirty)

    // Set at the end of sampleAndRedirect() when currentFB was actually redirected
    // to the temp FBO this frame, cleared by compositeAndRestore() after it reads it.
    // Guards against compositing against a stale temp FBO when the render pass
    // discarded the pre-surface element (see disableSimplification() in
    // GlassLayerPassElement.cpp).
    bool         m_redirectedThisFrame = false;

    void damageSampleRegion();

    // Track last position/size to detect movement and expand damage
    Vector2D     m_lastPosition;
    Vector2D     m_lastSize;

    // Scene generation at last blur — skip re-sampling when only the layer
    // surface content changed (e.g. clock tick) but the background didn't.
    uint64_t     m_lastSceneGeneration = 0;

    // Transformed blur region computed by sampleAndRedirect() this frame
    // (PROTOCOL_REGION only, empty otherwise), reused by compositeAndRestore()'s
    // regionRects mask upload so the scale/translate/transform sequence doesn't
    // run twice per frame. compositeAndRestore() recomputes via
    // transformedBlurRegion() as a defensive fallback if this is empty when it
    // shouldn't be (mask source disagreeing between the two calls in one frame).
    CRegion      m_cachedTransformedRegion;

    // The box the cached sample was taken from, set by each resample in
    // sampleAndRedirect() (same coordinate space as m_cachedTransformedRegion's
    // extents): the region's bounding box (PROTOCOL_REGION) or the content's
    // (CONTOUR), nullopt otherwise. Reused by compositeAndRestore() to build the
    // sampleUVOffset/uvScale mask values that reconcile sampleBackground()'s
    // shrunk sample box against the full-layer quad UV (sampleXform in
    // Shaders.hpp) — without this, sampleBlurred() would
    // index the (now smaller) sample texture as if it still covered the whole
    // layer, warping the visible blur. Also compared frame-to-frame in
    // sampleAndRedirect() to force a resample when the region itself moves or
    // resizes with no other invalidation trigger (a layer's own commit doesn't
    // mark itself dirty).
    std::optional<CBox> m_cachedRegionSampleBox;

    // Surface-local logical blur region intersected to the layer's own size,
    // then scaled/translated/transformed into transformBox's pixel space —
    // shared by regionBoundingBoxAbsolute() and compositeAndRestore()'s
    // regionRects upload so both use identical, proven math.
    [[nodiscard]] CRegion transformedBlurRegion(PHLMONITOR monitor, const CBox& rawBox) const;

    // Saved currentFB pointer, restored in compositeAndRestore
    SP<Render::IFramebuffer> m_savedCurrentFB;

    [[nodiscard]] bool           resolveThemeIsDark() const;
    [[nodiscard]] std::string    resolvePresetName() const;
    [[nodiscard]] ELayerMaskMode resolveMaskMode() const;
};
