-- Glass: the engine's configuration. The Glass plugin's service loads the engine
-- and runs this file with `hyprctl eval`, and again after every Hyprland config
-- reload (a reload resets the engine's settings and the rules below). Nothing here
-- is written to your Hyprland config: Glass goes away with the plugin.
--
-- GLASS is set by the service just before: the settings from the Glass menu and
-- the colours of the theme in use (cards and tooltips are keyed by them).

local G = GLASS
local hg = hl.plugin.hyprglass
if not G or not hg then return end

-- Clear glass (n = 1.52) with strong dispersion (Abbe 6), so rims split light into
-- colour; a hairline glint from the upper left; the rim mirrors the scene around
-- it. Almost no frosting or tint: the glass reads by its refraction.
local glass = {
  ior = 1.52, abbe = 6,
  specular_angle = 315, light_elevation = 35, specular_strength = 0.35,
  fresnel_strength = 0.25,
  -- one thin edge round every glass shape: lit facing the light, dark opposite
  bevel_strength = 0.25, bevel_size = 1.2, bevel_angle = 315, bevel_shadow = 0.18,
  tint_color = 0x16161606,
  dark = { brightness = 1.0, contrast = 1.0, saturation = 1.0, vibrancy = 0.0, adaptive_dim = 0.08, adaptive_boost = 0.0 },
  light = { brightness = 1.0, contrast = 1.0, saturation = 1.0, vibrancy = 0.0, adaptive_dim = 0.08, adaptive_boost = 0.0 },
}
-- Low power (older computers): no colour split at the rim (one read of the picture
-- behind instead of three) and no rim reflection (four fewer); the bend, the edge
-- light and the motion stay
if G.low_power then
  glass.abbe = 0
  glass.fresnel_strength = 0
end
local function preset(over)
  local p = {}
  for k, v in pairs(glass) do p[k] = v end
  for k, v in pairs(over) do p[k] = v end
  return p
end

-- The bar's glass is thin and bends only along its bottom edge; cards and windows
-- are thicker, with a wide rim so the bend rolls in gently; small cards and
-- tooltips keep a narrow rim so the lens frames their text.
hg.preset("glass-bar", preset({ thickness = 24, bezel_width = 24, blur_strength = 0.0, blur_iterations = 1, adaptive_dim = 0.1,
                                bevel_angle = 180, bevel_strength = 0.2, bevel_shadow = 0.0 }))
hg.preset("glass-window", preset({ thickness = 30, bezel_width = 26, blur_strength = 0.0, blur_iterations = 1, bevel_strength = 0.0 }))
hg.preset("glass-card", preset({ thickness = 56, bezel_width = 44, blur_strength = 0.0, blur_iterations = 1, bevel_size = 1.6 }))
hg.preset("glass-small", preset({ thickness = 20, bezel_width = 16, blur_strength = 0.0, blur_iterations = 1 }))
hg.preset("glass-tip", preset({ thickness = 10, bezel_width = 8, blur_strength = 0.0, blur_iterations = 1, bevel_size = 1.0,
                                bevel_strength = 0.8 }))

hg.config({
  enabled = true,
  default_theme = "dark",
  default_preset = "glass-window",
  -- (menus follow what changes behind them at its own rate, a video at its frame
  -- rate; Low power keeps them to 30 updates a second)
  layers = { enabled = true, live_resample_fps = G.low_power and 30 or 0 },
})

-- The bar keeps Omarchy's own geometry (a full-width strip): its glass runs past
-- the screen's top and sides, so its only rim is along the bottom.
-- Corners: the theme's own (Omarchy's stock themes are square: their rims are
-- mitred, meeting on the diagonal like a cut glass block), or round everywhere with
-- Round corners (windows here; the shell rounds its menus and buttons to match)
local ROUND = 20
if G.round then hl.config({ decoration = { rounding = ROUND } }) end
local radius = G.round and ROUND or math.max(G.rounding or 0, 0)
hg.config({ corner_miter = radius == 0 and 1 or 0 })

