#!/bin/sh
set -u

APP_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
PACKAGED_CONFIG="$APP_DIR/recorder.conf"
OVERRIDE_CONFIG=/root/dart_data_recorder.conf
UI_STATE=/root/dart_data_recorder_ui_state.conf

RECORD_SECONDS=10
RECORD_MODE=full180
EXPOSURE_US=500
GAIN=0
WB_GAINS=0.0682,0,0,0.04897
SAVE_SAMPLE_FRAME=true
DATA_ROOT=/root/dart_recordings
MIN_FREE_MB=512
MAX_MIPI_ERRORS=2
SESSION_LABEL=unlabeled
HEADLESS=false

load_config() {
    config_path=$1
    [ -f "$config_path" ] || return 0
    while IFS='=' read -r key value; do
        case "$key" in
            ''|'#'*) continue ;;
            seconds) RECORD_SECONDS=$value ;;
            mode) RECORD_MODE=$value ;;
            exposure_us) EXPOSURE_US=$value ;;
            gain) GAIN=$value ;;
            wb_gains) WB_GAINS=$value ;;
            save_sample_frame) SAVE_SAMPLE_FRAME=$value ;;
            data_root) DATA_ROOT=$value ;;
            min_free_mb) MIN_FREE_MB=$value ;;
            max_mipi_errors) MAX_MIPI_ERRORS=$value ;;
            session_label) SESSION_LABEL=$value ;;
            *) echo "Unknown recorder setting: $key" >&2; return 2 ;;
        esac
    done < "$config_path"
}

load_selection() {
    selection_path=$1
    [ -f "$selection_path" ] || return 2
    have_seconds=false
    have_exposure=false
    have_gain=false
    while IFS='=' read -r key value; do
        case "$key" in
            seconds) RECORD_SECONDS=$value; have_seconds=true ;;
            exposure_us) EXPOSURE_US=$value; have_exposure=true ;;
            gain) GAIN=$value; have_gain=true ;;
            *) echo "Unknown UI selection: $key" >&2; return 2 ;;
        esac
    done < "$selection_path"
    [ "$have_seconds" = true ] && [ "$have_exposure" = true ] && [ "$have_gain" = true ]
}

validate_tunable_settings() {
    case "$RECORD_SECONDS" in ''|*[!0-9]*) return 2 ;; esac
    [ "$RECORD_SECONDS" -ge 1 ] && [ "$RECORD_SECONDS" -le 600 ] || return 2
    case "$EXPOSURE_US" in ''|*[!0-9]*) return 2 ;; esac
    [ "$EXPOSURE_US" -ge 1 ] && [ "$EXPOSURE_US" -le 5554 ] || return 2
    case "$GAIN" in ''|*[!0-9]*) return 2 ;; esac
    [ "$GAIN" -le 16384 ] || return 2
}

show_status() {
    if [ "$HEADLESS" = true ]; then printf '%s: %s\n' "$1" "$2"; return 0; fi
    python_bin=$(command -v python3 || command -v python || true)
    if [ -n "$python_bin" ]; then
        "$python_bin" "$APP_DIR/show_status.py" "$1" "$2" "$3" "${4:-0}" || true
    fi
}

fail() {
    message=$1
    echo "$message" >&2
    show_status "RECORD FAILED" "$message" error 5
    exit 1
}

load_config "$PACKAGED_CONFIG" || fail "Invalid packaged config"
load_config "$OVERRIDE_CONFIG" || fail "Invalid override config"

while [ $# -gt 0 ]; do
    case "$1" in
        '') shift ;; # The MaixAPP launcher supplies one empty argument.
        --headless) HEADLESS=true; shift ;;
        --exposure-us) [ $# -ge 2 ] || fail "Missing --exposure-us"; EXPOSURE_US=$2; shift 2 ;;
        --gain) [ $# -ge 2 ] || fail "Missing --gain"; GAIN=$2; shift 2 ;;
        --seconds) [ $# -ge 2 ] || fail "Missing --seconds"; RECORD_SECONDS=$2; shift 2 ;;
        --label) [ $# -ge 2 ] || fail "Missing --label"; SESSION_LABEL=$2; shift 2 ;;
        *) fail "Unknown option: $1" ;;
    esac
done

validate_tunable_settings || fail "Invalid time, exposure or gain"
[ "$RECORD_MODE" = full180 ] || fail "Only full180 is enabled"
case "$MIN_FREE_MB" in ''|*[!0-9]*) fail "Invalid free-space limit" ;; esac
case "$MAX_MIPI_ERRORS" in 0|2) ;; *) fail "max_mipi_errors must be 0 or 2" ;; esac
case "$SAVE_SAMPLE_FRAME" in true|false) ;; *) fail "Invalid sample setting" ;; esac

