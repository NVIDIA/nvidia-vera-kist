#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: Apache-2.0

# Board facts for the IST hooks, from the entity-manager IST, ISTDomain and
# USBRCMRecovery records. ist_em_load sets IST_DOMAINS, IST_D<n>_* and
# IST_CPU<i>_EID plus the reset, CAK and timing variables. A domain CPU
# with no USBRCMRecovery record is not fitted and is left out.

EM_SERVICE="xyz.openbmc_project.EntityManager"
EM_CFG="xyz.openbmc_project.Configuration"

# em_paths <type>
function em_paths() {
    busctl call xyz.openbmc_project.ObjectMapper \
        /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper \
        GetSubTreePaths sias /xyz/openbmc_project/inventory 0 1 \
        "$EM_CFG.$1" 2>/dev/null | grep -oE '"[^"]+"' | tr -d '"'
}

# em_get <path> <type> <property>: the bare value; fails if it is absent.
function em_get() {
    local v
    v=$(busctl get-property "$EM_SERVICE" "$1" "$EM_CFG.$2" "$3" \
        2>/dev/null) || return 1
    case "$v" in
        a*) read -r _ _ v <<< "$v" ;;
        s*) v=${v#s \"}; v=${v%\"} ;;
        *) v=${v#* } ;;
    esac
    echo "$v"
}

# em_set <var> <path> <type> <property> [default]
function em_set() {
    local v
    v=$(em_get "$2" "$3" "$4") || v=$5
    printf -v "$1" '%s' "$v"
}

function ist_em_load() {
    local p d c v cpus declared=0

    [ -n "$IST_EM_LOADED" ] && return 0
    p=$(em_paths IST | head -n 1)
    if [ -z "$p" ]; then
        echo "No IST configuration in entity-manager" >&2
        return 1
    fi
    em_set IST_RESET_METHOD "$p" IST ResetMethod gpio
    em_set IST_RESET_COMMAND "$p" IST ResetCommand
    em_set IST_DOT_PREFIX "$p" IST DotObjectPrefix
    em_set IST_REQUIRE_CPUS "$p" IST RequiredCpus
    em_set IST_SHDN_TIMEOUT_S "$p" IST ShutdownOkTimeoutS 10
    em_set IST_SHDN_POLL_S "$p" IST ShutdownOkPollS 0.2
    em_set IST_SYS_RST_HOLD_S "$p" IST SysResetHoldS 5
    em_set IST_EID_TIMEOUT_S "$p" IST EidTimeoutS 45

    # FW_CPU_<i> / HGX_FW_CPU_<i>
    for p in $(em_paths USBRCMRecovery); do
        v=$(em_get "$p" USBRCMRecovery Name) || continue
        c=${v##*_}
        [[ "$c" =~ ^[0-9]+$ ]] || continue
        em_set "IST_CPU${c}_EID" "$p" USBRCMRecovery MctpEID
    done

    IST_DOMAINS=""
    for p in $(em_paths ISTDomain); do
        d=$(em_get "$p" ISTDomain Index) || continue
        IST_DOMAINS="$IST_DOMAINS $d"
        em_set "IST_D${d}_NAME" "$p" ISTDomain Name
        [ "$(em_get "$p" ISTDomain Detect)" = true ] && v=1 || v=0
        printf -v "IST_D${d}_DETECT" '%s' "$v"
        em_set "IST_D${d}_BOOT_GPIO" "$p" ISTDomain BootGpio
        em_set "IST_D${d}_PRE_RST_GPIO" "$p" ISTDomain PreResetGpio
        em_set "IST_D${d}_SHDN_OK_GPIO" "$p" ISTDomain ShutdownOkGpio
        em_set "IST_D${d}_SYS_RST_GPIO" "$p" ISTDomain SysResetGpio
        cpus=""
        for c in $(em_get "$p" ISTDomain Cpus); do
            declared=$((declared + 1))
            v="IST_CPU${c}_EID"
            [ -n "${!v}" ] && cpus="${cpus:+$cpus }$c"
        done
        printf -v "IST_D${d}_CPUS" '%s' "$cpus"
    done
    # shellcheck disable=SC2086 # one index per word
    IST_DOMAINS=$(printf '%s\n' $IST_DOMAINS | sort -n | tr '\n' ' ')
    if [ -n "$IST_REQUIRE_CPUS" ] && [ "$IST_REQUIRE_CPUS" -gt "$declared" ]; then
        echo "IST RequiredCpus $IST_REQUIRE_CPUS exceeds the $declared CPUs" \
            "the ISTDomain records declare" >&2
        return 1
    fi
    IST_EM_LOADED=1
}
