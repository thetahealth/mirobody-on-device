import SwiftUI

/// Colors + typography — the iOS analogue of `ui/theme/{Color,Theme,Type}.kt`.
/// Hex values are lifted verbatim from `Color.kt`. SwiftUI gives us the current
/// `ColorScheme`; `MBColors` picks the light/dark variant. Type sizes come from
/// `Type.kt` and are scaled at render time by the font-size offset.

extension Color {
    init(rgb: UInt32) {
        self.init(
            .sRGB,
            red: Double((rgb >> 16) & 0xFF) / 255,
            green: Double((rgb >> 8) & 0xFF) / 255,
            blue: Double(rgb & 0xFF) / 255,
            opacity: 1
        )
    }
}

/// Brand blue (mirobody icon). Stable across light/dark.
let brandBlue = Color(rgb: 0x3A78B5)

/// Resolves the app palette for the active color scheme.
struct MBColors {
    let scheme: ColorScheme
    private func pick(_ light: UInt32, _ dark: UInt32) -> Color {
        Color(rgb: scheme == .dark ? dark : light)
    }

    var primary: Color              { pick(0x2F5E78, 0xA0CDE5) }
    var onPrimary: Color            { pick(0xFFFFFF, 0x003549) }
    var primaryContainer: Color     { pick(0xCFE5F2, 0x184D67) }
    var background: Color           { pick(0xFBFCFD, 0x101315) }
    var onBackground: Color         { pick(0x1A1C1E, 0xE2E2E5) }
    var surface: Color              { pick(0xFBFCFD, 0x101315) }
    var onSurface: Color            { pick(0x1A1C1E, 0xE2E2E5) }
    var onSurfaceVariant: Color     { pick(0x44474A, 0xC4C7CB) }
    var surfaceContainerLowest: Color { pick(0xFFFFFF, 0x0A0D0F) }
    var surfaceContainerLow: Color  { pick(0xF4F6F8, 0x181B1D) }
    var outline: Color              { pick(0x74787C, 0x8E9194) }
    var outlineVariant: Color       { pick(0xC4C7CB, 0x44474A) }
    // Not in Color.kt (Android used the M3 default); standard M3 error tones.
    var error: Color                { pick(0xBA1A1A, 0xFFB4AB) }
}

extension EnvironmentValues {
    /// Convenience: `@Environment(\.mbColors) var colors` re-resolves with the scheme.
    var mbColors: MBColors { MBColors(scheme: colorScheme) }
}

// MARK: - Font-size scaling (mirrors MainActivity's fontScale * (1 + offset/14))

private struct MBFontScaleKey: EnvironmentKey { static let defaultValue: CGFloat = 1 }
extension EnvironmentValues {
    var mbFontScale: CGFloat {
        get { self[MBFontScaleKey.self] }
        set { self[MBFontScaleKey.self] = newValue }
    }
}

/// Maps a font-size offset (in px relative to a 14sp body) to a scale multiplier.
func fontScale(forOffset offset: Int) -> CGFloat { 1 + CGFloat(offset) / 14 }

// MARK: - Type scale (mirrors Type.kt)

enum MBTextStyle {
    case headlineMedium, headlineSmall
    case titleMedium, titleSmall
    case bodyLarge, bodyMedium, bodySmall
    case labelLarge, labelMedium, labelSmall

    var size: CGFloat {
        switch self {
        case .headlineMedium: return 22
        case .headlineSmall:  return 20
        case .titleMedium:    return 16
        case .titleSmall:     return 14
        case .bodyLarge:      return 16
        case .bodyMedium:     return 14
        case .bodySmall:      return 12
        case .labelLarge:     return 14
        case .labelMedium:    return 12
        case .labelSmall:     return 11
        }
    }
    var weight: Font.Weight {
        switch self {
        case .headlineMedium, .headlineSmall: return .semibold
        case .titleMedium, .titleSmall, .labelLarge, .labelMedium, .labelSmall: return .medium
        case .bodyLarge, .bodyMedium, .bodySmall: return .regular
        }
    }
}

private struct MBFontModifier: ViewModifier {
    let style: MBTextStyle
    let monospaced: Bool
    @Environment(\.mbFontScale) private var scale
    func body(content: Content) -> some View {
        let font = monospaced
            ? Font.system(size: style.size * scale, weight: style.weight, design: .monospaced)
            : Font.system(size: style.size * scale, weight: style.weight)
        content.font(font)
    }
}

extension View {
    /// Applies a type-scale style, scaled by the current font-size offset.
    func mbFont(_ style: MBTextStyle, monospaced: Bool = false) -> some View {
        modifier(MBFontModifier(style: style, monospaced: monospaced))
    }
}
