#!/bin/sh

# Resolve all application files relative to this script so the program works
# whether it is started by Launcher, SSH, or from another working directory.
APP_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$APP_DIR" || exit 1

# MaixCDK currently copies the ALSA linker names while libms_asr requests the
# SONAMEs. Materialize the aliases inside the app so a clean board does not
# depend on system-wide ALSA packages.
for SONAME_PAIR in "libasound.so.2:libasound.so" \
                   "libatopology.so.2:libatopology.so"; do
    SONAME=${SONAME_PAIR%%:*}
    LIBRARY=${SONAME_PAIR#*:}
    if [ ! -e "$APP_DIR/dl_lib/$SONAME" ] && \
       [ -e "$APP_DIR/dl_lib/$LIBRARY" ]; then
        ln -s "$LIBRARY" "$APP_DIR/dl_lib/$SONAME" || {
            echo "Failed to create $SONAME runtime alias" >&2
            exit 126
        }
    fi
done

# dl_lib contains application dependencies. /opt/lib contains the MaixCAM2
# platform runtime (libax_*.so, libmaixcam_lib.so, and related libraries).
# Preserve any paths already supplied by the system or Launcher.
export LD_LIBRARY_PATH="$APP_DIR/dl_lib:/opt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Some maixtool versions do not preserve executable bits when extracting an
# application ZIP. Restore the permission before every launch so both fresh
# installations and upgrades are deterministic.
chmod +x "$APP_DIR/dart_green_detect" || {
    echo "Failed to make dart_green_detect executable" >&2
    exit 126
}

exec "$APP_DIR/dart_green_detect" \
    --config "$APP_DIR/green_detector.conf" \
    "$@"
