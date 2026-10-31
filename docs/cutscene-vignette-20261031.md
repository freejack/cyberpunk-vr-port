# Cutscene comfort vignette

While a cinematic moves the camera, the edges of each eye's view are darkened, and the darkening fades
in and out over 0.5 s. Code: `include/Runtimes/CutsceneVignette.hpp` (policy, geometry, CPU reference),
`src/Runtimes/CutsceneVignette.cpp` (per-frame sampling, fade), `src/Render/VignettePass.cpp` (the draw),
call site in `src/Runtimes/OpenXRFrameLoop.cpp` beside the VR menu blink.

## When

`PlayerPuppet::sceneTier`, already read for the VRIK suspend (`g_sceneTier`, `Hooks/LocateCamera.cpp`).
Same stored encoding and labels as `xr_cutscene_suspend_tier`: -1 off, 1..4 = Tier2+..Tier5. The default
is Tier4+, the same threshold the VRIK suspend ships with: FPP and full cinematics. Takeover cameras and
the AV turret run at Tier 3 (`takeover-vehicle-yaw-20260925.md`), so they are not affected by default.

Never in a braindance (`g_bdActive`) or with a device camera latched (`DeviceCamActive()`), at any
threshold: the player drives the view in both and needs the periphery.

The tier is only written while `GetPlayer` returns a player. On the main menu or a loading screen it
keeps the last scene's value, so a new `g_sceneTierStampMs` is written beside it and a tier older than
2 s is ignored (the same window as `cvr::vrmenu::BlinkClosure`). Without this, quitting to the menu from
a cutscene would leave the vignette on. During a scene the refresh runs from both camera paths:
the FPP camera locate (`Hooks/LocateCamera.cpp`) and the camera director's external-camera serialize
(`Hooks/CameraPoseTransport.cpp`, `ExternalAllowed()`: no menu, session running, no device camera).
So the tier should stay fresh in first- and third-person scenes alike. If some scene ever runs with
neither, the panel's status line reads "Scene tier: none" while it plays, and the vignette will have
faded out.

## Where

Into each eye's OpenXR swapchain image on the submit list, after the eye's copy or decode and after the
last change to that eye's submitted FOV (asymmetric window, MAIN's window on a mono frame, the
flat-content vergence turn):

- head-locked, because it is part of the image;
- on every submitted frame, including generated and repeated ones, because it is applied after frame
  generation (the interpolator never sees it);
- under the HUD quads and the F10 panel, which are separate layers, so subtitles and dialogue choices
  are not darkened;
- not over a menu (`menuRectActive`): that image is shown as a flat quad.

The ring is a constant **angle** around the eye's optical axis (tan = 0), computed per pixel from the
submitted `XrFovf`. It is not centred on the image middle: under the asymmetric projection the image
centre is several degrees off the axis.

## How

One fullscreen triangle, root constants only (8 floats), no descriptors, so nothing can be rewritten
under the three submit lists in flight. Blend `ZERO / SRC_ALPHA` with colour writes only: `dst.rgb *=
keep`, and the eye image's alpha is untouched. Pixels inside the clear view are discarded.

`strength` is perceived darkening. Every target the submit picks, except the "game's own format" copy
fallback, blends in linear light, so `keep = (1 - darken)^2.2` there and `1 - darken` on an encoded
target (`VignettePass::BlendsInLinear`). Without the exponent, 85 % would look like about half.

## Settings (vrport.ini, General → Cutscene vignette)

| Key | Default | Range |
|---|---:|---|
| `xr_cutscene_vignette_tier` | 3 (Tier4+) | -1 off, 1..4 |
| `xr_cutscene_vignette_strength` | 0.85 | 0..1 |
| `xr_cutscene_vignette_size` | 1 (Medium) | 0 Light, 1 Medium, 2 Strong |

The size is three steps (`kVignetteSizes`). Each step is a clear full angle around the eye's axis,
followed by the same 20° ramp: Light 60° (darkening 30°→50° off-axis), Medium 45° (22.5°→42.5°),
Strong 30° (15°→35°). Medium is the default because a headset test found Light too weak. Three named
steps instead of free angles keep the choice simple in the VR menu.

Fields are appended at the end of `LiveControls` and `LiveControlsUiState`. The VR menu mirrors the
panel. The tier labels are the VRIK suspend's, so they are already translated. The other new strings
fall back to English until `tools/menu_locale` is rerun.

## Validation

- `tools/comfort_tests` `comfort_vignette_policy`: tier gate, staleness, exclusions, normalization, fade
  timing (0.5 s at 90 Hz both ways, hitch-clamped), and ring geometry: symmetric, and asymmetric with
  equal darkening at equal angles around the axis but not around the image centre.
- `comfort_vignette_gpu` (RTX 5090) and `comfort_vignette_gpu warp`: the pass on R8G8B8A8/B8G8R8A8
  `_SRGB`, R16G16B16A16_FLOAT, R8G8B8A8_UNORM decoded and encoded, and R10G10B10A2_UNORM, at all three
  sizes. Every pixel is compared with `VignetteKeep()`, and alpha is checked to be preserved. Three
  injected shader bugs (vertical flip, image-centred ring, missing gamma) were each caught.
- Measured: on 8-bit targets the RTX 5090 takes the `SRC_ALPHA` factor at 1/255 precision (D3D allows
  this; WARP does not). In the darkest rim that is about 2 encoded levels of near-black. The test
  accepts either neighbouring step.

**In a headset:** tested on SteamVR with `xr_engine_stereo=1`. The eye format was R8G8B8A8_UNORM_SRGB
(the copy path), the pass initialized, and the vignette showed in cutscenes. The Light size was judged
too weak, which set the default to Medium. Not yet verified there: Medium and Strong themselves, and
the tiers that more kinds of scenes report (the panel's status line shows it live).
