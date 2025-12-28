project(win-openvr)

if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /GL")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG /OPT:REF /OPT:ICF")
    include_directories(../../deps/openvr)
	link_directories(../../deps/openvr)
endif()

set(win-openvr_SOURCES win-openvr.cpp)

add_library(win-openvr MODULE ${win-openvr_SOURCES})
target_link_libraries(win-openvr libobs)
set_target_properties_obs(win-openvr PROPERTIES FOLDER plugins)