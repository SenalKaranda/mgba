include(ExportDirectory)
set(SOURCE_FILES
	commandline.c
	netplay/input.c
	netplay/peer.c
	netplay/protocol.c
	netplay/session.c
	netplay/transport-sim.c
	proxy-backend.c
	thread-proxy.c
	video-backend.c
	video-logger.c)

if(ENABLE_VFS)
	list(APPEND SOURCE_FILES
		updater.c)
endif()

set(TEST_FILES
	netplay/test/peer.c
	netplay/test/session.c)

set(GUI_FILES
	gui/cheats.c
	gui/gui-config.c
	gui/gui-runner.c
	gui/remap.c)

source_group("Extra features" FILES ${SOURCE_FILES})
source_group("Extra GUI source" FILES ${GUI_FILES})

export_directory(EXTRA SOURCE_FILES)
export_directory(EXTRA_GUI GUI_FILES)
export_directory(EXTRA_TEST TEST_FILES)
