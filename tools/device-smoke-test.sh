#!/bin/bash
# SPDX-FileCopyrightText: 2026 RAWfish Contributors
# SPDX-License-Identifier: BSD-3-Clause
#
# ProCam smoke test on a phone, run from the PC over SSH (developer mode,
# SSH key for defaultuser). It checks that the installed build works and
# measures it, so a regression shows up before anyone tries the build:
#
#   1. the package (optionally installs an RPM first) and that the plugin,
#      app and helper resolve all their symbols;
#   2. the Camera2 helper: camera list, a RAW frame from every camera with
#      RAW output (checked for damage as ProCam checks it before merging) and
#      a 3 s recording;
#   3. ProCam itself: cold starts (time to the first viewfinder frame), photos
#      taken through the shutter (shutter and merge times) and a recording.
#
# Part 3 needs the phone unlocked. It closes ProCam, enables the D-Bus shutter
# (/apps/procam/smokeTestCapture) for the run only, and deletes the photos and
# videos it took unless --keep is given. Timings are appended to
# build/smoke-history.tsv so builds can be compared.
#
# Usage: tools/device-smoke-test.sh [-d USER@HOST] [--install RPM] [--runs N]
#        [--photos N] [--no-app] [--keep]
#   -d USER@HOST   the phone (default: $PROCAM_DEVICE or defaultuser@192.168.2.15)
#   --install RPM  install this package first (pkcon install-local)
#   --runs N       cold starts to time (default 3)
#   --photos N     photos to take in ProCam (default 2)
#   --no-app       skip part 3 (phone may stay locked)
#   --keep         keep the photos and videos taken

set -u

device=${PROCAM_DEVICE:-defaultuser@192.168.2.15}
rpm=""
runs=3
photos=2
app_tests=1
keep=0
while [ $# -gt 0 ]; do
    case "$1" in
        -d) device=$2; shift 2 ;;
        --install) rpm=$2; shift 2 ;;
        --runs) runs=$2; shift 2 ;;
        --photos) photos=$2; shift 2 ;;
        --no-app) app_tests=0; shift ;;
        --keep) keep=1; shift ;;
        -h|--help) sed -n '5,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

repo=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/procam-smoke.XXXXXX")
remote_dir=/tmp/procam-smoke
helper=/usr/libexec/procam/sfos-camera2-probe
log='$HOME/Pictures/ProCam/procam-capture.log'
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=8 -o ControlMaster=auto
          -o "ControlPath=$work/ssh-%C" -o ControlPersist=300)

failures=0
results=()
pass() { echo "PASS  $1${2:+  ($2)}"; results+=("PASS  $1${2:+  ($2)}"); }
fail() { echo "FAIL  $1${2:+  ($2)}"; results+=("FAIL  $1${2:+  ($2)}"); failures=$((failures + 1)); }
note() { echo "      $*"; }

# Commands on the phone, with the session bus a login shell over SSH lacks.
remote() {
    ssh "${ssh_opts[@]}" "$device" \
        "export DBUS_SESSION_BUS_ADDRESS=\${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/\$(id -u)/dbus/user_bus_socket}; $1"
}
fetch() { scp -q "${ssh_opts[@]}" "$device:$1" "$2"; }

json() { # json FILE EXPRESSION: a Python expression over the parsed file `d`
    python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print($2)" "$1" 2>/dev/null
}

