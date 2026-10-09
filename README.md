# IntelVMD — Intel VMD driver for macOS (WIP)

Exposes NVMe drives hidden behind Intel Volume Management Device
controllers (8086:09AB/28Cx/467F/7D0B/AD0B/9A0B/…) to macOS, so Apple's
`IONVMeFamily` can attach to them. Hackintosh / OpenCore use.

Estado: **Phase 1** — matches the VMD PCI device, maps CFGBAR (BAR0),
dumps VMCAP/VMCONFIG/VMLOCK and computes the child bus start. Child-bus
enumeration (Phase 2) and MSI-X demux (Phase 3) are pending. See `PLAN.md`
and `HANDOFF.md` for the current state and the exact next step.

Reference: Linux `drivers/pci/controller/vmd.c` (GPL) — the VMD is a PCI
aperture (ECAM window + memory windows + MSI remapping), not a storage
controller, so this is a port of enumeration logic to IOKit, not a
from-scratch NVMe stack.

## Build (no full Xcode needed, Command Line Tools only)

```sh
make all   # -> IntelVMD.kext (ad-hoc signed)
make check # syntax-only check against Kernel.framework
```

## Test

1. Copy `IntelVMD.kext` to `EFI/OC/Kexts`, declare it in `config.plist`.
2. Reduced SIP required (`csr-active-config`, e.g. `03080000`) — unsigned kext.
3. Boot verbose (`-v`), then: `log show --predicate 'sender == IntelVMD' --last boot`.
4. Collect hardware data with `sudo ./recon/recon_vmd.sh` on Linux first —
   a VMware VM has no VMD device, testing needs real hardware.
