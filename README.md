# OpenVR Capture for OBS Studio

This 64bit plugin allows capturing directly from OpenVR/SteamVR mirror surface in full resolution.

A fork of OBS-OpenVR-Input-Plugin, originally made by Keijo "Kegetys" Ruotsalainen

### Q. How does this affect performance?
A. This results in a reduction of ~3 FPS.

---------

Compiling:
- Pull OBS Studio source code recursively (`git clone https://github.com/obsproject/obs-studio.git --recursive`)
- Extract source code of this repo into OBS Studio source code
- Pull OpenVR SDK inside "deps" folder. (`git clone https://github.com/ValveSoftware/openvr.git`)
- Add `add_obs_plugin(win-openvr PLATFORMS WINDOWS)` to the end of plugins/CMakeLists.txt
- Compile from root directory with `cmake --preset windows-x64`, build from generated .sln file inside obs-studio/build_x64/plugins/win-openvr using Visual Studio.

until i get smarter and fix it, if the build fails: inside the Solution Explorer, open plugins folder and right click win-openvr, open properties, C/C++, General and set "Treat Warnings as Errors" to "No (/WX-)"