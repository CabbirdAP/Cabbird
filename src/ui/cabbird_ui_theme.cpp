// Shared palette tokens, responsive font atlas and rounded glass control geometry.
#include "cabbird/cabbird_ui_theme.hpp"
#include "cabbird/liquid_glass.hpp"

#include <imgui.h>

#include "cabbird_ui_gb2312.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cabbird {
namespace {

constexpr CabbirdUiColor Color(
    const float red, const float green, const float blue, const float alpha = 1.0f) {
    return {red, green, blue, alpha};
}

/* Field order matches CabbirdUiThemeColors exactly. */
constexpr CabbirdUiThemeColors kMoss{
    Color(0.949f, 0.957f, 0.965f), Color(0.451f, 0.490f, 0.533f),
    Color(0.078f, 0.090f, 0.102f), Color(0.106f, 0.125f, 0.145f),
    Color(0.137f, 0.165f, 0.192f), Color(0.204f, 0.235f, 0.271f),
    Color(0.106f, 0.125f, 0.145f), Color(0.137f, 0.165f, 0.192f),
    Color(0.150f, 0.180f, 0.208f), Color(0.106f, 0.125f, 0.145f),
    Color(0.137f, 0.165f, 0.192f), Color(0.180f, 0.260f, 0.255f),
    Color(0.345f, 0.718f, 0.647f), Color(0.415f, 0.773f, 0.706f),
    Color(0.290f, 0.635f, 0.570f), Color(0.345f, 0.718f, 0.647f, 0.16f),
    Color(0.380f, 0.788f, 0.545f), Color(0.882f, 0.675f, 0.322f),
    Color(0.898f, 0.435f, 0.447f), Color(0.431f, 0.659f, 0.996f),
    Color(0.063f, 0.129f, 0.122f), Color(0.063f, 0.075f, 0.090f),
    Color(0.086f, 0.102f, 0.118f), Color(0.090f, 0.106f, 0.122f),
    Color(0.118f, 0.141f, 0.165f), Color(0.125f, 0.149f, 0.173f),
    Color(0.094f, 0.114f, 0.133f), Color(0.298f, 0.337f, 0.380f),
    Color(0.851f, 0.973f, 0.945f), Color(0.125f, 0.149f, 0.173f),
    Color(0.306f, 0.349f, 0.396f), Color(0.118f, 0.141f, 0.165f, 0.98f),
};

constexpr CabbirdUiThemeColors kAurora{
    Color(0.925f, 0.957f, 1.000f), Color(0.560f, 0.650f, 0.760f),
    Color(0.055f, 0.078f, 0.130f), Color(0.075f, 0.106f, 0.170f),
    Color(0.110f, 0.150f, 0.230f), Color(0.200f, 0.290f, 0.420f),
    Color(0.075f, 0.106f, 0.170f), Color(0.110f, 0.150f, 0.230f),
    Color(0.130f, 0.200f, 0.300f), Color(0.075f, 0.106f, 0.170f),
    Color(0.110f, 0.150f, 0.230f), Color(0.100f, 0.300f, 0.400f),
    Color(0.380f, 0.780f, 0.960f), Color(0.550f, 0.860f, 1.000f),
    Color(0.250f, 0.640f, 0.900f), Color(0.380f, 0.780f, 0.960f, 0.16f),
    Color(0.350f, 0.880f, 0.780f), Color(0.980f, 0.750f, 0.250f),
    Color(0.980f, 0.420f, 0.520f), Color(0.550f, 0.700f, 1.000f),
    Color(0.030f, 0.100f, 0.160f), Color(0.035f, 0.055f, 0.100f),
    Color(0.060f, 0.090f, 0.150f), Color(0.080f, 0.120f, 0.190f),
    Color(0.100f, 0.140f, 0.220f), Color(0.120f, 0.180f, 0.270f),
    Color(0.080f, 0.120f, 0.180f), Color(0.290f, 0.400f, 0.530f),
    Color(0.900f, 0.980f, 1.000f), Color(0.100f, 0.140f, 0.220f),
    Color(0.300f, 0.420f, 0.580f), Color(0.070f, 0.110f, 0.170f, 0.98f),
};

constexpr CabbirdUiThemeColors kEmber{
    Color(1.000f, 0.950f, 0.910f), Color(0.710f, 0.630f, 0.580f),
    Color(0.090f, 0.065f, 0.050f), Color(0.140f, 0.090f, 0.070f),
    Color(0.210f, 0.130f, 0.100f), Color(0.320f, 0.220f, 0.170f),
    Color(0.140f, 0.090f, 0.070f), Color(0.210f, 0.130f, 0.100f),
    Color(0.250f, 0.150f, 0.110f), Color(0.140f, 0.090f, 0.070f),
    Color(0.210f, 0.130f, 0.100f), Color(0.360f, 0.190f, 0.130f),
    Color(0.940f, 0.480f, 0.320f), Color(1.000f, 0.620f, 0.450f),
    Color(0.780f, 0.300f, 0.200f), Color(0.940f, 0.480f, 0.320f, 0.16f),
    Color(0.400f, 0.820f, 0.630f), Color(0.960f, 0.730f, 0.350f),
    Color(1.000f, 0.400f, 0.380f), Color(0.550f, 0.700f, 0.930f),
    Color(0.170f, 0.060f, 0.030f), Color(0.065f, 0.045f, 0.035f),
    Color(0.120f, 0.075f, 0.060f), Color(0.160f, 0.100f, 0.080f),
    Color(0.190f, 0.120f, 0.095f), Color(0.230f, 0.140f, 0.105f),
    Color(0.150f, 0.090f, 0.070f), Color(0.430f, 0.300f, 0.240f),
    Color(0.990f, 0.910f, 0.860f), Color(0.170f, 0.110f, 0.085f),
    Color(0.450f, 0.300f, 0.230f), Color(0.150f, 0.095f, 0.075f, 0.98f),
};

constexpr CabbirdUiThemeColors kPaper{
    Color(0.110f, 0.160f, 0.230f), Color(0.350f, 0.420f, 0.510f),
    Color(0.940f, 0.960f, 0.980f), Color(1.000f, 1.000f, 1.000f),
    Color(1.000f, 1.000f, 1.000f), Color(0.760f, 0.810f, 0.880f),
    Color(0.970f, 0.980f, 1.000f), Color(0.900f, 0.940f, 0.990f),
    Color(0.840f, 0.900f, 0.980f), Color(0.910f, 0.940f, 0.980f),
    Color(0.850f, 0.900f, 0.970f), Color(0.790f, 0.860f, 0.950f),
    Color(0.140f, 0.390f, 0.860f), Color(0.220f, 0.490f, 0.950f),
    Color(0.100f, 0.310f, 0.730f), Color(0.140f, 0.390f, 0.860f, 0.14f),
    Color(0.120f, 0.620f, 0.420f), Color(0.780f, 0.480f, 0.050f),
    Color(0.780f, 0.190f, 0.230f), Color(0.160f, 0.400f, 0.760f),
    Color(1.000f, 1.000f, 1.000f), Color(0.880f, 0.920f, 0.970f),
    Color(0.900f, 0.940f, 0.980f), Color(0.940f, 0.960f, 0.990f),
    Color(0.980f, 0.990f, 1.000f), Color(0.910f, 0.950f, 1.000f),
    Color(0.820f, 0.870f, 0.930f), Color(0.610f, 0.680f, 0.780f),
    Color(1.000f, 1.000f, 1.000f), Color(0.900f, 0.930f, 0.980f),
    Color(0.600f, 0.680f, 0.800f), Color(0.960f, 0.970f, 1.000f, 0.98f),
};

// Retained dark Cabbird palette. A shared palette does not define the launcher's
// window frame, page structure or typography; the launcher selects its own surface.
constexpr CabbirdUiThemeColors kCabbirdHub{
    Color(0.950f, 0.950f, 0.950f), Color(0.700f, 0.700f, 0.700f),
    Color(0.106f, 0.106f, 0.106f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.350f, 0.350f, 0.350f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(1.000f, 0.639f, 0.102f), Color(1.000f, 0.725f, 0.302f),
    Color(0.820f, 0.420f, 0.020f), Color(1.000f, 0.639f, 0.102f, 0.16f),
    Color(0.360f, 0.820f, 0.560f), Color(1.000f, 0.639f, 0.102f),
    Color(0.940f, 0.320f, 0.300f), Color(0.420f, 0.660f, 0.960f),
    Color(0.106f, 0.106f, 0.106f), Color(0.106f, 0.106f, 0.106f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.161f, 0.161f, 0.161f), Color(0.161f, 0.161f, 0.161f),
    Color(0.106f, 0.106f, 0.106f), Color(0.350f, 0.350f, 0.350f),
    Color(1.000f, 0.930f, 0.800f), Color(0.161f, 0.161f, 0.161f),
    Color(0.350f, 0.350f, 0.350f), Color(0.161f, 0.161f, 0.161f, 0.98f),
};

constexpr CabbirdUiColor MixColor(
    const CabbirdUiColor& from, const CabbirdUiColor& to, const float amount) noexcept {
    return Color(
        from.red + (to.red - from.red) * amount,
        from.green + (to.green - from.green) * amount,
        from.blue + (to.blue - from.blue) * amount,
        from.alpha + (to.alpha - from.alpha) * amount);
}

/* Derive a full theme from the five user-supplied tokens.  Deriving rather than asking
 * for 32 colours means a custom palette cannot be half-specified: every slot that is not
 * an override is computed to be consistent with the ones that are. */
CabbirdUiThemeColors BuildCustomTheme(const CabbirdUiCustomColors& colors) noexcept {
    CabbirdUiThemeColors theme = kCabbirdHub;
    theme.text = colors.text;
    theme.text_muted = MixColor(colors.child_background, colors.text, 0.62f);
    theme.window_background = colors.window_background;
    theme.child_background = colors.child_background;
    theme.popup_background = colors.child_background;
    theme.border = colors.border;
    theme.frame = colors.child_background;
    theme.frame_hovered = MixColor(colors.child_background, colors.accent, 0.12f);
    theme.frame_active = MixColor(colors.child_background, colors.accent, 0.22f);
    theme.button = colors.child_background;
    theme.button_hovered = MixColor(colors.child_background, colors.accent, 0.12f);
    theme.button_active = MixColor(colors.child_background, colors.accent, 0.22f);
    theme.accent = colors.accent;
    theme.accent_hovered = MixColor(colors.accent, Color(1.0f, 1.0f, 1.0f), 0.18f);
    theme.accent_active = MixColor(colors.accent, Color(0.0f, 0.0f, 0.0f), 0.18f);
    theme.accent_soft = Color(colors.accent.red, colors.accent.green, colors.accent.blue, 0.16f);
    // Pick the inverse text colour from the accent's luminance so a light accent does not
    // get white-on-white labels.  Rec. 709 weights, with the threshold Anomaly uses.
    const float accent_luminance = 0.2126f * colors.accent.red +
        0.7152f * colors.accent.green + 0.0722f * colors.accent.blue;
    theme.inverse_text = accent_luminance >= 0.58f
        ? Color(0.06f, 0.06f, 0.06f) : Color(0.96f, 0.96f, 0.96f);
    theme.navigation_background = colors.window_background;
    theme.header_background = colors.child_background;
    theme.toolbar_background = colors.child_background;
    theme.panel_background = colors.child_background;
    theme.row_hovered = MixColor(colors.child_background, colors.text, 0.08f);
    theme.toggle_off = colors.window_background;
    theme.toggle_off_border = colors.border;
    theme.toggle_on_knob = colors.text;
    theme.icon_fill = colors.child_background;
    theme.icon_border = colors.border;
    theme.toast_background = Color(colors.child_background.red,
        colors.child_background.green, colors.child_background.blue, 0.98f);
    return theme;
}

CabbirdUiCustomColors g_custom_colors;
CabbirdUiThemeColors g_custom_theme = BuildCustomTheme(g_custom_colors);
std::atomic<CabbirdUiPalette> g_palette{CabbirdUiPalette::Paper};
std::atomic<float> g_surface_alpha{1.0f};
CabbirdUiThemeColors g_surface_theme = kPaper;
std::filesystem::path g_runtime_root;

const CabbirdUiThemeColors& BaseTheme() noexcept {
    switch (g_palette.load(std::memory_order_relaxed)) {
        case CabbirdUiPalette::Aurora: return kAurora;
        case CabbirdUiPalette::Ember: return kEmber;
        case CabbirdUiPalette::Paper: return kPaper;
        case CabbirdUiPalette::Custom: return g_custom_theme;
        case CabbirdUiPalette::Moss: return kMoss;
        case CabbirdUiPalette::CabbirdHub: return kCabbirdHub;
    }
    return kPaper;
}

void RefreshSurfaceTheme() noexcept {
    g_surface_theme = BaseTheme();
    const float alpha = g_surface_alpha.load(std::memory_order_relaxed);
    for (CabbirdUiColor* color : {&g_surface_theme.window_background,
             &g_surface_theme.child_background, &g_surface_theme.navigation_background,
             &g_surface_theme.header_background, &g_surface_theme.toolbar_background,
             &g_surface_theme.panel_background}) {
        color->alpha *= alpha;
    }
}

/* ---------------------------------------------------------------------------
 * STATIC-TLS RULE FOR THIS FILE -- read before adding a function-local static.
 *
 * This translation unit is linked into Cabbird.Core.dll, which is manually mapped.
 * A mapped image must have NO static TLS directory: there is no loader to fill the
 * per-thread slot, and the mapper rejects such an image outright.  That failure is
 * all-or-nothing and completely silent at the source
 * level -- the whole overlay simply never initialises.
 *
 * The trap: MSVC implements function-local statics with non-trivial destructors using
 * "magic statics", whose thread-safe initialisation guard lives in TLS (`_tls_index` via
 * __Init_thread_header).  So
 *
 *     void F() { static std::mutex m; ... }          // ADDS 8 BYTES OF STATIC TLS
 *
 * while a namespace-scope equivalent does not:
 *
 *     std::mutex g_m;  void F() { ... g_m ... }      // no TLS
 *
 * Namespace-scope statics are safe for a manually mapped image because the mapper calls
 * the PE entry point (OptionalHeader.AddressOfEntryPoint), which for a /MT DLL is
 * _DllMainCRTStartup, which runs _initterm over .CRT$XC* before CabbirdStart.  That is
 * why the cache below is at namespace scope and not inside the function that uses it.
 *
 * This was not a hypothetical: the first version of this file had `static std::mutex` and
 * `static std::map` inside CachedFontBytes(), and it added exactly 8 bytes of static TLS,
 * which failed the mapper and took the offline overlay regression from 87/87 to 8/59.
 * --------------------------------------------------------------------------- */
std::mutex g_font_cache_mutex;
std::map<std::filesystem::path, std::unique_ptr<std::vector<unsigned char>>> g_font_cache;

ImVector<ImWchar> g_chinese_host_ranges;
ImVector<ImWchar> g_chinese_own_ranges;
ImVector<ImWchar> g_icon_glyph_ranges;
constexpr ImWchar kGeneralPunctuation[] = {0x2000, 0x206F, 0};

constexpr float kFontSizePixels = 13.0f;
constexpr std::array kFontBakeScales{1.00f, 1.25f, 1.50f, 1.75f, 2.00f};
std::vector<float> g_font_bake_scales{kFontBakeScales.begin(), kFontBakeScales.end()};
constexpr std::string_view kFontNamePrefix{"Cabbird host "};

const std::vector<unsigned char>* CachedFontBytes(
    const std::filesystem::path& path, const bool allow_read = false) noexcept {
    try {
        {
            std::scoped_lock lock(g_font_cache_mutex);
            if (const auto found = g_font_cache.find(path); found != g_font_cache.end()) {
                return found->second.get();
            }
        }
        if (!allow_read) return nullptr;
        // Worker loads outside the cache lock. Render only ever looks up
        // immutable, prepared sources and cannot wait for disk access.
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return nullptr;
        }
        auto bytes = std::make_unique<std::vector<unsigned char>>(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (bytes->empty() ||
            bytes->size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return nullptr;
        }
        std::scoped_lock lock(g_font_cache_mutex);
        const auto [entry, inserted] = g_font_cache.try_emplace(path, std::move(bytes));
        return entry->second.get();
    } catch (...) {
        return nullptr;
    }
}

ImFont* AddCachedFontFromPath(
    ImFontAtlas& atlas,
    const std::filesystem::path& path,
    const float size_pixels,
    ImFontConfig configuration,
    const ImWchar* glyph_ranges) noexcept {
    try {
        const auto* bytes = CachedFontBytes(path);
        if (bytes == nullptr) {
            return nullptr;
        }
        // The cache owns the bytes for the process lifetime, so the atlas must not free
        // them.  Getting this backwards is a double free at shutdown.
        configuration.FontDataOwnedByAtlas = false;
        return atlas.AddFontFromMemoryTTF(const_cast<unsigned char*>(bytes->data()),
            static_cast<int>(bytes->size()), size_pixels, &configuration, glyph_ranges);
    } catch (...) {
        return nullptr;
    }
}

void SetFontName(ImFontConfig& configuration, const float scale) noexcept {
    std::snprintf(configuration.Name, std::size(configuration.Name), "%.*s%.2f",
        static_cast<int>(kFontNamePrefix.size()), kFontNamePrefix.data(), scale);
}

bool IsCabbirdFont(const ImFont& font, const float scale) noexcept {
    char expected[40]{};
    std::snprintf(expected, std::size(expected), "%.*s%.2f",
        static_cast<int>(kFontNamePrefix.size()), kFontNamePrefix.data(), scale);
    return std::strcmp(font.GetDebugName(), expected) == 0;
}

const ImWchar* ChineseGlyphRanges(ImFontAtlas& atlas, const bool symbol_blocks) noexcept {
    static constexpr ImWchar symbols[]{
        0x2000, 0x206F, 0x2190, 0x21FF, 0x2460, 0x24FF,
        0x2500, 0x257F, 0x25A0, 0x25FF, 0x2600, 0x26FF, 0x2700, 0x27BF, 0};
    static constexpr ImWchar additional_ideographs[]{0x5F02, 0x73AF, 0x7570, 0x74B0};
    // Atlas sources retain these range pointers until their next rebuild.
    auto& ranges = symbol_blocks ? g_chinese_host_ranges : g_chinese_own_ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(atlas.GetGlyphRangesChineseSimplifiedCommon());
        builder.AddRanges(symbol_blocks ? symbols : kGeneralPunctuation);
        ImWchar codepoint = 0x4E00;
        for (const ImWchar offset : kCabbirdGb2312IdeographOffsets) {
            codepoint = static_cast<ImWchar>(codepoint + offset);
            builder.AddChar(codepoint);
        }
        for (const ImWchar ideograph : additional_ideographs) builder.AddChar(ideograph);
        builder.BuildRanges(&ranges);
    }
    return ranges.Data;
}

const ImWchar* IconGlyphRanges(const std::span<const char32_t> icons) {
    static constexpr ImWchar full_range[]{0xE700, 0xF8FF, 0};
    if (icons.empty()) return full_range;
    std::vector<ImWchar> glyphs;
    glyphs.reserve(icons.size());
    for (const char32_t glyph : icons)
        if (glyph > 0 && glyph <= 0xFFFF) glyphs.push_back(static_cast<ImWchar>(glyph));
    std::sort(glyphs.begin(), glyphs.end());
    glyphs.erase(std::unique(glyphs.begin(), glyphs.end()), glyphs.end());
    g_icon_glyph_ranges.clear();
    for (const ImWchar glyph : glyphs) {
        g_icon_glyph_ranges.push_back(glyph);
        g_icon_glyph_ranges.push_back(glyph);
    }
    g_icon_glyph_ranges.push_back(0);
    return g_icon_glyph_ranges.Data;
}

}  // namespace

