#!/bin/bash

# Shared implementation of the IST CAK bypass hook.
#
# A board's ist_cak_bypass.sh sets the parameters below, defines
# list_present_cpus, sources this file and calls ist_cak_main. Keeping the
# sequence here means a board cannot omit a step by accident.
#
# Required parameters:
#   IST_DOT_PREFIX      DOT object path prefix; the CPU index is appended,
#                       e.g. /xyz/openbmc_project/DOT/IRoT_CPU_
#
# Required function:
#   list_present_cpus   Echoes the index of each present CPU, one per line.
#                       Called on every poll rather than once, because on
#                       boards that read the inventory it can lag the reset.
#
# ist_hook.sh also sources ist_cpu_reset.sh and ist_mctp_common.sh first,
# so a missing CPU endpoint gets a recovery reset (recover_mctp_eids).
#
# Optional parameters:
#   IST_REQUIRE_CPUS    If set and fewer CPUs than this are present at
#                       startup, skip the hook and exit 0. Only for boards
#                       whose discovery is immediate and definitive (GPIO
#                       board detect); leave unset where discovery can lag.
#   IST_CAK_TIMEOUT_S   Overall wait for CAK to reach Complete (default 120).
#   IST_ASYNC_TIMEOUT_S Wait for one Bypass call's async result (default 30).
#   IST_POLL_INTERVAL_S Poll interval (default 1).

CAK_DBUS_SERVICE="xyz.openbmc_project.State.Boot.Raw"
CAK_DBUS_OBJECT="/xyz/openbmc_project/state/boot/cak0"
CAK_DBUS_IFACE="xyz.openbmc_project.State.Boot.Progress"

NSM_SERVICE="xyz.openbmc_project.NSM"
DOT_IFACE="com.nvidia.Dot.Action"
ASYNC_STATUS_IFACE="com.nvidia.Async.Status"

: "${IST_CAK_TIMEOUT_S:=120}"
: "${IST_ASYNC_TIMEOUT_S:=30}"
: "${IST_POLL_INTERVAL_S:=1}"

# Missing configuration is fatal rather than silently skipped, so that a new
# board cannot end up running a hook that quietly does nothing.
function check_config() {
    if [ -z "$IST_DOT_PREFIX" ]; then
        echo "IST_DOT_PREFIX is not set"
        return 1
    fi

    if ! declare -F list_present_cpus >/dev/null; then
        echo "list_present_cpus is not defined"
        return 1
    fi

    return 0
}

# Present CPU indices, deduplicated and filtered to integers. On boards that
# discover CPUs from the inventory a socket can appear under more than one
# path, which would otherwise mean acting on the same CPU twice.
function present_cpus() {
    local cpu_index

    for cpu_index in $(list_present_cpus); do
        if [[ "$cpu_index" =~ ^[0-9]+$ ]]; then
            echo "$cpu_index"
        else
            echo "Ignoring non-numeric CPU index '$cpu_index'" >&2
        fi
    done | sort -u
}

# Set by read_cak_progress rather than echoed, so the busctl failure reaches
# the caller instead of dying in a command substitution.
CAK_PROGRESS=""
CAK_PROGRESS_ERROR=""

# A property that cannot be read at all looks exactly like a progress value we
# are not waiting for, and both just time out. Keep them apart.
function read_cak_progress() {
    local reply

    CAK_PROGRESS=""
    CAK_PROGRESS_ERROR=""

    if ! reply=$(busctl get-property "$CAK_DBUS_SERVICE" "$CAK_DBUS_OBJECT" \
            "$CAK_DBUS_IFACE" BootProgressOem 2>&1); then
        CAK_PROGRESS_ERROR=$reply
        return 1
    fi

    # shellcheck disable=SC2001 # strip busctl type and quotes
    CAK_PROGRESS=$(echo "$reply" | sed 's/^s "\(.*\)"$/\1/')
    return 0
}

function send_cak_bypass() {
    local dot_obj="$1"

    echo "Sending CAK Bypass to $dot_obj"
    local reply
    if ! reply=$(busctl call "$NSM_SERVICE" "$dot_obj" "$DOT_IFACE" Bypass 2>&1); then
        echo "Failed to call Bypass on $dot_obj: $reply"
        return 1
    fi

    local async_path
    # shellcheck disable=SC2001 # strip busctl type and quotes
    async_path=$(echo "$reply" | sed 's/^o "\(.*\)"$/\1/')
    if [ -z "$async_path" ]; then
        echo "No async path returned for $dot_obj"
        return 1
    fi

    echo "Polling async status at $async_path"
    local deadline=$(( $(date +%s) + IST_ASYNC_TIMEOUT_S ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        local status
        status=$(busctl get-property "$NSM_SERVICE" "$async_path" \
                "$ASYNC_STATUS_IFACE" Status 2>/dev/null \
            | sed 's/^s "\(.*\)"$/\1/')

        case "$status" in
            *Success)
                echo "CAK Bypass succeeded for $dot_obj"
                return 0
                ;;
            *InProgress)
                sleep "$IST_POLL_INTERVAL_S"
                ;;
            *)
                echo "CAK Bypass failed for $dot_obj: $status"
                return 1
                ;;
        esac
    done

    echo "Timeout waiting for CAK Bypass result on $dot_obj"
    return 1
}