# Labels become directory suffixes. Reject path syntax rather than trying to sanitize it.
case "$SESSION_LABEL" in ''|*[!A-Za-z0-9_-]*) fail "Label uses invalid characters" ;; esac
case "$DATA_ROOT" in /*) ;; *) fail "data_root must be absolute" ;; esac

mkdir -p "$DATA_ROOT" || fail "Cannot create data directory"

# v0.3.0 used an on-disk directory as the lock.  A power loss or SIGKILL could
# leave that directory behind forever, even though no recorder was active.
# Remove only the known-empty legacy directory, then use a kernel flock.  The
# lock file may persist, but its lock is released automatically when the
# process exits or the board reboots.
LOCK_FILE="$DATA_ROOT/.recording.lock"
if [ -d "$LOCK_FILE" ]; then
    if ! rmdir "$LOCK_FILE" 2>/dev/null && [ -d "$LOCK_FILE" ]; then
        fail "Legacy recorder lock is not empty"
    fi
    echo "Recovered stale legacy recorder lock"
fi
command -v flock >/dev/null 2>&1 || fail "flock command is required"
exec 9>"$LOCK_FILE" || fail "Cannot open recorder lock"
if ! flock -n 9; then
    fail "Recorder is already active"
fi
RUN_DIR=
SELECTION_FILE=
STATE_PARENT=${TMPDIR:-/tmp}
STATE_DIR=$(mktemp -d "$STATE_PARENT/dart_data_recorder.XXXXXX") || fail "Cannot create recorder state"
cleanup() {
    if [ -n "$SELECTION_FILE" ]; then
        rm -f "$SELECTION_FILE" "$SELECTION_FILE.tmp"
    fi
    rmdir "$STATE_DIR" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

available_kb=$(df -Pk "$DATA_ROOT" | awk 'NR==2 {print $4}')
case "$available_kb" in ''|*[!0-9]*) fail "Cannot read free space" ;; esac
[ "$available_kb" -ge $((MIN_FREE_MB * 1024)) ] || fail "Less than ${MIN_FREE_MB}MB free"

INDEX_FILE="$DATA_ROOT/next_clip_id"
if [ -f "$INDEX_FILE" ]; then
    clip_id=$(sed -n '1p' "$INDEX_FILE")
else
    clip_id=1
fi
case "$clip_id" in ''|*[!0-9]*) fail "Invalid clip counter" ;; esac
clip_number=$(printf '%06d' "$clip_id")

# Confirmation deliberately happens before the counter or recording directory is
# changed. Exit code 10 means the user cancelled and is not an error.
python_bin=$(command -v python3 || command -v python || true)
[ -n "$python_bin" ] || fail "Python is required for confirmation"
if [ "$HEADLESS" != true ]; then
SELECTION_FILE="$STATE_DIR/selection.conf"
"$python_bin" "$APP_DIR/confirm_start.py" \
    --clip "$clip_number" --seconds "$RECORD_SECONDS" \
    --exposure-us "$EXPOSURE_US" --gain "$GAIN" --label "$SESSION_LABEL" \
    --output "$SELECTION_FILE" --state "$UI_STATE"
confirm_result=$?
case "$confirm_result" in
    0) ;;
    10)
        show_status "RECORDING CANCELLED" "No clip was created" info 1
        exit 0
        ;;
    *) fail "Confirmation screen failed, code $confirm_result" ;;
esac
load_selection "$SELECTION_FILE" || fail "Invalid UI selection"
rm -f "$SELECTION_FILE" "$SELECTION_FILE.tmp"
SELECTION_FILE=
validate_tunable_settings || fail "Invalid UI time, exposure or gain"

fi

# Budget 8 MiB/s plus a fixed reserve; CBR is a target, not a hard ceiling.
# Check after the panel returns, since the selected duration may have changed.
available_kb=$(df -Pk "$DATA_ROOT" | awk 'NR==2 {print $4}')
case "$available_kb" in ''|*[!0-9]*) fail "Cannot read free space" ;; esac
required_kb=$((RECORD_SECONDS * 8192 + MIN_FREE_MB * 1024))
[ "$available_kb" -ge "$required_kb" ] || fail "Not enough space for selected duration"

next_id=$((clip_id + 1))
printf '%s\n' "$next_id" > "$INDEX_FILE.tmp" || fail "Cannot reserve clip id"
mv "$INDEX_FILE.tmp" "$INDEX_FILE" || fail "Cannot update clip id"

base_name="clip_${clip_number}_${SESSION_LABEL}"
RUN_DIR="$DATA_ROOT/${base_name}.incomplete"
[ ! -e "$RUN_DIR" ] || fail "Clip directory already exists"
mkdir "$RUN_DIR" || fail "Cannot create clip directory"

cat > "$RUN_DIR/record_context.json" <<EOF
{"schema_version":1,"clip_id":$clip_id,"label":"$SESSION_LABEL","mode":"$RECORD_MODE","width":1344,"height":760,"requested_fps":180,"seconds":$RECORD_SECONDS,"requested_exposure_us":$EXPOSURE_US,"requested_gain":$GAIN,"requested_wb_gains":"$WB_GAINS","max_mipi_errors":$MAX_MIPI_ERRORS,"wall_clock":"$(date -Iseconds 2>/dev/null || date)"}
EOF

printf 'record_directory=%s seconds=%s exposure_us=%s gain_raw=%s\n' "$RUN_DIR" "$RECORD_SECONDS" "$EXPOSURE_US" "$GAIN"
show_status "RECORDING ${clip_number}" "Screen off for capture; wait for SAVED" recording 1
cd "$RUN_DIR" || fail "Cannot enter clip directory"
chmod +x "$APP_DIR/direct_record" || fail "Recorder binary permission"
export LD_LIBRARY_PATH="$APP_DIR:/opt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

set -- "$APP_DIR/direct_record" \
    --mode "$RECORD_MODE" --record --nv21 --queue-depth 4 \
    --seconds "$RECORD_SECONDS" --sensor-lib "$APP_DIR/libsns_os04a10.so" \
    --exposure-us "$EXPOSURE_US" --gain "$GAIN" --wb-gains "$WB_GAINS" \
    --venc-retry --venc-depth 8 \
    --max-mipi-errors "$MAX_MIPI_ERRORS"
[ "$SAVE_SAMPLE_FRAME" = true ] && set -- "$@" --sample-frame

"$@" > recorder.stdout.log 2> recorder.stderr.log
result=$?
printf '%s\n' "$result" > process_exit_code
sync

cd "$DATA_ROOT" || exit 1
if [ "$result" -eq 0 ] && [ -s "$RUN_DIR/record.h264" ] && [ -s "$RUN_DIR/frames.csv" ] &&
   grep -q '"exit_code":0' "$RUN_DIR/capture.json"; then
    final_dir="$DATA_ROOT/$base_name"
    mv "$RUN_DIR" "$final_dir" || fail "Cannot finalize clip"
    RUN_DIR=$final_dir
    : > "$RUN_DIR/.complete"
    sync
    sequence_missing=$(
        "$python_bin" -c 'import json,sys; print(json.load(open(sys.argv[1])).get("sequence_missing", 0))' \
            "$RUN_DIR/capture.json" 2>/dev/null || printf '%s' unknown
    )
    mipi_errors_max=$(
        "$python_bin" -c 'import json,sys; print(json.load(open(sys.argv[1])).get("mipi_errors_max", 0))' \
            "$RUN_DIR/capture.json" 2>/dev/null || printf '%s' unknown
    )
    case "$sequence_missing:$mipi_errors_max" in
        *[!0-9:]*) show_status "SAVED ${clip_number}" "$base_name" success 3 ;;
        0:0) show_status "SAVED ${clip_number}" "$base_name | no transport gaps" success 3 ;;
        0:*) show_status "SAVED WITH WARNING ${clip_number}" "MIPI errors: $mipi_errors_max" warning 5 ;;
        *) show_status "SAVED WITH GAPS ${clip_number}" "missing: $sequence_missing | MIPI: $mipi_errors_max" warning 5 ;;
    esac
    exit 0
fi

failed_dir="$DATA_ROOT/${base_name}.failed"
mv "$RUN_DIR" "$failed_dir" 2>/dev/null || true
RUN_DIR=$failed_dir
failure_reason=$(
    "$python_bin" -c 'import json,sys; print(json.load(open(sys.argv[1])).get("health_failure", ""))' \
        "$RUN_DIR/capture.json" 2>/dev/null || true
)
case "$failure_reason" in
    mipi_error_limit) fail "MIPI unstable; check camera FPC" ;;
    temperature_limit) fail "Sensor too hot; cool before retry" ;;
    *) fail "Clip ${clip_number}, code $result" ;;
esac