void SetCabbirdUiPalette(const CabbirdUiPalette palette) noexcept {
    g_palette.store(palette, std::memory_order_relaxed);
    RefreshSurfaceTheme();
}

CabbirdUiPalette GetCabbirdUiPalette() noexcept {
    return g_palette.load(std::memory_order_relaxed);
}

void SetCabbirdUiCustomColors(const CabbirdUiCustomColors& colors) noexcept {
    g_custom_colors = colors;
    g_custom_theme = BuildCustomTheme(colors);
    RefreshSurfaceTheme();
}

const CabbirdUiCustomColors& GetCabbirdUiCustomColors() noexcept {
    return g_custom_colors;
}

const CabbirdUiThemeColors& CabbirdUiTheme() noexcept {
    return g_surface_theme;
}

void SetCabbirdUiSurfaceAlpha(const float alpha) noexcept {
    g_surface_alpha.store(std::isfinite(alpha)
        ? std::clamp(alpha, kCabbirdUiSurfaceAlphaMinimum, 1.0f) : 1.0f,
        std::memory_order_relaxed);
    RefreshSurfaceTheme();
}

float GetCabbirdUiSurfaceAlpha() noexcept {
    return g_surface_alpha.load(std::memory_order_relaxed);
}

CabbirdUiColor CabbirdUiSurfaceColor() noexcept {
    auto color = BaseTheme().window_background;
    color.alpha = 1.0f;
    return color;
}