median() { # median of the numbers given, empty if none
    [ $# -gt 0 ] && printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'
}

field() { # field LINE KEY: KEY=value from a log line
    sed -n "s/.*[ ]$2=\([^ ]*\).*/\1/p" <<<"$1" | head -n 1
}

log_lines() { remote "wc -l < $log 2>/dev/null || echo 0" | tr -d ' '; }
log_since() { remote "tail -n +$(($1 + 1)) $log 2>/dev/null"; }

# Waits until a line matching PATTERN appears after line START of the log;
# prints it. TIMEOUT in seconds.
wait_log() {
    local start=$1 pattern=$2 timeout=$3 waited=0 line
    while [ "$waited" -lt $((timeout * 2)) ]; do
        line=$(log_since "$start" | grep -E "$pattern" | tail -n 1)
        if [ -n "$line" ]; then echo "$line"; return 0; fi
        sleep 0.5
        waited=$((waited + 1))
    done
    return 1
}

# The bracketed letter keeps the patterns from matching the shell that runs
# them, whose command line contains them.
stop_app() {
    remote "pkill -TERM -f '/usr/bin/procam( |\$)' 2>/dev/null; for i in \$(seq 20); do
                pgrep -f '/usr/bin/proca[m]( |\$)|sfos-camera2-prob[e]' >/dev/null || exit 0; sleep 0.5; done
            pkill -KILL -f 'sfos-camera2-prob[e]' 2>/dev/null; true"
    sleep 1
}

launch_app() {
    remote "dbus-send --session --type=method_call --dest=com.procam.camera / com.procam.camera.ui.showViewfinder variant:string:smoke-test"
}

keep_display_on() {
    remote "dbus-send --system --type=method_call --dest=com.nokia.mce /com/nokia/mce/request com.nokia.mce.request.req_display_state_on
            dbus-send --system --type=method_call --dest=com.nokia.mce /com/nokia/mce/request com.nokia.mce.request.req_display_blanking_pause" >/dev/null 2>&1
    [ "$app_tests" = 1 ] && unlock_screen
    true
}

tklock_mode() {
    remote "dbus-send --system --print-reply --dest=com.nokia.mce /com/nokia/mce/request com.nokia.mce.request.get_tklock_mode" \
        | sed -n 's/.*string "\(.*\)"/\1/p'
}

# The lock screen a woken display shows is dismissed when no security code is
# pending (the device lock is open); a code is never bypassed.
unlock_screen() {
    [ "$(tklock_mode)" = locked ] || return 0
    local devicelock
    devicelock=$(remote "dbus-send --system --print-reply --dest=org.nemomobile.devicelock /devicelock org.nemomobile.lipstick.devicelock.state" \
                 | sed -n 's/.*int32 \([0-9]*\).*/\1/p')
    if [ "$devicelock" = 0 ]; then
        remote "dbus-send --system --type=method_call --dest=com.nokia.mce /com/nokia/mce/request com.nokia.mce.request.req_tklock_mode_change string:unlocked" >/dev/null 2>&1
        sleep 1
    fi
    [ "$(tklock_mode)" != locked ]
}

press_shutter() {
    remote "dbus-send --session --print-reply --dest=com.procam.camera / com.procam.camera.ui.capture" 2>&1 | grep -q 'boolean true'
}

saved_dconf=()
restore() {
    if [ ${#saved_dconf[@]} -gt 0 ]; then
        for entry in "${saved_dconf[@]}"; do
            key=${entry%%=*}; value=${entry#*=}
            if [ -z "$value" ]; then remote "dconf reset $key"; else remote "dconf write $key \"$value\""; fi
        done
    fi
    if [ "$keep" = 0 ] && [ "$app_tests" = 1 ]; then
        remote "[ -e $remote_dir/marker ] && find \$HOME/Pictures \$HOME/Videos /run/media/\$USER -newer $remote_dir/marker \
                    -type f -name 'ProCam_*' ! -name 'ProCam_camera2_compatibility_*' -delete 2>/dev/null; true"
    fi
    remote "rm -rf $remote_dir" >/dev/null 2>&1
    ssh "${ssh_opts[@]}" -O exit "$device" >/dev/null 2>&1
    rm -rf "$work"
}
trap restore EXIT

save_dconf() { # remembers a key's value (empty: unset) for restore()
    saved_dconf+=("$1=$(remote "dconf read $1")")
}

echo "== ProCam smoke test on $device"
if ! remote true; then
    echo "Cannot reach $device over SSH (developer mode, SSH key for the user)." >&2
    exit 2
fi
remote "mkdir -p $remote_dir"

# 1. Package and libraries ---------------------------------------------------
if [ -n "$rpm" ]; then
    stop_app
    scp -q "${ssh_opts[@]}" "$rpm" "$device:$remote_dir/" || { fail install "copy failed"; exit 1; }
    if remote "pkcon -y -p install-local $remote_dir/$(basename "$rpm")" > "$work/install.log" 2>&1; then
        pass install "$(basename "$rpm")"
    else
        fail install "pkcon install-local failed; see below"
        tail -n 5 "$work/install.log"
        note "If it needs root: ssh in, run devel-su pkcon install-local $remote_dir/$(basename "$rpm"),"
        note "then run this script again without --install."
        exit 1
    fi
fi
version=$(remote "rpm -q procam" 2>/dev/null)
system=$(remote "sed -n 's/^VERSION_ID=//p' /etc/sailfish-release; sed -n 's/^NAME=//p' /etc/hw-release" | tr '\n' ' ')
note "package: ${version:-not installed}"
note "system:  $system"
[ -n "$rpm" ] && [ "$version" != "$(basename "$rpm" .rpm)" ] && fail "installed version" "expected $(basename "$rpm" .rpm)"

unresolved=$(remote "for f in /usr/bin/procam /usr/lib64/qt5/qml/com/procam/camera/*.so $helper; do
        if command -v ldd >/dev/null; then ldd -r \"\$f\" 2>&1; else
            LD_TRACE_LOADED_OBJECTS=1 LD_WARN=1 LD_BIND_NOW=1 /lib/ld-linux-aarch64.so.1 \"\$f\" 2>&1; fi \
        | grep -E 'undefined symbol|not found' | sed \"s|^|\$(basename \$f): |\"; done")
if [ -z "$unresolved" ]; then pass "libraries resolve"; else fail "libraries resolve" "$(echo "$unresolved" | head -n 3)"; fi

# 2. Camera2 helper ----------------------------------------------------------
stop_app
start=$(date +%s%N)
if remote "$helper" > "$work/probe.json" 2> "$work/probe.err" && [ "$(json "$work/probe.json" "d['status']")" = ok ]; then
    probe_ms=$((($(date +%s%N) - start) / 1000000))
    cameras=$(json "$work/probe.json" "' '.join(c['id'] for c in d['cameras'] if c.get('status') == 'ok')")
    pass "camera probe" "cameras: $cameras; ${probe_ms} ms"
else
    probe_ms=""
    fail "camera probe" "$(tail -n 2 "$work/probe.err")"
    cameras=""
fi

checker="$repo/build/host-rawframecheck/rawframecheck"
if [ ! -x "$checker" ] && command -v qmake-qt5 >/dev/null; then
    mkdir -p "$repo/build/host-rawframecheck"
    (cd "$repo/build/host-rawframecheck" && qmake-qt5 ../../tools/rawframecheck/rawframecheck.pro >/dev/null && make -s >/dev/null 2>&1)
fi
for id in $cameras; do
    size=$(json "$work/probe.json" "max(((o['width'], o['height']) for c in d['cameras'] if c['id'] == '$id' for o in c.get('raw_outputs', []) if o.get('format') == 'RAW16'), key=lambda s: s[0] * s[1], default=None)")
    [ -z "$size" ] || [ "$size" = None ] && continue
    size=$(tr -d '() ' <<<"$size" | tr ',' 'x')
    if ! remote "$helper --capture --camera $id --size $size --timeout 15 --output $remote_dir/raw$id" > "$work/capture$id.out" 2>&1; then
        fail "RAW capture camera $id" "$(tail -n 1 "$work/capture$id.out")"
        continue
    fi
    fetch "$remote_dir/raw$id.json" "$work/" && fetch "$remote_dir/raw$id.raw16" "$work/"
    remote "rm -f $remote_dir/raw$id.*"
    if [ ! -x "$checker" ]; then
        pass "RAW capture camera $id" "$size; frame not checked: build tools/rawframecheck"
    elif check=$("$checker" "$work/raw$id.json"); then
        pass "RAW capture camera $id" "${check##*: }"
    else
        fail "RAW frame camera $id" "${check##*: }"
    fi
    rm -f "$work/raw$id.raw16"
done

if remote "$helper --record-video --camera 0 --seconds 3 --audio off --output $remote_dir/video.mp4" > "$work/record.json" 2> "$work/record.err" \
        && [ "$(json "$work/record.json" "d['status']")" = ok ]; then
    # The file's figures. The clip is shorter than --seconds: the session's
    # start-up counts toward it.
    pass "helper recording" "$(json "$work/record.json" "(lambda f: '%sx%s %s, %.1f s, %d frames, %.1f fps, %.1f Mbit/s, %d stale' % (f['width'], f['height'], f['codec'], f['seconds'], f['frames'], f['fps'], f['bitrate'] / 1e6, f['stale_frames']))(d['files'][-1])")"
else
    fail "helper recording" "$(tail -n 1 "$work/record.err")"
fi
remote "rm -f $remote_dir/video.mp4"

# 3. ProCam ------------------------------------------------------------------
# Merge stages in the photo's sidecar (raw_pipeline.timing_ms).
stages="('load', 'align', 'merge', 'super_resolution', 'finish')"
# The finish stage's steps, in the order of FinishStats::stageMs.
finish_steps="('highlights', 'demosaic', 'chroma-denoise', 'tone', 'colour', 'sharpen')"
startups=(); adopted=0; first_probe=""; exposed=(); started=(); renders=(); waits=(); merges=()
video_result=""
if [ "$app_tests" = 1 ]; then
    keep_display_on
    if ! unlock_screen; then
        fail "app tests" "the phone asks for its security code: unlock it, or run with --no-app"
        app_tests=0
    fi
fi
if [ "$app_tests" = 1 ]; then
    save_dconf /apps/procam/smokeTestCapture
    save_dconf /apps/procam/captureMode
    remote "dconf write /apps/procam/smokeTestCapture true; dconf write /apps/procam/captureMode \"'image'\"; touch $remote_dir/marker"

    for run in $(seq "$runs"); do
        stop_app
        keep_display_on
        mark=$(log_lines)
        launch_app
        if line=$(wait_log "$mark" 'timing first-frame' 25); then
            ms=$(field "$line" since_app_start_ms)
            lines=$(log_since "$mark")
            probe=$(grep -E 'timing probe (cached|ran)' <<<"$lines" | tail -n 1 | awk '{print $4}')
            grep -q 'timing preview-adopted' <<<"$lines" && adopted=$((adopted + 1))
            [ "$run" = 1 ] && first_probe=$probe
            note "start $run: first frame ${ms} ms after launch (probe $probe$(grep -q 'timing preview-adopted' <<<"$lines" && echo ', prelaunched helper'))"
            # Starts after the first re-probe a new install needs are compared.
            if [ "$run" -gt 1 ] || [ "$probe" = cached ]; then startups+=("$ms"); fi
        else
            fail "app start $run" "no viewfinder frame within 25 s"
        fi
    done
    [ ${#startups[@]} -gt 0 ] && pass "app start" "median $(median "${startups[@]}") ms to the first frame over ${#startups[@]} starts"

    sleep 2 # exposure settles
    for shot in $(seq "$photos"); do
        keep_display_on
        mark=$(log_lines)
        if ! press_shutter; then
            fail "photo $shot" "the D-Bus shutter was refused (viewfinder not on screen?)"
            break
        fi
        if line=$(wait_log "$mark" 'timing capture-exposed' 20); then
            exposed+=("$(field "$line" since_command_ms)")
            line=$(log_since "$mark" | grep 'timing capture-started' | tail -n 1)
            [ -n "$line" ] && started+=("$(field "$line" since_command_ms)")
        fi
        if line=$(wait_log "$mark" 'timing (burst-render|raw-render)' 90); then
            if [ "$(field "$line" ok)" = 0 ]; then
                fail "photo $shot" "merge failed: $(log_since "$mark" | grep -iE 'damaged|failed' | tail -n 1 | cut -c 21-)"
                continue
            fi
            render=$(field "$line" render_ms)
            renders+=("$render")
            wait=$(field "$line" wait_ms); [ -n "$wait" ] && waits+=("$wait")
            sleep 1
            sidecar=$(remote "find \$HOME/Pictures \$HOME/Videos /run/media/\$USER -newer $remote_dir/marker -name 'ProCam_*.json' \
                              ! -name '*_burst*' ! -name 'ProCam_camera2_compatibility_*' 2>/dev/null | xargs -r ls -t | head -n 1")
            if [ -n "$sidecar" ] && fetch "$sidecar" "$work/photo$shot.json"; then
                merge=$(json "$work/photo$shot.json" "int(sum(d['raw_pipeline']['timing_ms'].get(k, 0) for k in $stages))")
                [ -n "$merge" ] && merges+=("$merge")
                note "photo $shot: $(json "$work/photo$shot.json" "(lambda p, t: '%s frames, %d ms: %s; slowest finishing steps: %s%s' % (p.get('frames'), sum(t.get(k, 0) for k in $stages), ', '.join('%s %d' % (k.replace('super_resolution', 'super-res'), t.get(k, 0)) for k in $stages), ', '.join('%s %d' % kv for kv in sorted(zip($finish_steps, t.get('finish_stages', [])), key=lambda kv: -kv[1])[:3]), ', %d damaged' % p['damaged_frames'] if p.get('damaged_frames') else ''))(d['raw_pipeline'], d['raw_pipeline']['timing_ms'])")"
            fi
            feedback=$([ ${#exposed[@]} -gt 0 ] && echo "${exposed[${#exposed[@]}-1]}")
            pass "photo $shot" "shutter feedback ${feedback:-?} ms, photo ready ${render} ms later"
        else
            fail "photo $shot" "no finished photo within 90 s"
        fi
    done

    keep_display_on
    remote "dconf write /apps/procam/captureMode \"'video'\""
    sleep 4 # the session restarts with the encoder stream
    if press_shutter; then
        sleep 3
        press_shutter
        sleep 3
        video=$(remote "find \$HOME/Videos \$HOME/Pictures /run/media/\$USER -newer $remote_dir/marker -name 'ProCam_*.mp4' 2>/dev/null | xargs -r ls -t | head -n 1")
        size=$([ -n "$video" ] && remote "stat -c %s '$video'")
        if [ -n "$video" ] && [ "${size:-0}" -gt 100000 ]; then
            video_result="$((size / 1024)) KB"
            pass "app recording" "$(basename "$video"), $video_result"
        else
            fail "app recording" "no video file after a 3 s recording"
        fi
    else
        fail "app recording" "the D-Bus shutter was refused in video mode"
    fi
    stop_app
fi

# Summary and history --------------------------------------------------------
echo
echo "== Summary: $((${#results[@]} - failures)) passed, $failures failed"
printf '%s\n' "${results[@]}"
startup_median=$(median "${startups[@]+"${startups[@]}"}")
exposed_median=$(median "${exposed[@]+"${exposed[@]}"}")
render_median=$(median "${renders[@]+"${renders[@]}"}")
merge_median=$(median "${merges[@]+"${merges[@]}"}")
echo
echo "   probe ${probe_ms:-?} ms | start ${startup_median:-?} ms | shutter feedback ${exposed_median:-?} ms | photo ready ${render_median:-?} ms (merge ${merge_median:-?} ms)"

history="$repo/build/smoke-history.tsv"
mkdir -p "$repo/build"
[ -s "$history" ] || printf 'date\tversion\tdevice\tfailures\tprobe_ms\tstart_ms\tshutter_ms\tready_ms\tmerge_ms\n' > "$history"
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date +%Y-%m-%dT%H:%M)" "${version:-?}" "$device" "$failures" \
    "${probe_ms:-}" "${startup_median:-}" "${exposed_median:-}" "${render_median:-}" "${merge_median:-}" >> "$history"
echo
echo "   Recent runs ($history):"
tail -n 6 "$history" | column -t -s $'\t' | sed 's/^/   /'

[ "$failures" = 0 ]
