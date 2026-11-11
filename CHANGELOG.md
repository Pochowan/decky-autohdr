# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/) and the project uses
[Semantic Versioning](https://semver.org/).

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

[1.0.0]: https://github.com/Pochowan/decky-autohdr/releases/tag/v1.0.0
