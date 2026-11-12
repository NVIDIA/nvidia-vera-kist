#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: Apache-2.0

# Board facts for the IST hooks, from the entity-manager ISTDomain records
# and the CPU NvDevice records, or USBRCMRecovery records where there are
# none. ist_em_load sets IST_DOMAINS, IST_D<n>_* and IST_CPU<i>_EID plus
# the reset, CAK and timing variables. A domain CPU with no such record is
# not fitted and is left out (MGX C1 drops CPU 1); the fitted CPUs must all
# be present for the CAK bypass. The CPU record names carry the platform
# prefix (HGX_CPU_0, CPU_0), which the CAK bypass DOT objects share.

EM_SERVICE="xyz.openbmc_project.EntityManager"
EM_CFG="xyz.openbmc_project.Configuration"

# em_paths <type>
function em_paths() {
    busctl call xyz.openbmc_project.ObjectMapper \
        /xyz/openbmc_project/object_mapper xyz.openbmc_project.ObjectMapper \
        GetSubTreePaths sias /xyz/openbmc_project/inventory 0 1 \
        "$EM_CFG.$1" 2>/dev/null | grep -oE '"[^"]+"' | tr -d '"'
}

# em_get <path> <type> <property>: the bare value; returns 1 if the
# record has no such property, 2 if it cannot be read.
function em_get() {
    local v i="$EM_CFG.$2"
    if ! v=$(busctl get-property "$EM_SERVICE" "$1" "$i" "$3" 2>&1); then
        [[ "$v" == *"Unknown interface $i or property $3."* ]] && return 1
        echo "$v" >&2
        return 2
    fi
    case "$v" in
        a*) read -r _ _ v <<< "$v" ;;
        s*) v=${v#s \"}; v=${v%\"} ;;
        *) v=${v#* } ;;
    esac
    echo "$v"
}

# em_set <var> <path> <type> <property> [default]: fails if unreadable.
function em_set() {
    local v rc=0
    v=$(em_get "$2" "$3" "$4") || { rc=$?; v=$5; }
    printf -v "$1" '%s' "$v"
    [ "$rc" -ne 2 ]
}

# shellcheck disable=SC2034 # the hooks that source this file read them
function ist_em_load() {
    local p d c v t e paths cpus prefix="" fitted=0

    [ -n "$IST_EM_LOADED" ] && return 0
    IST_RESET_METHOD=gpio
    IST_RESET_COMMAND=""
    IST_SHDN_TIMEOUT_S=10
    IST_SHDN_POLL_S=0.2
    IST_SYS_RST_HOLD_S=5
    IST_EID_TIMEOUT_S=45

    # CPU_<i> / HGX_CPU_<i> NvDevice of Kind cpu; without one, the
    # FW_CPU_<i> / HGX_FW_CPU_<i> USBRCMRecovery records.
    t=NvDevice e=EID
    paths=$(for p in $(em_paths NvDevice); do
            [ "$(em_get "$p" NvDevice Kind)" = cpu ] && echo "$p"
    done)
    if [ -z "$paths" ]; then
        t=USBRCMRecovery e=MctpEID
        paths=$(em_paths USBRCMRecovery)
    fi
    for p in $paths; do
        v=$(em_get "$p" "$t" Name) || continue
        c=${v##*_}
        [[ "$c" =~ ^[0-9]+$ ]] || continue
        prefix=${v%CPU_*}
        prefix=${prefix%FW_}
        em_set "IST_CPU${c}_EID" "$p" "$t" "$e"
    done
    IST_DOT_PREFIX="/xyz/openbmc_project/DOT/${prefix}IRoT_CPU_"

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
            v="IST_CPU${c}_EID"
            [ -n "${!v}" ] || continue
            cpus="${cpus:+$cpus }$c"
            fitted=$((fitted + 1))
        done
        printf -v "IST_D${d}_CPUS" '%s' "$cpus"
    done
    # shellcheck disable=SC2086 # one index per word
    IST_DOMAINS=$(printf '%s\n' $IST_DOMAINS | sort -n | tr '\n' ' ')
    IST_REQUIRE_CPUS=$fitted
    IST_EM_LOADED=1
}
