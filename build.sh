#!/bin/bash
# Build script for Linux hosts (native tests/CLI) and MinGW cross builds for Windows.
#   ./build.sh native   -> build/native/{aes67vsc,test_core}
#   ./build.sh windows  -> build/win64/{aes67asio.dll,aes67vsc.exe,aes67vsc-panel.exe}
#   ./build.sh test     -> native build + run unit tests + loopback self test
set -e
cd "$(dirname "$0")"
CXX_NATIVE=${CXX_NATIVE:-g++}
CXX_WIN=${CXX_WIN:-x86_64-w64-mingw32-g++}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
CORE="src/core/platform.cpp src/core/sdp.cpp src/core/rtp.cpp src/core/log.cpp src/core/config.cpp src/core/status.cpp src/core/ptp.cpp src/core/sap.cpp src/core/engine.cpp"
FLAGS="-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -DNDEBUG"
VERSION=$(cat VERSION 2>/dev/null || echo 0.1.0)

native() {
  mkdir -p build/native
  $CXX_NATIVE $FLAGS -pthread $CORE src/app/cli.cpp -o build/native/aes67vsc -lrt
  $CXX_NATIVE $FLAGS -pthread $CORE src/tests/test_core.cpp -o build/native/test_core -lrt
  echo "built build/native/aes67vsc and build/native/test_core"
}

windows() {
  mkdir -p build/win64
  local WLIBS="-lws2_32 -liphlpapi -lwinmm -lole32 -luuid -ladvapi32 -lshell32"
  local WFLAGS="$FLAGS -D_WIN32_WINNT=0x0A00 -DAES67VSC_VERSION=\"$VERSION\" -static"
  # ASIO driver DLL
  $WINDRES src/asio/aes67asio.rc -O coff -o build/win64/aes67asio.res
  $CXX_WIN $WFLAGS -shared $CORE src/asio/asio_driver.cpp build/win64/aes67asio.res src/asio/aes67asio.def -o build/win64/aes67asio.dll $WLIBS -Wl,--enable-stdcall-fixup
  # console CLI
  $CXX_WIN $WFLAGS $CORE src/app/cli.cpp -o build/win64/aes67vsc.exe $WLIBS
  # WASAPI bridge
  $CXX_WIN $WFLAGS $CORE src/app/bridge.cpp -o build/win64/aes67vsc-bridge.exe $WLIBS -lavrt
  # control panel GUI
  $WINDRES src/app/panel.rc -O coff -o build/win64/panel.res
  $CXX_WIN $WFLAGS -mwindows $CORE src/app/panel.cpp build/win64/panel.res -o build/win64/aes67vsc-panel.exe $WLIBS -lcomdlg32 -lcomctl32 -lgdi32 -luser32
  ${STRIP:-x86_64-w64-mingw32-strip} build/win64/aes67asio.dll build/win64/aes67vsc.exe build/win64/aes67vsc-panel.exe build/win64/aes67vsc-bridge.exe
  cp scripts/install.bat scripts/uninstall.bat README.md LICENSE build/win64/ 2>/dev/null || true
  cp sdp/example-receive.sdp build/win64/ 2>/dev/null || true
  echo "built build/win64/"
  ls -la build/win64/*.dll build/win64/*.exe
}

test_all() {
  native
  ./build/native/test_core
  ./build/native/aes67vsc selftest 6 --general.log_level=1
}

case "${1:-all}" in
  native) native ;;
  windows) windows ;;
  test) test_all ;;
  all) native; windows ;;
  *) echo "usage: $0 native|windows|test|all"; exit 1 ;;
esac