# The sockets whose bypass has already succeeded, and those still waiting on
# one. Both are kept across calls so a retry neither re-sends to a socket that
# is already through nor skips one that has only just been discovered.
CAK_DONE_CPUS=""
CAK_PENDING_CPUS=""

function cak_bypass_done() {
    case " $CAK_DONE_CPUS " in
        *" $1 "*) return 0 ;;
    esac

    return 1
}

function send_cak_bypass_all() {
    local cpu_index still_pending="" discovered

    # Discovery is re-read on every pass because it can lag the reset, so a socket
    # that appears after the first attempt still has to be sent a bypass.
    discovered=$(present_cpus)
    if [ -z "$discovered" ]; then
        # An empty list is only outstanding work while nothing has succeeded.
        # Past that it is a transient discovery failure, not a socket waiting
        # on a bypass it never received.
        if [ -z "$CAK_DONE_CPUS" ]; then
            echo "No CPUs discovered yet"
            return 1
        fi

        return 0
    fi

    # Every socket that still needs one is tried on each pass. Returning at
    # the first failure used to leave the sockets after it waiting on a bypass
    # that was never sent at all.
    for cpu_index in $discovered; do
        if cak_bypass_done "$cpu_index"; then
            continue
        fi

        if send_cak_bypass "${IST_DOT_PREFIX}${cpu_index}"; then
            CAK_DONE_CPUS=${CAK_DONE_CPUS:+$CAK_DONE_CPUS }$cpu_index
        else
            still_pending="$still_pending $cpu_index"
        fi
    done

    CAK_PENDING_CPUS=${still_pending# }
    [ -z "$CAK_PENDING_CPUS" ]
}

function wait_for_cak() {
    local deadline=$(( $(date +%s) + IST_CAK_TIMEOUT_S ))
    local seen_waiting=0
    local reported="" seen

    echo "Waiting for CAK progress (timeout ${IST_CAK_TIMEOUT_S}s)..."
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if read_cak_progress; then
            seen=${CAK_PROGRESS:-<empty>}
        else
            seen="unreadable: $CAK_PROGRESS_ERROR"
        fi

        # On change only, so a full timeout reports every value the hook
        # actually saw without printing a line per poll.
        if [ "$seen" != "$reported" ]; then
            echo "CAK progress: $seen"
            reported=$seen
        fi

        if [ "$CAK_PROGRESS" = "Complete" ]; then
            echo "CAK reached Complete"
            return 0
        fi

        if [ "$CAK_PROGRESS" = "Waiting" ] && [ "$seen_waiting" -eq 0 ]; then
            echo "CAK reached Waiting, sending bypass..."
            seen_waiting=1
        fi

        # The published value is an aggregate: it reads Waiting only while
        # every socket is waiting and falls back to EarlyBoot as soon as one
        # of them gets through. Retrying only while it reads Waiting therefore
        # abandoned the remaining sockets the moment the first one succeeded,
        # which is how a single failed bypass wedged a whole run. Once any
        # socket has reached the gate, keep retrying the rest until CAK
        # completes rather than stopping at the first pass that sent to every
        # socket it knew about, because that list can still grow. A socket
        # already through is skipped, so a pass with nothing left to do costs
        # only discovery.
        if [ "$seen_waiting" -eq 1 ]; then
            if ! send_cak_bypass_all; then
                echo "CAK Bypass still pending for socket(s):" \
                    "$CAK_PENDING_CPUS"
            fi
        fi

        sleep "$IST_POLL_INTERVAL_S"
    done

    if [ -n "$CAK_PENDING_CPUS" ]; then
        echo "Timeout waiting for CAK Complete; bypass never succeeded for" \
            "socket(s): $CAK_PENDING_CPUS"
    else
        echo "Timeout waiting for CAK Complete"
    fi
    return 1
}

# Boards with immediate, definitive discovery use this to opt out on
# partially populated systems.
function should_skip() {
    local count

    if [ -z "$IST_REQUIRE_CPUS" ]; then
        return 1
    fi

    count=$(present_cpus | wc -l)
    if [ "$count" -lt "$IST_REQUIRE_CPUS" ]; then
        echo "Only $count CPU(s) detected, skipping CAK bypass"
        return 0
    fi

    return 1
}

function ist_cak_main() {
    if ! check_config; then
        exit 1
    fi

    if should_skip; then
        exit 0
    fi

    # The DOT objects the bypass is sent to are backed by the CPU MCTP
    # endpoints. If one does not come back, mctpreactor issues a USB clear
    # halt, which on a CPU in IST mode resets XUSB and takes both sockets
    # down, so fail here rather than let a test start against it. The IST
    # service can run this hook without ist_sys_rst.sh, so the recovery
    # reset has to be reachable from here too.
    if ! recover_mctp_eids; then
        exit 1
    fi

    wait_for_cak
}
