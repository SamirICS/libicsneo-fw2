# FW2 bring-up harnesses. Only meaningful when the OneWili driver is compiled in,
# so the whole directory sits behind LIBICSNEO_ENABLE_ONEWILI.

add_executable(fw2_smoke fw2_smoke.cpp)
target_link_libraries(fw2_smoke icsneocpp)

# Interactive transmit console. POSIX termios, so it is not built on Windows.
if(UNIX)
	add_executable(fw2_tx fw2_tx.cpp)
	target_link_libraries(fw2_tx icsneocpp)
endif()