CabbirdUiPalette ParseCabbirdUiPalette(const std::string_view value) noexcept {
    // Unknown or removed preset names use Paper; other settings remain intact.
    if (value == "moss") return CabbirdUiPalette::Moss;
    if (value == "aurora") return CabbirdUiPalette::Aurora;
    if (value == "ember") return CabbirdUiPalette::Ember;
    if (value == "paper") return CabbirdUiPalette::Paper;
    if (value == "cabbirdhub") return CabbirdUiPalette::CabbirdHub;
    if (value == "custom") return CabbirdUiPalette::Custom;
    return CabbirdUiPalette::Paper;
}

std::string_view ToString(const CabbirdUiPalette palette) noexcept {
    switch (palette) {
        case CabbirdUiPalette::Moss: return "moss";
        case CabbirdUiPalette::Aurora: return "aurora";
        case CabbirdUiPalette::Ember: return "ember";
        case CabbirdUiPalette::Paper: return "paper";
        case CabbirdUiPalette::Custom: return "custom";
        case CabbirdUiPalette::CabbirdHub: return "cabbirdhub";
    }
    return "paper";
}

void SetCabbirdUiRuntimeRoot(std::filesystem::path root) noexcept {
    g_runtime_root = std::move(root);
}

const std::filesystem::path& CabbirdUiRuntimeRoot() noexcept {
    return g_runtime_root;
}

