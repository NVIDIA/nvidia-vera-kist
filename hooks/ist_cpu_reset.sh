#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: Apache-2.0

# How this board resets its CPUs and which MCTP EIDs must come back,
# from /etc/ist/platform.env. Sourced by ist_hook.sh for sys_rst and
# cak_bypass; provides ist_reset_cpus and ist_expected_eids for
# ist_mctp_common.sh and list_present_cpus for ist_cak_common.sh.

# shellcheck disable=SC2046 # gpiofind output is "<chip> <line>" on purpose

# shellcheck source=/dev/null
. /etc/ist/platform.env || exit 1

# The timing defaults are set by ist_platform_env.py.
TIMEOUT_S=$IST_SHDN_TIMEOUT_S

FRU_SERVICE="xyz.openbmc_project.FruDevice"
FRU_SUBTREE="/xyz/openbmc_project/FruDevice"
MAPPER_SERVICE="xyz.openbmc_project.ObjectMapper"
MAPPER_OBJECT="/xyz/openbmc_project/object_mapper"
MAPPER_IFACE="xyz.openbmc_project.ObjectMapper"

# dvar <domain> <field>
function dvar() {
    local v="IST_D$1_$2"
    echo "${!v}"
}

# domain_present <domain> <BOOT|PRE_RST>
function domain_present() {
    [ "$(dvar "$1" DETECT)" = 1 ] || return 0
    gpiofind "$(dvar "$1" "$2_GPIO")" >/dev/null 2>&1
}

# First FRU whose product name contains one of the domain's match strings.
function detect_product() {
    local d=$1 path name k n pat="" v

    n=$(dvar "$d" FRU_MATCH_COUNT)
    for path in $(busctl call "$MAPPER_SERVICE" "$MAPPER_OBJECT" \
            "$MAPPER_IFACE" GetSubTreePaths sias "$FRU_SUBTREE" 0 1 \
            "$FRU_SERVICE" 2>/dev/null | grep -oE '"[^"]+"' | tr -d '"'); do
        name=$(busctl get-property "$FRU_SERVICE" "$path" "$FRU_SERVICE" \
            PRODUCT_PRODUCT_NAME 2>/dev/null | awk -F'"' '{print $2}')
        for ((k = 0; k < n; k++)); do
            v="IST_D${d}_FRU_MATCH_$k"
            case "$name" in
                *"${!v}"*)
                    pat=${!v}
                    echo "Matched '$name' on $path" >&2
                    break 2
                    ;;
            esac
        done
    done

    echo "$pat"
}

function domain_product() {
    local p
    p=$(dvar "$1" PRODUCT)
    if [ -n "$p" ]; then
        echo "$p"
        return
    fi
    detect_product "$1"
}

# One line per present domain: "<cpu>:<eid> ...".
function expected_cpus() {
    local d cpu product absent v line
    for d in $IST_DOMAINS; do
        domain_present "$d" PRE_RST || continue
        product=""
        if [ -n "$(dvar "$d" FRU_MATCH_COUNT)" ]; then
            product=$(domain_product "$d")
        fi
        line=""
        for cpu in $(dvar "$d" CPUS); do
            absent="IST_CPU${cpu}_ABSENT_ON"
            if [ -n "${!absent}" ] && [ "$product" = "${!absent}" ]; then
                continue
            fi
            v="IST_CPU${cpu}_EID"
            line="${line:+$line }$cpu:${!v}"
        done
        echo "$line"
    done
}

function list_present_cpus() {
    local line e
    while read -r line; do
        for e in $line; do
            echo "${e%%:*}"
        done
    done < <(expected_cpus)
}

function ist_expected_eids() {
    local line e out
    while read -r line; do
        out=""
        for e in $line; do
            out="${out:+$out }${e#*:}"
        done
        echo "$out"
    done < <(expected_cpus)
}

