# OpenVR Capture for OBS Studio

This plugin allows capturing directly from OpenVR/SteamVR in full resolution.

A fork of OBS-OpenVR-Input-Plugin, originally made by Keijo "Kegetys" Ruotsalainen

### Q. How does this affect performance?
A. This has virtually no effect on performance.

---------

Installation:
- Download latest release .zip
- Extract all files to the root of your OBS Studio installation.

---------

Compiling:
- Pull OBS Studio source code recursively (`git clone https://github.com/obsproject/obs-studio.git --recursive`)
- Pull this repo into the root of OBS Studio's source code (`git clone https://github.com/Pigney/OpenVR-Capture.git`)
- Pull OpenVR SDK inside "deps" folder. (`git clone https://github.com/ValveSoftware/openvr.git`)
- Add `add_obs_plugin(win-openvr PLATFORMS WINDOWS)` to the end of obs-studio/plugins/CMakeLists.txt
- Compile from root directory with `cmake --preset windows-x64 && cmake --build ./build_x64/plugins/win-openvr --config Release`