namespace {

ImVec4 ImGuiColor(const CabbirdUiColor& color) noexcept {
    return {color.red, color.green, color.blue, color.alpha};
}

}  // namespace

void ApplyCabbirdUiStyle() noexcept {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle{};
    const float scale = std::isfinite(ImGui::GetIO().FontGlobalScale) &&
            ImGui::GetIO().FontGlobalScale > 0.0f
        ? ImGui::GetIO().FontGlobalScale
        : 1.0f;
    const CabbirdUiThemeColors& theme = CabbirdUiTheme();
    // Liquid glass geometry: continuous, generous corners. Panels draw their
    // own material, so ImGui's child and frame borders are off and every frame
    // is a soft neutral fill derived from the text colour instead. That keeps a
    // field legible on any palette and on any glass tint without a hard outline.
    const bool glass = cabbird::glass::Active();
    style.WindowRounding = 18.0f;
    style.ChildRounding = 14.0f;
    style.FrameRounding = 9.0f;
    style.PopupRounding = 14.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 9.0f;
    style.TabRounding = 9.0f;
    style.WindowBorderSize = glass ? 0.0f : 1.0f;
    style.PopupBorderSize = glass ? 0.0f : 1.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 0.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.WindowPadding = ImVec2(14.0f, 14.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 5.0f);
    style.CellPadding = ImVec2(8.0f, 5.0f);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 10.0f;
    style.GrabMinSize = 12.0f;
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    const auto ink = [&theme](const float alpha) {
        return ImVec4(theme.text.red, theme.text.green, theme.text.blue, alpha);
    };
    const auto accent = [&theme](const float alpha) {
        return ImVec4(theme.accent.red, theme.accent.green, theme.accent.blue, alpha);
    };
    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = ImGuiColor(theme.text);
    colors[ImGuiCol_TextDisabled] = ImGuiColor(theme.text_muted);
    colors[ImGuiCol_WindowBg] = ImGuiColor(theme.window_background);
    colors[ImGuiCol_ChildBg] = ImGuiColor(theme.child_background);
    // Ordinary popups remain readable and opaque; a local glass surface may
    // opt out explicitly, rather than making every menu reveal the game.
    colors[ImGuiCol_PopupBg] = ImVec4(theme.popup_background.red,
        theme.popup_background.green, theme.popup_background.blue, 1.0f);
    colors[ImGuiCol_Border] = ink(0.12f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_FrameBg] = ink(0.075f);
    colors[ImGuiCol_FrameBgHovered] = ink(0.075f);
    colors[ImGuiCol_FrameBgActive] = ink(0.15f);
    colors[ImGuiCol_TitleBg] = ImGuiColor(theme.navigation_background);
    colors[ImGuiCol_TitleBgActive] = ImGuiColor(theme.window_background);
    colors[ImGuiCol_TitleBgCollapsed] = ImGuiColor(theme.navigation_background);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_Button] = ink(0.085f);
    colors[ImGuiCol_ButtonHovered] = ink(0.085f);
    colors[ImGuiCol_ButtonActive] = ink(0.18f);
    colors[ImGuiCol_Header] = accent(0.20f);
    // Hover never changes a background: on glass any fill under the pointer
    // reads as a shadow. Controls signal hover through their label instead.
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_HeaderActive] = accent(0.30f);
    colors[ImGuiCol_CheckMark] = ImGuiColor(theme.accent);
    colors[ImGuiCol_SliderGrab] = ImGuiColor(theme.accent);
    colors[ImGuiCol_SliderGrabActive] = ImGuiColor(theme.accent_active);
    colors[ImGuiCol_Separator] = ink(0.10f);
    colors[ImGuiCol_SeparatorHovered] = accent(0.55f);
    colors[ImGuiCol_SeparatorActive] = accent(0.85f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_ScrollbarGrab] = ink(0.20f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ink(0.30f);
    colors[ImGuiCol_ScrollbarGrabActive] = ink(0.40f);
    colors[ImGuiCol_ResizeGrip] = accent(0.0f);
    colors[ImGuiCol_ResizeGripHovered] = accent(0.45f);
    colors[ImGuiCol_ResizeGripActive] = accent(0.80f);
    colors[ImGuiCol_Tab] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TabSelected] = ink(0.11f);
    colors[ImGuiCol_TabSelectedOverline] = ImGuiColor(theme.accent);
    colors[ImGuiCol_TabDimmed] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TabDimmedSelected] = ink(0.08f);
    colors[ImGuiCol_TableHeaderBg] = ink(0.05f);
    colors[ImGuiCol_TableBorderStrong] = ink(0.12f);
    colors[ImGuiCol_TableBorderLight] = ink(0.07f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = ink(0.03f);
    colors[ImGuiCol_TextLink] = ImGuiColor(theme.info);
    colors[ImGuiCol_TextSelectedBg] = accent(0.32f);
    colors[ImGuiCol_NavCursor] = ImGuiColor(theme.accent);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.22f);
    colors[ImGuiCol_PlotLines] = ImGuiColor(theme.accent);
    colors[ImGuiCol_PlotHistogram] = ImGuiColor(theme.accent);
    style.ScaleAllSizes(scale);
}

