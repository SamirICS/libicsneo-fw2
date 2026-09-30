# fw2_desktop -- Qt6 desktop console for the FreeWili 2.
#
# Skipped rather than fatal when Qt6 is absent: the command-line harnesses in the
# parent directory are the ones the CI gate depends on, and a build machine
# without Qt should still produce them.
#
# Qt6::Widgets is LGPLv3 and is linked dynamically, which is what a shared Qt
# build produces by default. Do not switch this to a static Qt without revisiting
# that -- this binary ships to customers as freeware.

find_package(Qt6 QUIET COMPONENTS Widgets)

if(NOT Qt6_FOUND)
	message(STATUS "Qt6 not found -- skipping fw2_desktop (set CMAKE_PREFIX_PATH to your Qt6)")
	return()
endif()

set(CMAKE_AUTOMOC ON)

add_executable(fw2_desktop
	main.cpp
	MainWindow.cpp
	DeviceWorker.cpp
	MonitorPanel.cpp
	TransmitPanel.cpp
	SettingsPanel.cpp
	PowerPanel.cpp
	ConsolePanel.cpp
)

target_include_directories(fw2_desktop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fw2_desktop PRIVATE Qt6::Widgets icsneocpp)

if(WIN32)
	set_target_properties(fw2_desktop PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

message(STATUS "fw2_desktop: building against Qt ${Qt6_VERSION}")
