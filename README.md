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

With `-Dhooks=enabled` the build installs the shared hooks into
`/usr/share/ist/hooks/` and `/etc/ist/platform_cfg.json`. Board-specific
`.bbappend` files should install:

- `/usr/share/ist/kist_itm_verify_key.pem` — ECDSA-P384 public key for
  `kist_itm` signature verification

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
  }
}
```

### Adding a platform

`platform_cfg.json` is the same on every board; tegra MODS in the factory also
reads its storage paths. The board's facts come from entity-manager records
(schema `nvidia_ist.json` in entity-manager), which the daemon and the hooks
read over D-Bus:

| `IST` record (one per board) | Meaning                                                                                                                                                                                    |
| ---------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `SoftwareInventoryId`        | Name of the vector inventory object, `/xyz/openbmc_project/inventory_software/<id>`                                                                                                        |
| `ResetMethod`                | `gpio` (default): drive the reset lines. `command`: run `ResetCommand`. `hostWarmReboot`: request `ForceWarmReboot` from x86-power-control and wait for `CpuResetIndicator` to go 0 then 1 |
| `ResetCommand`               | Command for `ResetMethod` `command`                                                                                                                                                        |
| `DotObjectPrefix`            | Present: CAK bypass on (the daemon runs `ist_cak_bypass.sh` after the power cycle and fails the run if it is missing); the CPU index is appended                                           |
| `RequiredCpus`               | Fewer CPUs present means a partial machine: skip the bypass                                                                                                                                |
| Timings                      | `ShutdownOkTimeoutS` (10), `ShutdownOkPollS` (0.2), `SysResetHoldS` (5), `EidTimeoutS` (45)                                                                                                |

| `ISTDomain` record (one per set of IST lines)    | Meaning                                                                       |
| ------------------------------------------------ | ----------------------------------------------------------------------------- |
| `Name`, `Index`                                  | Name used in hook messages; domains run in `Index` order                      |
| `BootGpio`                                       | Driven 1 by `ist_boot_enable.sh` and 0 by `ist_boot_disable.sh`               |
| `PreResetGpio`, `ShutdownOkGpio`, `SysResetGpio` | CPU reset lines for `ResetMethod` `gpio`                                      |
| `Detect`                                         | Present only if its GPIO lines are found (vr-nvl single-CPU SKU); `gpio` only |
| `Cpus`                                           | CPU indices reset with this domain                                            |

A CPU's MCTP EID comes from its `USBRCMRecovery` record (`FW_CPU_<i>` or
`HGX_FW_CPU_<i>`, `MctpEID`); a CPU with no such record is not fitted, which is
how the MGX C1 drops CPU 1. The hooks exit 1 when there is no `IST` record, and
the daemon waits for one before it starts.