bool ConfigureCabbirdUiFontAtlas(
    const std::filesystem::path& runtime_root) noexcept {
    return ConfigureCabbirdUiFontAtlas(runtime_root, CabbirdUiFontBake{});
}

bool PrepareCabbirdUiFontSources(const std::filesystem::path& runtime_root) noexcept {
    try {
        const auto& root = runtime_root.empty() ? g_runtime_root : runtime_root;
        const bool latin = CachedFontBytes(L"C:\\Windows\\Fonts\\segoeui.ttf", true) != nullptr;
        const bool icons = CachedFontBytes(L"C:\\Windows\\Fonts\\segmdl2.ttf", true) != nullptr;
        const bool cjk = !root.empty() && CachedFontBytes(
            root / L"assets" / L"fonts" / L"NotoSansCJKsc-Regular.ttf", true) != nullptr;
        return latin && icons && cjk;
    } catch (...) {
        return false;
    }
}

bool ConfigureCabbirdUiFontAtlas(
    const std::filesystem::path& runtime_root, const CabbirdUiFontBake& bake) noexcept {
    static_cast<void>(PrepareCabbirdUiFontSources(runtime_root));
    return ConfigureCabbirdUiFontAtlasFromMemory(runtime_root, bake);
}

bool ConfigureCabbirdUiFontAtlasFromMemory(
    const std::filesystem::path& runtime_root, const CabbirdUiFontBake& bake) noexcept {
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) {
        return false;
    }
    io.FontDefault = nullptr;
    io.Fonts->Clear();
    if (bake.tight_atlas) io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    else io.Fonts->Flags &= ~ImFontAtlasFlags_NoPowerOfTwoHeight;
    const auto scales = bake.scales.empty() ? std::span<const float>(kFontBakeScales) : bake.scales;
    g_font_bake_scales.assign(scales.begin(), scales.end());
    ImFontConfig cjk_configuration;
    const ImWchar* const icon_range = IconGlyphRanges(bake.icons);
    bool complete = true;
    for (const float scale : scales) {
        ImFontConfig configuration;
        configuration.OversampleH = 2;
        configuration.OversampleV = 1;
        SetFontName(configuration, scale);
        ImFont* const font = AddCachedFontFromPath(*io.Fonts,
            L"C:\\Windows\\Fonts\\segoeui.ttf",
            kFontSizePixels * scale, configuration, nullptr);
        if (io.FontDefault == nullptr && font != nullptr) {
            io.FontDefault = font;
        }
        if (font == nullptr) {
            complete = false;
            continue;
        }

        cjk_configuration = {};
        cjk_configuration.MergeMode = true;
        cjk_configuration.PixelSnapH = true;
        // Merge the CJK font so Latin stays Segoe UI while Chinese renders properly.
        // A failure here is reported, not ignored: it is the difference between Chinese
        // labels and a wall of '?' box glyphs.
        // Anomaly takes the root as a parameter and Cabbird used to read a global the
        // injector pushed in; both are supported, the parameter first.  The parameter is
        // the one that matters on the platform-host path, where nothing has pushed a
        // global -- reading only the global there silently dropped the CJK merge and
        // turned every Chinese label into box glyphs.
        const std::filesystem::path& effective_root =
            runtime_root.empty() ? g_runtime_root : runtime_root;
        const std::filesystem::path cjk_path =
            effective_root.empty()
                ? std::filesystem::path{}
                : effective_root / L"assets" / L"fonts" / L"NotoSansCJKsc-Regular.ttf";
        if (cjk_path.empty() ||
            AddCachedFontFromPath(*io.Fonts, cjk_path, kFontSizePixels * scale,
                cjk_configuration, ChineseGlyphRanges(*io.Fonts, bake.symbol_blocks)) == nullptr) {
            complete = false;
        }

        ImFontConfig icon_configuration;
        icon_configuration.MergeMode = true;
        icon_configuration.PixelSnapH = true;
        icon_configuration.GlyphOffset = ImVec2(0.0f, 2.0f * scale);
        icon_configuration.GlyphMinAdvanceX = kFontSizePixels * scale;
        if (AddCachedFontFromPath(*io.Fonts,
                L"C:\\Windows\\Fonts\\segmdl2.ttf", 14.0f * scale,
                icon_configuration, icon_range) == nullptr) {
            complete = false;
        }
    }
    if (io.FontDefault == nullptr) {
        io.FontDefault = io.Fonts->AddFontDefault();
        complete = false;
    }
    const bool scaled = ApplyCabbirdUiFontScale(1.0f);
    return complete && scaled;
}

