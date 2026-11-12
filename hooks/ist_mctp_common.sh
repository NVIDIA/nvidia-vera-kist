#!/bin/bash

# Confirm the CPU MCTP endpoints came back after an IST reset.
#
# mctpreactor no longer issues a USB clear halt while IST is running: a clear
# halt resets USB, but RAS is never told it has to reinitialize, so the IST
# interface stays dead. The CPU reset this hook already performs is what
# recovers MCTP, because RAS reinits USB as part of the boot. This wait is
# how we know that worked before letting the run continue, and it applies
# even when no CAK bypass follows, since volatile DOT mode needs the EIDs.
#
# A board's ist_sys_rst.sh defines the two functions below, sources this file
# and calls recover_mctp_eids after its reset.
#
# Required functions:
#   ist_reset_cpus      Resets every CPU the board owns. Returns non-zero on
#                       failure. Called once more if the EIDs do not return.
#   ist_expected_eids   Echoes the EIDs that must be present once the reset
#                       has taken effect, space or newline separated. Called
#                       on each poll so boards that discover what they reset
#                       can report only those.
#
# Parameters:
#   IST_EID_TIMEOUT_S     Wait for the EIDs (45 s from ist_em.sh).
#   IST_POLL_INTERVAL_S   Poll interval (default 1).

MCTP_SERVICE="au.com.codeconstruct.MCTP1"
MCTP_ENDPOINT_PATH="/au/com/codeconstruct/mctp1/networks/1/endpoints"

: "${IST_POLL_INTERVAL_S:=1}"

function mctp_endpoint_present() {
    busctl introspect "$MCTP_SERVICE" "${MCTP_ENDPOINT_PATH}/$1" >/dev/null 2>&1
}

# Returns as soon as every expected EID is up, so a healthy reset pays only
# the time the CPUs genuinely need.
function wait_for_mctp_eids() {
    local start_time deadline eids eid missing

    start_time=$(date +%s)
    deadline=$(( start_time + IST_EID_TIMEOUT_S ))
    missing=" (no EIDs expected yet)"

    echo "Waiting for CPU MCTP endpoints (timeout ${IST_EID_TIMEOUT_S}s)..."
    while [ "$(date +%s)" -lt "$deadline" ]; do
        eids=$(ist_expected_eids)

        if [ -n "$eids" ]; then
            missing=""
            for eid in $eids; do
                if ! mctp_endpoint_present "$eid"; then
                    missing="$missing $eid"
                fi
            done

            if [ -z "$missing" ]; then
                echo "CPU MCTP endpoints present after" \
                    "$(( $(date +%s) - start_time ))s"
                return 0
            fi
        fi

        sleep "$IST_POLL_INTERVAL_S"
    done

    echo "Timeout after ${IST_EID_TIMEOUT_S}s waiting for CPU MCTP endpoints:$missing"
    return 1
}

# Call after the caller's own reset. One more reset is attempted if the EIDs
# did not come back; if that does not recover them either, give up rather
# than keep resetting a CPU that is not responding to it.
function recover_mctp_eids() {
    local fn

    for fn in ist_reset_cpus ist_expected_eids; do
        if ! declare -F "$fn" >/dev/null; then
            echo "$fn is not defined"
            return 1
        fi
    done

    if wait_for_mctp_eids; then
        return 0
    fi

    echo "Resetting the CPUs again to recover MCTP"
    if ! ist_reset_cpus; then
        return 1
    fi

    wait_for_mctp_eids
}
