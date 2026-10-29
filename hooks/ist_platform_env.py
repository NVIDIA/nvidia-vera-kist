#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# All rights reserved. SPDX-License-Identifier: Apache-2.0
"""Validate the "hookConfig" object of platform_cfg.json and write
platform.env.

Fails when the config has no "hookConfig" object: the hooks exit 1 without
platform.env, so a board that installs them needs one.
"""

import argparse
import json
import re
import sys

import jsonschema

SAFE = re.compile(r"[A-Za-z0-9_./ -]+")
INVENTORY_ID = re.compile(r"[A-Za-z0-9_]+")


def fail(msg):
    sys.exit(f"ist_platform_env: {msg}")


def env_lines(hc):
    out = {}
    domains = hc["domains"]
    out["IST_DOMAINS"] = " ".join(str(i) for i in range(len(domains)))

    seen = set()
    for i, dom in enumerate(domains):
        p = f"IST_D{i}_"
        out[p + "NAME"] = dom["name"]
        out[p + "DETECT"] = "1" if dom.get("detect") else "0"
        out[p + "BOOT_GPIO"] = dom["istBootGpio"]
        for key, var in (
            ("preResetGpio", "PRE_RST_GPIO"),
            ("shutdownOkGpio", "SHDN_OK_GPIO"),
            ("sysResetGpio", "SYS_RST_GPIO"),
        ):
            if key in dom:
                out[p + var] = dom[key]

        matches = dom.get("fruProductMatch", [])
        if matches:
            out[p + "FRU_MATCH_COUNT"] = str(len(matches))
            for k, m in enumerate(matches):
                out[f"{p}FRU_MATCH_{k}"] = m

        cpus = dom.get("cpus", [])
        if cpus:
            out[p + "CPUS"] = " ".join(str(c["index"]) for c in cpus)
        for cpu in cpus:
            idx = cpu["index"]
            if idx in seen:
                fail(f"CPU index {idx} is listed twice")
            seen.add(idx)
            out[f"IST_CPU{idx}_EID"] = str(cpu["eid"])
            if "absentOnProduct" in cpu:
                if cpu["absentOnProduct"] not in matches:
                    fail(f"CPU {idx}: absentOnProduct not in fruProductMatch")
                out[f"IST_CPU{idx}_ABSENT_ON"] = cpu["absentOnProduct"]

    out["IST_RESET_METHOD"] = hc.get("resetMethod", "gpio")
    if "resetCommand" in hc:
        out["IST_RESET_COMMAND"] = hc["resetCommand"]

    cak = hc.get("cakBypass")
    if cak:
        out["IST_DOT_PREFIX"] = cak["dotObjectPrefix"]
        if "requiredCpus" in cak:
            if cak["requiredCpus"] > len(seen):
                fail(
                    f"requiredCpus {cak['requiredCpus']} exceeds the "
                    f"{len(seen)} CPU(s) declared"
                )
            out["IST_REQUIRE_CPUS"] = str(cak["requiredCpus"])

    t = hc.get("timings", {})
    out["IST_SHDN_TIMEOUT_S"] = str(t.get("shutdownOkTimeoutS", 10))
    out["IST_SHDN_POLL_S"] = str(t.get("shutdownOkPollS", 0.2))
    out["IST_SYS_RST_HOLD_S"] = str(t.get("sysResetHoldS", 5))
    out["IST_EID_TIMEOUT_S"] = str(t.get("eidTimeoutS", 45))

    lines = []
    for key, val in out.items():
        if not SAFE.fullmatch(val):
            fail(f"{key}: unsafe value {val!r}")
        lines.append(f"{key}='{val}'\n")
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--schema", required=True)
    ap.add_argument("config")
    ap.add_argument("output")
    args = ap.parse_args()

    try:
        with open(args.config, encoding="utf-8") as f:
            cfg = json.load(f)
        with open(args.schema, encoding="utf-8") as f:
            schema = json.load(f)
    except (OSError, ValueError) as e:
        fail(str(e))

    if not isinstance(cfg, dict):
        fail("config is not an object")
    # The daemon will not start without it, so fail the build instead.
    inv = cfg.get("softwareInventoryId")
    if not isinstance(inv, str) or not INVENTORY_ID.fullmatch(inv):
        fail(f"{args.config}: softwareInventoryId missing or invalid")
    if "hookConfig" not in cfg:
        fail(f"{args.config}: no hookConfig object")

    try:
        jsonschema.validate(cfg["hookConfig"], schema)
    except jsonschema.ValidationError as e:
        where = "".join(f"[{p!r}]" for p in e.absolute_path)
        fail(f"{args.config}: hookConfig{where}: {e.message}")

    lines = env_lines(cfg["hookConfig"])
    with open(args.output, "w", encoding="utf-8") as f:
        f.writelines(lines)


if __name__ == "__main__":
    main()
