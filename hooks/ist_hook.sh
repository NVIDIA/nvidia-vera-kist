#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: Apache-2.0

# Board-independent IST hook logic, sourced by the per-hook wrappers so
# $0 stays the wrapper. Board facts come from platform.env, generated at
# build time from the "hookConfig" object of platform_cfg.json.
# Usage: . ist_hook.sh boot_enable|boot_disable|sys_rst|cak_bypass|check_error

# shellcheck disable=SC2046 # gpiofind output is "<chip> <line>" on purpose

HOOKS=/usr/share/ist/hooks

# shellcheck source=/dev/null
. /etc/ist/platform.env || exit 1

STEP=$1
shift

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

function load_reset() {
    # shellcheck source=hooks/ist_cpu_reset.sh
    . "$HOOKS/ist_cpu_reset.sh"
    # shellcheck source=hooks/ist_mctp_common.sh
    . "$HOOKS/ist_mctp_common.sh"
}

# ist_boot <value> <Enabling|Disabling> <set|clear> <enable|disable>
function ist_boot() {
    local d gpio name count=0

    for d in $IST_DOMAINS; do
        gpio=$(dvar "$d" BOOT_GPIO)
        name=$(dvar "$d" NAME)
        if ! domain_present "$d" BOOT; then
            echo "$name not present, skipping"
            continue
        fi

        echo "$2 IST boot on $name"
        if ! gpioset $(gpiofind "$gpio")="$1"; then
            echo "Failed to $3 $gpio"
            exit 1
        fi
        count=$((count + 1))
    done

    if [ "$count" -eq 0 ]; then
        echo "No boards found to $4 IST boot"
        exit 1
    fi
    exit 0
}

function check_error() {
    local marker_dir="/tmp/ist/err_marker" markers

    # A missing or unreadable directory must not cancel the run.
    [ -d "$marker_dir" ] || exit 0
    if ! markers=$(ls -A "$marker_dir"); then
        echo "Failed to read marker directory: $marker_dir" >&2
        exit 0
    fi

    if [ -n "$markers" ]; then
        echo "$markers"
        exit 1
    fi
    exit 0
}

function sys_rst() {
    local skip_cak=false cak

    if [ "$IST_RESET_METHOD" = command ]; then
        # shellcheck disable=SC2086
        if ! $IST_RESET_COMMAND; then
            echo "Failed to gracefully reset"
            exit 1
        fi
        exit 0
    fi

    cak="$(dirname "$0")/ist_cak_bypass.sh"
    if [ "$1" = "--skip-cak" ]; then
        skip_cak=true
    fi

    load_reset

    if ! ist_reset_cpus; then
        exit 1
    fi

    # The reset is what brings USB back, since RAS reinits it on boot.
    if ! recover_mctp_eids; then
        exit 1
    fi

    # Set only when hookConfig.cakBypass is present; a missing script fails.
    if [ "$skip_cak" = false ] && [ -n "$IST_DOT_PREFIX" ]; then
        echo "Running CAK bypass script: $cak"
        if ! "$cak"; then
            echo "CAK bypass failed"
            exit 1
        fi
    else
        echo "Skipping CAK bypass"
    fi
    exit 0
}

case "$STEP" in
    boot_enable) ist_boot 1 Enabling set enable ;;
    boot_disable) ist_boot 0 Disabling clear disable ;;
    check_error) check_error ;;
    sys_rst) sys_rst "$@" ;;
    cak_bypass)
        load_reset
        # shellcheck source=hooks/ist_cak_common.sh
        . "$HOOKS/ist_cak_common.sh"
        ist_cak_main
        ;;
    *)
        echo "Usage: $0 boot_enable|boot_disable|sys_rst|cak_bypass|check_error" >&2
        exit 1
        ;;
esac
