# OpenVR Capture for OBS Studio

This 64bit plugin allows capturing directly from OpenVR/SteamVR mirror surface in full resolution.

A fork of OBS-OpenVR-Input-Plugin, originally made by Keijo "Kegetys" Ruotsalainen

---------

Compiling:
- Pull OBS Studio source code recursively (git clone https://github.com/obsproject/obs-studio.git --recursive)
- Extract source code of this repo into OBS Studio source code
- Pull OpenVR SDK inside "deps" folder. (git clone https://github.com/ValveSoftware/openvr.git)
- Add add_subdirectory(win-openvr) to the end of plugins/CMakeLists.txt
- Compile with "cmake --preset windows-x64", build from generated .sln file using Visual Studio.