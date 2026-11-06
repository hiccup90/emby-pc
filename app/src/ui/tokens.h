#pragma once

#include <QString>

// T43：qEmby 风格视觉 token 的唯一代码来源。
// QSS 无法直接引用 C++ 常量，dark.qss 中的数值必须与本文件保持一致；
// 修改 token 时请同步 docs/qemby-ui-reference.md。
namespace ui::tokens {

// ---- 颜色（Tailwind Slate / qEmby dark-style.qss）----
inline constexpr auto ColorBg = "#0F172A";            // Slate-900
inline constexpr auto ColorSurface = "#1E293B";       // Slate-800（侧边栏/卡片/输入）
inline constexpr auto ColorSurfaceHover = "#334155";  // Slate-700
inline constexpr auto ColorSurfacePressed = "#475569";// Slate-600
inline constexpr auto ColorBorder = "#334155";
inline constexpr auto ColorBorderStrong = "#475569";
inline constexpr auto ColorTextPrimary = "#F8FAFC";   // Slate-50
inline constexpr auto ColorTextBody = "#CBD5E1";      // Slate-300
inline constexpr auto ColorTextSecondary = "#94A3B8"; // Slate-400
inline constexpr auto ColorTextDisabled = "#64748B";  // Slate-500
inline constexpr auto ColorAccent = "#3B82F6";        // Blue-500
inline constexpr auto ColorAccentHover = "#2563EB";   // Blue-600
inline constexpr auto ColorAccentPressed = "#1D4ED8"; // Blue-700
inline constexpr auto ColorAccentLight = "#60A5FA";   // Blue-400
inline constexpr auto ColorFocus = "#60A5FA";
inline constexpr auto ColorSuccess = "#22C55E";
inline constexpr auto ColorDanger = "#EF4444";
inline constexpr auto ColorDangerHover = "#DC2626";

// ---- 间距（4/8 网格）----
inline constexpr int Space1 = 4;
inline constexpr int Space2 = 8;
inline constexpr int Space3 = 12;
inline constexpr int Space4 = 16;
inline constexpr int Space5 = 24;
inline constexpr int Space6 = 32;
inline constexpr int Space7 = 48;

inline constexpr int PageGutter = Space6;       // 页面左右统一 32
inline constexpr int SectionGap = Space5;       // 首页 section 间距 24
inline constexpr int CardGap = Space4;          // 卡片之间 16
inline constexpr int CardPadding = Space3;      // 卡片内边距 12

// ---- 字号 / 字重 ----
inline constexpr int FontDisplay = 28;
inline constexpr int FontH1 = 24;
inline constexpr int FontH2 = 20;
inline constexpr int FontH3 = 16;
inline constexpr int FontBody = 14;
inline constexpr int FontBodySmall = 13;
inline constexpr int FontCaption = 12;
inline constexpr int FontMicro = 11;

// ---- 圆角 ----
inline constexpr int RadiusXs = 4;
inline constexpr int RadiusSm = 6;
inline constexpr int RadiusMd = 8;
inline constexpr int RadiusLg = 10;
inline constexpr int RadiusXl = 12;
inline constexpr int RadiusPill = 999;

// ---- 框架尺寸 ----
inline constexpr int TopBarHeight = 56;
inline constexpr int TopBarHorizontalPadding = Space6;
inline constexpr int SidebarDefaultWidth = 240;
inline constexpr int SidebarMinWidth = 180;
inline constexpr int SidebarMaxWidth = 320;
inline constexpr int ControlHeight = 36;
inline constexpr int IconSize = 18;
inline constexpr int IconSizeLarge = 24;

// ---- 常用尺寸 ----
inline constexpr int PosterCardWidth = 168;
inline constexpr int EpisodeThumbWidth = 120;
inline constexpr int EpisodeThumbHeight = 68;
inline constexpr int ScrollBarWidth = 10;

} // namespace ui::tokens
