# Nvidia Vera KIST

nvidia-vera-kist is a daemon that orchestrates CPU IST (In-System Test). It
manages the full IST lifecycle — verifying test vector collateral, running
signature checks, coordinating host power-cycle sequences, launching the
`kist_itm` test runner, and archiving results. It also handles test vector
firmware updates by accepting PLDM-wrapped images and managing mount/activation
state.

The daemon runs as a systemd service (`com.nvidia.vera.ist.service`) and exposes
its functionality over D-Bus, which is made available to clients through Redfish
via bmcweb.

## Usage

### Building

```bash
meson setup build
ninja -C build
meson test -C build
```

To set the architecture subdirectory for `kist_itm` lookup:

```bash
meson setup build -Dkist_arch_subdir=aarch64
```

### OpenBMC / Yocto Integration

Add the Yocto recipe (`nvidia-vera-kist_git.bb`) to the appropriate meta-layer
and include it in the image:

```bash
IMAGE_INSTALL:append = " nvidia-vera-kist"
```

Board-specific `.bbappend` files should install:

- `/etc/ist/platform_cfg.json` — runtime configuration
- `/usr/share/ist/kist_itm_verify_key.pem` — ECDSA-P384 public key for
  `kist_itm` signature verification
- Hook scripts referenced by `hookPaths` in the platform configuration, or the
  shared ones from this repository (see [Adding a platform](#adding-a-platform))

### Runtime Configuration

The daemon reads `/etc/ist/platform_cfg.json` at startup:

```json
{
  "hookDirectory": "/usr/share/ist/hooks/",
  "hookPaths": {
    "istBootAssert": "/usr/share/ist/hooks/ist_boot_enable.sh",
    "istBootDeassert": "/usr/share/ist/hooks/ist_boot_disable.sh",
    "resetSystem": "/usr/share/ist/hooks/ist_sys_rst.sh",
    "errorCheck": "/usr/share/ist/hooks/ist_check_error.sh"
  },
  "storageConfig": {
    "vectorMountPath": "/var/emmc/ist/IST_Test_Package/",
    "vectorStoragePath": "/var/emmc/ist/",
    "resultStoragePath": "/var/emmc/ist/results/"
  },
  "softwareInventoryId": "IST_Vectors"
}
```

### Adding a platform

`platform_cfg.json` must keep `hookDirectory`, `hookPaths`, `storageConfig` and
`softwareInventoryId` as above; tegra MODS in the factory also reads them from
this file. To use the shared hooks in `hooks/`, add a `hookConfig` object with
the board's facts:

| Field                                                      | Meaning                                                                                                                                                                                                                                                            |
| ---------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `domains[]`                                                | One entry per set of IST lines, run in order: `name`, `istBootGpio` (required)                                                                                                                                                                                     |
| `domains[].detect`                                         | Treat the domain as present only if its GPIO lines are found. This is SKU identification: vr-nvl has an internal single-CPU configuration with no FRU field, so GPIO presence is the only way to tell. `resetMethod` `gpio` only                                   |
| `domains[].preResetGpio`, `shutdownOkGpio`, `sysResetGpio` | CPU reset lines; required for `resetMethod` `gpio`                                                                                                                                                                                                                 |
| `domains[].fruProductMatch[]`                              | FruDevice product names to look for; the first match is the board's product                                                                                                                                                                                        |
| `domains[].cpus[]`                                         | `index` (DOT object suffix), `eid` (MCTP EID), optional `absentOnProduct` (CPU not fitted on that product, e.g. MGX C1; the bypass still runs); required unless `resetMethod` is `command`                                                                         |
| `resetMethod`                                              | `gpio` (default): drive the reset lines. `command`: run `resetCommand`. `hostWarmReboot`: request `ForceWarmReboot` from x86-power-control and wait for `CpuResetIndicator` to go 0 then 1                                                                         |
| `resetCommand`                                             | Command for `resetMethod` `command`                                                                                                                                                                                                                                |
| `cakBypass`                                                | `dotObjectPrefix`, optional `requiredCpus` (fewer CPUs present means a partial machine: skip the bypass; not with `absentOnProduct`). Present: the daemon runs `ist_cak_bypass.sh` after the power cycle and fails the run if it is missing; absent: no CAK bypass |
| `timings`                                                  | `shutdownOkTimeoutS` (10), `shutdownOkPollS` (0.2), `sysResetHoldS` (5), `eidTimeoutS` (45); the schema says which reset method reads each                                                                                                                         |

`hooks/ist_platform_cfg.schema.json` holds the full rules. Example:

```json
"hookConfig": {
  "domains": [
    {
      "name": "B0_M1",
      "istBootGpio": "B0_M1_CPU_IST_BOOT_HMC-O",
      "preResetGpio": "B0_M1_HMC_IST_PRE_RST_L-O",
      "shutdownOkGpio": "B0_M1_CPU_SHDN_OK_L_3V3-I",
      "sysResetGpio": "B0_M1_IST_SYS_RST_L-O",
      "cpus": [
        { "index": 0, "eid": 16 },
        { "index": 1, "eid": 17 }
      ]
    }
  ],
  "cakBypass": { "dotObjectPrefix": "/xyz/openbmc_project/DOT/IRoT_CPU_" }
}
```

The recipe builds with `-Dhooks=enabled` and, after installing the board's
`platform_cfg.json`, generates `/etc/ist/platform.env` from it, which the hooks
source:

```bash
${PYTHON} ${S}/hooks/ist_platform_env.py \
    --schema ${S}/hooks/ist_platform_cfg.schema.json \
    ${D}/etc/ist/platform_cfg.json ${D}/etc/ist/platform.env
```

The generator checks the file against the schema and fails the build when
`hookConfig` is absent, since the hooks cannot run without `platform.env`.
