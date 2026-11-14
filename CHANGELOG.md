# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/) and the project uses
[Semantic Versioning](https://semver.org/).

## [1.1.0] - 2026-11-14

### Added
- **Edit profiles** selector: pick the global profile or any installed game (Steam and non-Steam)
  and set up its profile and launch options without launching it. Games with their own profile
  are marked with ★ and listed first. The running game is selected automatically.
- Per-game switch **Hide the game's native HDR**. Some games (Elden Ring, Nightreign and other
  FromSoftware titles) turn on their own HDR as soon as the display is in HDR. It stacks on top of
  AutoHDR and makes colors look washed out. The switch adds `DXVK_HDR=0` to the game's Steam launch
  options so the game sees an SDR display, and removes it when turned off. Existing launch options
  are kept. Available in all 10 languages.

## [1.0.0] - 2026-11-11

First public release.

### Added
- Native Vulkan layer (`VK_LAYER_AUTOHDR`) that turns a game's SDR swapchain into HDR10
  (PQ / BT.2020). It runs below Wine/Proton, so no Windows DLL is injected into the game.
- Compute-shader port of the ReShade `AutoHDR.fx` math: inverse Reinhard tonemap,
  BT.709 → BT.2020 (optional "Vivid" expanded gamut) and PQ encoding.
- Color controls: paper white, peak brightness, brightness, contrast, saturation,
  color temperature and gamma. All of them apply live.
- Global profile and per-game profiles, applied automatically when a game starts.
- Status that reads the real panel state from the kernel (DRM `Colorspace`), plus a
  diagnostics report.
- UI in 10 languages: Español, English, Français, Deutsch, Italiano, Português, Català,
  中文, 日本語, Русский. Automatic mode follows Steam's language.
- Per-game opt-out with the `DISABLE_AUTOHDR=1 %command%` launch option.

[1.1.0]: https://github.com/Pochowan/decky-autohdr/releases/tag/v1.1.0
[1.0.0]: https://github.com/Pochowan/decky-autohdr/releases/tag/v1.0.0
