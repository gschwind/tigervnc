include_directories(${X11_INCLUDE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/unix/common)
include_directories(${CMAKE_SOURCE_DIR}/unix/tx)
include_directories(${CMAKE_SOURCE_DIR}/unix)
include_directories(${CMAKE_SOURCE_DIR}/common)

add_executable(spawnvncserver
  buildtime.c
  Geometry.cxx
  TimeMillis.cxx
  qnum_to_xorgevdev.c
  qnum_to_xorgkbd.c
  spawnvncserver.cxx
  XPixelBuffer.cxx
  XDesktop.cxx
  VNCSConnectionSpawn.cxx
  VNCScreenSpawn.cxx
  VNCServerSpawn.cxx
  RandrGlue.c
  ../vncconfig/QueryConnectDialog.cxx
)

target_link_libraries(spawnvncserver tx rfb network rdr unixcommon ${XCB_LIBRARIES})

if(X11_FOUND AND X11_XTest_LIB)
  add_definitions(-DHAVE_XTEST)
  target_link_libraries(spawnvncserver ${X11_XTest_LIB})
else()
  message(WARNING "No XTest extension.  spawnvncserver will be view-only.")
endif()

if(X11_FOUND AND X11_Xdamage_LIB)
  add_definitions(-DHAVE_XDAMAGE)
  target_link_libraries(spawnvncserver ${X11_Xdamage_LIB})
else()
  message(WARNING "No DAMAGE extension.  spawnvncserver will have to use the slower polling method.")
endif()

if(X11_FOUND AND X11_Xfixes_LIB)
  add_definitions(-DHAVE_XFIXES)
  target_link_libraries(spawnvncserver ${X11_Xfixes_LIB})
else()
  message(WARNING "No XFIXES extension.  spawnvncserver will not be able to show cursors.")
endif()

if(X11_FOUND AND X11_Xrandr_LIB)
  add_definitions(-DHAVE_XRANDR)
  target_link_libraries(spawnvncserver ${X11_Xrandr_LIB})
else()
  message(WARNING "No Xrandr extension.  spawnvncserver will not be able to resize session.")
endif()

target_link_libraries(spawnvncserver ${X11_LIBRARIES})

install(TARGETS spawnvncserver DESTINATION ${BIN_DIR})
install(FILES spawnvncserver.man DESTINATION ${MAN_DIR}/man1 RENAME spawnvncserver.1)