function reset_domain() {
    local d=$1 pre shdn sys
    pre=$(dvar "$d" PRE_RST_GPIO)
    shdn=$(dvar "$d" SHDN_OK_GPIO)
    sys=$(dvar "$d" SYS_RST_GPIO)

    if ! gpioset $(gpiofind "$pre")=0; then
        echo "Failed to assert $pre"
        return 1
    fi

    current_time=$(date +%s)
    timeout_time=$((current_time + TIMEOUT_S))
    success=0
    while [ "$current_time" -lt "$timeout_time" ]; do
        if shdn_ok_l=$(gpioget $(gpiofind "$shdn")); then
            if [ "$shdn_ok_l" -eq 0 ]; then
                success=1
                break
            fi
        fi
        sleep "$IST_SHDN_POLL_S"
        current_time=$(date +%s)
    done

    if ! gpioset $(gpiofind "$pre")=1; then
        echo "Failed to deassert $pre"
        return 1
    fi

    if [ "$success" -eq 0 ]; then
        echo "$shdn failed to assert in $TIMEOUT_S seconds"
        return 1
    fi

    echo "Received $shdn. Toggling $sys"
    if ! gpioset $(gpiofind "$sys")=0; then
        echo "Failed to assert $sys"
        return 1
    fi
    sleep "$IST_SYS_RST_HOLD_S"
    if ! gpioset $(gpiofind "$sys")=1; then
        echo "Failed to deassert $sys"
        return 1
    fi

    return 0
}

# Domains whose lines this run drove, for the exit trap.
RESET_DOMAINS=""

function cleanup_gpios() {
    local d
    for d in $RESET_DOMAINS; do
        gpioset $(gpiofind "$(dvar "$d" PRE_RST_GPIO)")=1 2>/dev/null
        gpioset $(gpiofind "$(dvar "$d" SYS_RST_GPIO)")=1 2>/dev/null
    done
}

# The reset lines belong to x86-power-control, so ask it for its CPU-only
# reset and wait for CpuResetIndicator (raw, active low) to go 0 then 1.
HOST_SERVICE="xyz.openbmc_project.State.Host"
HOST_OBJECT="/xyz/openbmc_project/state/host0"

function cpu_reset_indicator() {
    busctl get-property "$HOST_SERVICE" "$HOST_OBJECT" \
        xyz.openbmc_project.State.Gpio CpuResetIndicator 2>/dev/null |
    awk '{print $2}'
}

# wait_for_reset_indicator <value> <what>
function wait_for_reset_indicator() {
    local deadline
    deadline=$(($(date +%s) + TIMEOUT_S))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if [ "$(cpu_reset_indicator)" = "$1" ]; then
            return 0
        fi
        sleep 0.1
    done
    echo "Timeout after ${TIMEOUT_S}s waiting for the CPUs to $2"
    return 1
}

function host_warm_reboot() {
    echo "Requesting ForceWarmReboot to reset the CPUs"
    if ! busctl set-property "$HOST_SERVICE" "$HOST_OBJECT" "$HOST_SERVICE" \
        RequestedHostTransition s \
        xyz.openbmc_project.State.Host.Transition.ForceWarmReboot; then
        echo "Failed to request ForceWarmReboot"
        return 1
    fi
    wait_for_reset_indicator 0 "enter reset" || return 1
    echo "CPUs entered reset, waiting for them to be released"
    wait_for_reset_indicator 1 "leave reset"
}

function ist_reset_cpus() {
    local d name count=0

    if [ "$IST_RESET_METHOD" = hostWarmReboot ]; then
        host_warm_reboot
        return
    fi

    RESET_DOMAINS=""
    for d in $IST_DOMAINS; do
        name=$(dvar "$d" NAME)
        if ! domain_present "$d" PRE_RST; then
            echo "$name not present, skipping"
            continue
        fi

        RESET_DOMAINS="$RESET_DOMAINS $d"
        echo "Resetting $name"
        if ! reset_domain "$d"; then
            return 1
        fi
        count=$((count + 1))
    done

    if [ "$count" -eq 0 ]; then
        echo "No boards found to reset"
        return 1
    fi

    return 0
}

# Cached here because callers use command substitution.
for _d in $IST_DOMAINS; do
    if [ -n "$(dvar "$_d" FRU_MATCH_COUNT)" ]; then
        printf -v "IST_D${_d}_PRODUCT" '%s' "$(detect_product "$_d")"
    fi
done
unset _d

trap cleanup_gpios EXIT
trap 'exit 1' INT TERM
