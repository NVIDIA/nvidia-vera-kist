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

The platform name prefix (`HGX_` on the HMC, none on a BMC) comes from the name
of the CPU `NvDevice` records (`HGX_CPU_<i>`, `CPU_<i>`). The vector inventory
object is `/xyz/openbmc_project/inventory_software/<prefix>IST_Vectors`, and the
CAK bypass (`ist_cak_bypass.sh`, run after the power cycle; the run fails if it
is missing) goes to `/xyz/openbmc_project/DOT/<prefix>IRoT_CPU_<i>`. The bypass
is skipped when fewer than the fitted CPUs are present. CPUs are reset through
the domain GPIOs, with fixed timings: 10 s for ShutdownOk (polled every 0.2 s),
a 5 s system-reset hold and 45 s for the EIDs.

| `ISTDomain` record (one per set of IST lines)    | Meaning                                                          |
| ------------------------------------------------ | ---------------------------------------------------------------- |
| `Name`, `Index`                                  | Name used in hook messages; domains run in `Index` order         |
| `BootGpio`                                       | Driven 1 by `ist_boot_enable.sh` and 0 by `ist_boot_disable.sh`  |
| `PreResetGpio`, `ShutdownOkGpio`, `SysResetGpio` | CPU reset lines                                                  |
| `Detect`                                         | Present only if its GPIO lines are found (vr-nvl single-CPU SKU) |
| `Cpus`                                           | CPU indices reset with this domain                               |

A CPU's MCTP EID comes from its `NvDevice` record (Kind `cpu`, `EID`), or from
its `USBRCMRecovery` record (`FW_CPU_<i>` or `HGX_FW_CPU_<i>`, `MctpEID`) where
there is none; a CPU with no record is not fitted, which is how the MGX C1 drops
CPU 1. The daemon waits for a CPU `NvDevice` record before it starts.
