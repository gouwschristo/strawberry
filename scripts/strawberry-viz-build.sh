#!/usr/bin/env bash
#
# build-strawberry-viz.sh
# Build Strawberry with projectM visualizations and install it to /usr/local.
#
# Steps:
#   1. Install build dependencies (pacman).
#   2. Build libprojectM 4.2 from source. The Arch repo ships 4.1.6, which is
#      missing projectm_opengl_render_frame_fbo() — required to render into
#      Strawberry's QOpenGLWidget framebuffer. Git master (v4.2.0) has it.
#   3. Build Strawberry with HAVE_VISUALIZATIONS=ON. When this script sits inside
#      the tree it belongs to (the feat/youtubemusic checkout) that tree is built
#      as-is; run from anywhere else, it clones upstream 'visualisations' into
#      $WORK/strawberry and applies $PATCH first.
#   4. Install Strawberry and register /usr/local/lib in the linker path.
#   5. Install the monochrome icon set into Strawberry's user icon directory so the
#      left sidebar tabs use it instead of the colored artwork. (Runtime override
#      only - tweaking the icons needs no rebuild.)
#
# Pins (update if upstream moves):
#   - libprojectM: git master (v4.2.0)
#   - Strawberry:  'visualisations' branch (commit 304faf3 at time of writing)
#
# Usage:  scripts/strawberry-viz-build.sh      (inside this repository)
#         strawberry-viz-build.sh               (standalone, clones upstream)
# Env:    PREFIX=/usr/local   WORK=$HOME/Projects/strawberry-viz   JOBS=$(nproc)
#         REUSE=1 (default) reuses an existing checkout and rebuilds it in place;
#         REUSE=0 wipes it and clones fresh.
#         ICON_SRC=~/Projects/scripts/strawberry-icons   (icon set to install)
#         PATCH=~/Projects/scripts/patches/strawberry-local-changes.patch

set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
# Kept under $HOME so a reboot does not wipe the build tree (/tmp is cleared).
WORK="${WORK:-$HOME/Projects/strawberry-viz}"
JOBS="${JOBS:-$(nproc)}"
REUSE="${REUSE:-1}"

log() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

for c in git cmake ninja sudo; do
  command -v "$c" >/dev/null || die "required tool not found: $c"
done

# --- 1. Build dependencies ---------------------------------------------------
log "Installing build dependencies (pacman)"
sudo pacman -S --noconfirm --needed \
  git cmake ninja \
  qt6-base qt6-tools \
  taglib icu glib2 \
  gstreamer gst-plugins-base gst-plugins-base-libs \
  sqlite alsa-lib libxcb \
  kdsingleapplication sparsehash glm libglvnd \
  chromaprint fftw libebur128 libgpod libmtp gdk-pixbuf2 \
  yt-dlp python-mutagen

# --- 2. libprojectM 4.2 -------------------------------------------------------
if [[ -e "$PREFIX/lib/libprojectM-4.so" && -e "$PREFIX/lib/cmake/projectM4/projectM4Config.cmake" ]]; then
  log "libprojectM already installed at $PREFIX — skipping"
else
  log "Building libprojectM 4.2 from source"
  rm -rf "$WORK/projectm"
  git clone --depth 1 https://github.com/projectM-visualizer/projectm.git "$WORK/projectm"
  (
    cd "$WORK/projectm"
    git submodule update --init --depth 1 vendor/projectm-eval
    cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" \
      -DENABLE_INSTALL=ON \
      -DENABLE_SDL_UI=OFF \
      -DENABLE_PLAYLIST=ON \
      -DBUILD_SHARED_LIBS=ON \
      -DENABLE_TESTING=OFF \
      -DENABLE_SYSTEM_GLM=ON \
      -DENABLE_SYSTEM_PROJECTM_EVAL=OFF
    cmake --build build -j"$JOBS"
    sudo cmake --install build
  )
fi

# --- 3. Strawberry source tree ------------------------------------------------
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SELF_DIR" rev-parse --show-toplevel 2>/dev/null || true)"

if [[ -n "$REPO_ROOT" && -d "$REPO_ROOT/src/youtubemusic" ]]; then
  # Running from inside the tree this script belongs to: build it, patch nothing.
  STRAWBERRY_SRC="$REPO_ROOT"
  log "Building this checkout: $STRAWBERRY_SRC"
else
  log "Building Strawberry (branch: visualisations)"
  if [[ "$REUSE" == "1" && -d "$WORK/strawberry/.git" ]]; then
    log "Reusing existing checkout: $WORK/strawberry (REUSE=0 to clone fresh)"
  else
    rm -rf "$WORK/strawberry"
    git clone --depth 1 --branch visualisations \
      https://github.com/strawberrymusicplayer/strawberry.git "$WORK/strawberry"
  fi
  STRAWBERRY_SRC="$WORK/strawberry"
fi

# Patching is only needed for the standalone clone; the in-repo build already has the work.
if [[ "$STRAWBERRY_SRC" == "$REPO_ROOT" ]]; then
  log "Patch step skipped: $STRAWBERRY_SRC already contains the work"
else
  LOCAL_PATCH="${PATCH:-$HOME/Projects/scripts/patches/strawberry-local-changes.patch}"
  if [[ -f "$LOCAL_PATCH" ]]; then
    # On a reused checkout the patch is normally already in the tree - applying it again
    # would fail and look like a broken build.
    if git -C "$STRAWBERRY_SRC" apply --check --reverse "$LOCAL_PATCH" >/dev/null 2>&1; then
      log "Local patch already applied: $(basename "$LOCAL_PATCH")"
    elif git -C "$STRAWBERRY_SRC" apply --3way "$LOCAL_PATCH"; then
      log "Applied local patch: $(basename "$LOCAL_PATCH")"
    else
      printf '\033[1;33mwarning:\033[0m patch did not apply, building unpatched\n' >&2
    fi
  else
    printf '\033[1;33mwarning:\033[0m %s missing, building unpatched\n' "$LOCAL_PATCH" >&2
  fi
fi
(
  cd "$STRAWBERRY_SRC"
  cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_PREFIX_PATH="$PREFIX"
  cmake --build build -j"$JOBS"
  sudo cmake --install build
)

# --- 4. Runtime library path ---------------------------------------------------
log "Registering $PREFIX/lib in ld.so.conf"
echo "$PREFIX/lib" | sudo tee /etc/ld.so.conf.d/zz-local-viz.conf >/dev/null
sudo ldconfig

# --- 5. Monochrome sidebar icons -----------------------------------------------
# Strawberry prefers icons in <app data>/icons over its built-in artwork. Link that
# directory to the versioned icon set in this repo so edits take effect on restart.
ICON_SRC="${ICON_SRC:-$HOME/Projects/scripts/strawberry-icons}"
ICON_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/strawberry/strawberry/icons"
if [[ -x "$ICON_SRC/install.sh" ]]; then
  log "Installing monochrome sidebar icons into $ICON_DIR"
  DEST="$ICON_DIR" "$ICON_SRC/install.sh"
else
  printf '\033[1;33mwarning:\033[0m %s missing, sidebar icons left unchanged\n' "$ICON_SRC" >&2
fi

log "Done. Launch:  $PREFIX/bin/strawberry   then Tools -> Visualizations"
