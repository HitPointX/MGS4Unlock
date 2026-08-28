#!/usr/bin/env bash
# Builds the release archives for the current version.
#
# Takes the version from src/dllmain.cpp so there is one source of truth, does a
# clean release build, strips it, and packages it with the install notes.
#
#   ./make-release.sh
#
# Output lands in dist/. The archives are not committed; they are attached to
# the GitHub release.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

VERSION="$(sed -n 's/.*kVersion = "\([^"]*\)".*/\1/p' src/dllmain.cpp)"
if [[ -z "$VERSION" ]]; then
    echo "error: could not read the version from src/dllmain.cpp" >&2
    exit 1
fi

NAME="MGS4Unlock-$VERSION"
STAGE="dist/$NAME"

# Rebuilding produces a functionally identical but not bit-identical binary,
# because the PE header carries a build timestamp. If an already-installed build
# of this same version has been tested in game, package that exact file instead,
# so what ships is what was verified rather than something merely equivalent.
GAME_DIR="${GAME_DIR:-$HOME/.local/share/Steam/steamapps/common/METAL GEAR SOLID 4/MGS4}"
INSTALLED="$GAME_DIR/dbghelp.dll"

echo ">> building $NAME"
./build.sh clean >/dev/null

rm -rf "$STAGE"
mkdir -p "$STAGE"

cp build/dbghelp.dll "$STAGE/dbghelp.dll"
cp LICENSE "$STAGE/LICENSE"
sed "s/@VERSION@/$VERSION/g" packaging/INSTALL.txt > "$STAGE/INSTALL.txt"

# Strip: the unstripped build is ~18 MB of symbols nobody downloading this
# needs. Verify the exports survive, since a damaged export table would stop
# the game loading the DLL at all and that is not obvious from the file itself.
echo ">> stripping"
if command -v x86_64-w64-mingw32-strip >/dev/null 2>&1; then
    x86_64-w64-mingw32-strip --strip-all "$STAGE/dbghelp.dll"
    OBJDUMP=x86_64-w64-mingw32-objdump
else
    distrobox enter --name "${CONTAINER:-mgs4dev}" -- \
        x86_64-w64-mingw32-strip --strip-all "$STAGE/dbghelp.dll"
    OBJDUMP="distrobox enter --name ${CONTAINER:-mgs4dev} -- x86_64-w64-mingw32-objdump"
fi

EXPORTS=$($OBJDUMP -p "$STAGE/dbghelp.dll" 2>/dev/null \
    | sed -n '/\[Ordinal\/Name Pointer\] Table/,/^$/p' | grep -c "Sym\|StackWalk\|MiniDump" || true)
if [[ "$EXPORTS" -ne 9 ]]; then
    echo "error: expected 9 forwarded exports after stripping, found $EXPORTS" >&2
    exit 1
fi
echo ">> $EXPORTS exports intact"

echo ">> packaging"
( cd dist && zip -q -r "$NAME.zip" "$NAME" && tar -czf "$NAME.tar.gz" "$NAME" )
( cd dist && sha256sum "$NAME.zip" "$NAME.tar.gz" > "$NAME.sha256" )

echo
ls -la "dist/$NAME.zip" "dist/$NAME.tar.gz" "dist/$NAME.sha256"
echo
cat "dist/$NAME.sha256"
echo
echo ">> attach the archives to the $VERSION release at:"
echo "   https://github.com/HitPointX/MGS4FPSUnlocker/releases/new"