-- A light theme: its menus' text is dark, so the clear glass keeps it readable with
-- a light veil right around it over a dark backdrop (a dark theme's light text gets
-- a dark one), and the light card behind it is never taken for text
hg.config({ light_ui = G.light_ui and 1 or 0 })

hg.layer("omarchy-bar", {
  preset = "glass-bar", shape = { inset = { -80, -80, 0, -80 }, radius = 0 },
  adaptive = true, popups = true, popup_preset = "glass-tip", popup_radius = radius, mask_threshold = 0.04,
  -- the open item's accent line is drawn along the bar's top edge, as the card
  -- flows out of the bar's bottom edge there
  fill_key = G.accent, fill_alpha = 0.0, fill_key_top = true,
  -- its tooltips: the theme's solid tooltip fill keyed back to light glass
  popup_fill_key = G.tooltip, popup_fill_alpha = 0.08,
})
-- Cards drawn by the shell: the glass follows each card's own outline, and the
-- theme background the cards paint is keyed down to a light tint
local small = { ["omarchy-notifications"] = true, ["omarchy-osd"] = true, ["omarchy-reminders"] = true }
for _, ns in ipairs({ "omarchy-keyboard-panel", "omarchy-notifications", "omarchy-menu", "omarchy-osd",
                      "omarchy-polkit", "omarchy-clipboard", "omarchy-emojis", "omarchy-reminders" }) do
  hg.layer(ns, { preset = small[ns] and "glass-small" or "glass-card", mask_mode = "contour", shadow = small[ns] and 0.16 or 0.2,
                 mask_threshold = 0.04, fill_key = G.card, fill_alpha = 0.03,
                 -- (the notification stack fades in place: one toast leaving is
                 -- not the stack closing)
                 held_close = ns ~= "omarchy-notifications" })
end

-- The settings from the Glass menu (frost: 0 clear, 0.5 auto, 1 frosted)
hg.config({
  frost = { windows = G.windows, cards = G.menus, blur = 32, milk = 0 },
  depth = G.depth,
  low_power = G.low_power and 1 or 0,
  dark_glass = G.tone == "dark" and 1 or 0,
  light_glass = G.tone == "light" and 1 or 0,
  gleam = G.light and 1 or 0,
  gleam_style = G.light_style,
})

-- The screensaver runs in a terminal: the engine leaves it alone, and Hyprland's
-- dim-around puts solid black behind it
hl.window_rule({ match = { class = "^(org.omarchy.screensaver)$" }, tag = "+hyprglass_disabled", dim_around = true })
hl.config({ decoration = { dim_around = 1.0 } })

-- Clear terminals: their background is see-through (the menu's Clear terminals),
-- so Hyprland's own window opacity would only fade the text; and with the border
-- light on, the light marks the window you're in instead of a painted border.
if G.clear_terminals then
  hl.window_rule({ match = { tag = "terminal" }, opacity = "1 1" })
  if G.light then
    hl.window_rule({ match = { class = "^(com.mitchellh.ghostty|foot|kitty|Alacritty)$" }, border_size = 0 })
  end
end

-- Cards move with the engine's own motion: Hyprland's layer fade is off for the
-- cards whose windows go the moment they close (the engine closes them from the
-- card it held), and its popup fade (a plain snapshot without glass) is off.
hl.layer_rule({ match = { namespace = "^(omarchy-osd|omarchy-polkit|omarchy-reminders|omarchy-notifications)$" }, no_anim = true, animation = "none" })
-- The bar's menus flow out of the bar with the engine's motion: Hyprland only fades
-- their layer, whatever its own layer animation (a zoom such as "popin" scaled the
-- layer under the moving glass, and a menu poured back into a spot beside its icon)
hl.layer_rule({ match = { namespace = "^(omarchy-keyboard-panel)$" }, animation = "fade" })
hl.animation({ leaf = "fadePopupsOut", enabled = false })

-- presets and layers given here apply now (from a config file they would wait
-- for the reload to finish)
hg.commit()
