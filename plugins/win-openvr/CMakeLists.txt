project(win-openvr)

if(MSVC)
    include_directories(../../deps/openvr)
	link_directories(../../deps/openvr)
endif()

set(win-openvr_SOURCES
	win-openvr.cpp)

add_library(win-openvr MODULE
	${win-openvr_SOURCES})
target_link_libraries(win-openvr
	libobs)
set_target_properties_obs(win-openvr PROPERTIES FOLDER plugins)