set(vsyncconvenience_SOURCES
    softwarevsyncmonitor.cpp
    vsyncmonitor.cpp
)

add_library(VsyncSupport OBJECT ${vsyncconvenience_SOURCES})
target_link_libraries(VsyncSupport Qt5::Core Qt5::Gui)
target_include_directories(VsyncSupport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