bool ApplyCabbirdUiFontScale(const float scale) noexcept {
    if (!std::isfinite(scale) || scale <= 0.0f) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) {
        return false;
    }
    // Pick the smallest baked size that is at least the requested scale: scaling a bitmap
    // font down looks better than scaling it up.
    float selected_scale = g_font_bake_scales.back();
    for (const float baked_scale : g_font_bake_scales) {
        if (baked_scale >= scale) {
            selected_scale = baked_scale;
            break;
        }
    }
    ImFont* selected = io.FontDefault != nullptr &&
            IsCabbirdFont(*io.FontDefault, selected_scale)
        ? io.FontDefault
        : nullptr;
    if (selected == nullptr) {
        for (ImFont* const font : io.Fonts->Fonts) {
            if (font != nullptr && IsCabbirdFont(*font, selected_scale)) {
                selected = font;
                break;
            }
        }
    }
    if (selected == nullptr) {
        return false;
    }
    io.FontDefault = selected;
    selected->Scale = selected->FontSize > 0.0f
        ? kFontSizePixels / selected->FontSize
        : 1.0f / selected_scale;
    io.FontGlobalScale = scale;
    ApplyCabbirdUiStyle();
    return true;
}

ImFont* CabbirdUiFontForPixelSize(const float size_pixels) noexcept {
    // WHY THIS EXISTS: ImDrawList::AddText(font, font_size, ...) renders `font`'s RASTERISED
    // glyphs scaled by `font_size / font->FontSize`.  ImGui has no vector text, so passing a
    // `font_size` larger than the bake it was rasterised at MAGNIFIES A BITMAP -- and bitmap
    // magnification of CJK, whose strokes are already one or two pixels wide at 13px, does not
    // look like "bigger text", it looks like "blurry text".  That was the reported symptom.
    //
    // The atlas already holds five bakes of the same two font files (Latin + merged CJK):
    // kFontBakeScales * kFontSizePixels = 13, 16.25, 19.5, 22.75 and 26 pixels.  So ANY request
    // up to 26px is answered by the smallest bake that is at least that large and drawn with a
    // ratio <= 1.0 -- a DOWNSCALE, which stays crisp.  Only a request beyond the largest bake
    // has to magnify, which is why the caller's range stops at 2.0.
    //
    // Rounding UP is the whole trick: 20.8px answered by the 22.75px bake is a 0.914 ratio.
    // Answering it with the 19.5px bake would be a 1.067 magnification and still soft.
    if (!std::isfinite(size_pixels) || size_pixels <= 0.0f) {
        return nullptr;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) {
        return nullptr;
    }
    const float requested_scale = size_pixels / kFontSizePixels;
    float selected_scale = g_font_bake_scales.back();
    for (const float baked_scale : g_font_bake_scales) {
        if (baked_scale >= requested_scale) {
            selected_scale = baked_scale;
            break;
        }
    }
    for (ImFont* const font : io.Fonts->Fonts) {
        if (font != nullptr && IsCabbirdFont(*font, selected_scale)) {
            return font;
        }
    }
    // No bake matched -- the atlas was not configured by `ConfigureCabbirdUiFontAtlas`, or the
    // CJK merge failed so the font carries a different name.  Returning null is deliberate: the
    // caller then keeps its current font, which is the behaviour that shipped before this.
    return nullptr;
}

