# OpenVR Capture for OBS Studio

This plugin allows capturing directly from OpenVR/SteamVR in full resolution.

A fork of OBS-OpenVR-Input-Plugin, originally made by Keijo "Kegetys" Ruotsalainen

### Q. How does this affect performance?
A. This has virtually no effect on performance.
### Q. What benefits does this have over the original?
A.
- initialization is asynchronous which eliminates stutter in OBS Studio
- OpenVR SDK updated from v1.12.5 to v2.5.1
- Runtime check prevents SteamVR crash on launch, making it usable while livestreaming
- Quest 3 presets by default
- Various minor performance tweaks
- Grammar fixed
- Can be compiled individually while only using cmake

---------

Installation:
1. Download latest release .zip
2. Extract all files to the root of your OBS Studio installation.

---------

Compiling:
1. Pull OBS Studio source code recursively (`git clone https://github.com/obsproject/obs-studio.git --recursive`)
2. Pull this repo into the root of OBS Studio's source code (`git clone https://github.com/Pigney/OpenVR-Capture.git`)
3. Pull OpenVR SDK inside "deps" folder. (`git clone https://github.com/ValveSoftware/openvr.git`)
4. Add `add_obs_plugin(win-openvr PLATFORMS WINDOWS)` to the end of obs-studio/plugins/CMakeLists.txt
5. Compile from root directory with `cmake --preset windows-x64 && cmake --build ./build_x64/plugins/win-openvr --config Release`
