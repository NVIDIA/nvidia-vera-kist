#!/bin/sh
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: Apache-2.0

PROG_NAME="ist_marker"
MARKER_DIR="/tmp/ist/err_marker"
ACTION="$1"
MARKER_NAME="$2"

if [ -z "$ACTION" ] || [ -z "$MARKER_NAME" ]; then
    echo "$PROG_NAME: Usage: ist_marker.sh <create|remove> <MARKER_NAME>" >&2
    exit 1
fi

case "$ACTION" in
    create)
        if ! mkdir -p "$MARKER_DIR"; then
            echo "$PROG_NAME: Failed to create marker directory: $MARKER_DIR" >&2
            exit 1
        fi

        if ! echo asserted > "$MARKER_DIR/$MARKER_NAME"; then
            echo "$PROG_NAME: Failed to write marker: $MARKER_DIR/$MARKER_NAME" >&2
            exit 1
        fi
        ;;
    remove)
        if [ ! -f "$MARKER_DIR/$MARKER_NAME" ]; then
            exit 0
        fi

        PROP=$(busctl --timeout=3 get-property com.nvidia.vera.ist /com/nvidia/vera/ist com.nvidia.vera.ist.State IstInProgress 2>/dev/null)
        PROP_RC=$?

        if [ "$PROP_RC" -eq 0 ] && echo "$PROP" | grep -q "false"; then
            rm -f "$MARKER_DIR/$MARKER_NAME"
        else
            if ! echo deasserted > "$MARKER_DIR/$MARKER_NAME"; then
                echo "$PROG_NAME: Failed to write marker: $MARKER_DIR/$MARKER_NAME" >&2
                exit 1
            fi
        fi
        ;;
    *)
        echo "$PROG_NAME: Unknown action '$ACTION'. Use 'create' or 'remove'." >&2
        exit 1
        ;;
esac