float CabbirdUiBaseFontSize() noexcept { return kFontSizePixels; }

ImFont* CabbirdUiFontForScale(const float scale) noexcept {
    if (!std::isfinite(scale) || scale <= 0.0f || ImGui::GetCurrentContext() == nullptr)
        return nullptr;
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts == nullptr) return nullptr;
    ImFont* best = nullptr;
    ImFont* largest = nullptr;
    for (const float baked : g_font_bake_scales) {
        for (ImFont* const font : io.Fonts->Fonts) {
            if (font == nullptr || !IsCabbirdFont(*font, baked)) continue;
            if (largest == nullptr || font->FontSize > largest->FontSize) largest = font;
            if (baked >= scale - 0.001f && (best == nullptr || font->FontSize < best->FontSize))
                best = font;
            break;
        }
    }
    return best != nullptr ? best : largest != nullptr ? largest : io.FontDefault;
}

void ReleaseCabbirdUiFontAtlas(const CabbirdUiFontRetention retention) noexcept {
    if (ImGui::GetCurrentContext() == nullptr || ImGui::GetIO().Fonts == nullptr) return;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->ClearTexData();
    if (retention != CabbirdUiFontRetention::Minimal) return;
    io.Fonts->ClearInputData();
    // Minimal is restricted to the sole font-context owner (the launcher).
    // Its borrowed CJK sources can now leave the shared file cache as well.
    std::scoped_lock lock(g_font_cache_mutex);
    g_font_cache.clear();
}

}  // namespace cabbird
