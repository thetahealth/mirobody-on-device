package ai.thetahealth.mirobody.ui.theme

import androidx.compose.ui.graphics.Color

// Brand navy (mirobody / Theta Health) — independent of M3 dynamicColor so it stays
// stable across devices and wallpapers, matching the web client's brand color.
val BrandBlue = Color(0xFF1E3A6B)

// Light scheme — the "Theta Health" design (mirrors the web client htdoc): a deep
// navy primary on warm cream surfaces with warm hairlines.
val LightPrimary = Color(0xFF1E3A6B)            // deep navy: primary buttons, links, logo
val LightOnPrimary = Color(0xFFFFFFFF)
val LightPrimaryContainer = Color(0xFFDCE4F4)
val LightOnPrimaryContainer = Color(0xFF0A1B3D)
val LightSecondary = Color(0xFF4A5568)
val LightOnSecondary = Color(0xFFFFFFFF)
val LightSecondaryContainer = Color(0xFFDDE2EA)
val LightOnSecondaryContainer = Color(0xFF161A22)
val LightBackground = Color(0xFFF2EFE9)         // warm cream page background
val LightOnBackground = Color(0xFF1A1C1E)
val LightSurface = Color(0xFFF2EFE9)
val LightOnSurface = Color(0xFF1A1C1E)           // near-black text
val LightSurfaceVariant = Color(0xFFE7E1D5)
val LightOnSurfaceVariant = Color(0xFF52565C)    // secondary text (subtitle, hints)
val LightSurfaceContainerLowest = Color(0xFFFFFFFF)
val LightSurfaceContainerLow = Color(0xFFFAF7F1) // field / card fill (slightly lighter than bg)
val LightSurfaceContainer = Color(0xFFF4F0E8)
val LightSurfaceContainerHigh = Color(0xFFEDE8DE)
val LightSurfaceContainerHighest = Color(0xFFE7E1D5)
val LightOutline = Color(0xFF74787C)
val LightOutlineVariant = Color(0xFFDDD6C9)       // warm hairlines / idle field borders

// Dark scheme — near-black surfaces, soft slate-blue accent.
val DarkPrimary = Color(0xFFA0CDE5)
val DarkOnPrimary = Color(0xFF003549)
val DarkPrimaryContainer = Color(0xFF184D67)
val DarkOnPrimaryContainer = Color(0xFFCFE5F2)
val DarkSecondary = Color(0xFFB7C9D4)
val DarkOnSecondary = Color(0xFF21323B)
val DarkSecondaryContainer = Color(0xFF384952)
val DarkOnSecondaryContainer = Color(0xFFD3E5F0)
val DarkBackground = Color(0xFF101315)
val DarkOnBackground = Color(0xFFE2E2E5)
val DarkSurface = Color(0xFF101315)
val DarkOnSurface = Color(0xFFE2E2E5)
val DarkSurfaceVariant = Color(0xFF44474A)
val DarkOnSurfaceVariant = Color(0xFFC4C7CB)
val DarkSurfaceContainerLowest = Color(0xFF0A0D0F)
val DarkSurfaceContainerLow = Color(0xFF181B1D)
val DarkSurfaceContainer = Color(0xFF1C1F22)
val DarkSurfaceContainerHigh = Color(0xFF272A2D)
val DarkSurfaceContainerHighest = Color(0xFF313437)
val DarkOutline = Color(0xFF8E9194)
val DarkOutlineVariant = Color(0xFF44474A)
