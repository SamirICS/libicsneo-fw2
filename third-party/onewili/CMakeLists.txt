project(onewili C)

# Vendored verbatim from the OneWili C client -- see README.md for provenance and
# the outstanding license question. Do not edit the sources; regenerate instead.
#
# All five translation units are listed even though libicsneo-fw2 only calls into
# binary_framing.c (ow_bin_parser_*) and onewili_events.c
# (ow_decode_can_rx_report). The rest are their link closure and each other's:
# onewili_events.c references ow_poll_text_line from onewili.c, and
# onewili_files.c references a generated symbol from the same. Building this as a
# static library means the linker still pulls in only the objects that are
# actually reached.
add_library(onewili
	src/binary_framing.c
	src/binary_transport.c
	src/onewili.c
	src/onewili_events.c
	src/onewili_files.c
)

target_include_directories(onewili PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Generated code; it is not our job to make it warning-clean, and we must not
# edit it to do so. Errors still error.
if(NOT MSVC)
	target_compile_options(onewili PRIVATE -w)
endif()
