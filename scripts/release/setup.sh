#!/bin/sh
# FnQAA setup (Linux). Finds your Quake Arena Arcade package in this folder
# (the XBLA zip, the zip extracted, your Xbox 360 Content folder or the
# package file), recompiles QAA's game code from it on this machine and
# installs the result here. Run it again after updating FnQAA.
# Options are passed to setup/tools/recomp/build.py (--help lists them).
here=$(cd "$(dirname "$0")" && pwd) || exit 1
if ! command -v python3 >/dev/null 2>&1; then
	echo "Python 3 is needed: sudo apt install python3" >&2
	exit 1
fi
exec python3 "$here/setup/tools/recomp/build.py" "$@"
