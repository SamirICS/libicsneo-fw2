# FW2 bring-up harnesses. Only meaningful when the OneWili driver is compiled in,
# so the whole directory sits behind LIBICSNEO_ENABLE_ONEWILI.

add_executable(fw2_smoke fw2_smoke.cpp)
target_link_libraries(fw2_smoke icsneocpp)

# Configurable CLI transmit tool. Pure icsneocpp API, so unlike fw2_tx it
# builds on Windows too.
add_executable(fw2_send fw2_send.cpp)
target_link_libraries(fw2_send icsneocpp)

# Interactive transmit console. POSIX termios, so it is not built on Windows.
if(UNIX)
	add_executable(fw2_tx fw2_tx.cpp)
	target_link_libraries(fw2_tx icsneocpp)
endif()

# Bring-up diagnostic: dumps raw SetupAPI enumeration, independent of
# OneWili::Find()'s matching logic. Windows-only; no icsneocpp dependency.
if(WIN32)
	add_executable(fw2_ports fw2_ports.cpp)
	target_link_libraries(fw2_ports setupapi)
endif()

# Qt6 desktop application. Self-skipping when Qt6 is not present.
add_subdirectory(fw2_desktop)
