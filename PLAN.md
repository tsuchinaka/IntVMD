# Proyecto VMD en macOS — plan

Objetivo: que el NVMe interno (hoy detras de Intel VMD `8086:9A0B`) sea usable
**sin apagar VMD**, en los dos planos que lo necesitan:

- **UEFI (keystone):** un driver DXE que OpenCore carga. Expone el NVMe **y**
  elimina el cuelgue de `OcConnectDrivers()` que hoy impide arrancar macOS.
- **macOS:** un kext `IOPCIBridge` para que `IONVMeFamily` reclame el NVMe.

## Por que es viable

El VMD no es un controlador de almacenamiento: es una **apertura PCI**. BAR0 =
CFGBAR (ventana ECAM al bus oculto), BAR2/4 = MEMBAR1/2, registro 0x44 = MSI
remapping. El driver Linux `vmd.c` (GPL) es la especificacion ejecutable. Y
**Intel ya pone un driver VMD en UEFI** (`VMDVROC_1/2.efi`); reimplementamos algo
que existe.

## Núcleo compartido

`VMDCore/` — C++ freestanding (sin stdlib/excepciones). Matematica ECAM,
decodificacion VMCAP/VMCONFIG/VMLOCK, `busn_start`, MSI-remap, modelo de
dispositivo. Lo usan **los dos** backends: no duplicar logica.

## Vehiculos

- Kext clasico (no dext): un dext userspace no puede publicar nubs IOPCI, asi que
  `IONVMeFamily` nunca se adjuntaria.
- Driver UEFI DXE clasico: OpenCore lo carga desde `UEFI.Drivers`.

## Fases

- [x] **F0 — Nucleo compartido.** `VMDCore/VMDLogic.*` extraido; kext y UEFI compilan.
- [~] **F1 — Driver UEFI, esqueleto.** `IntelVMDUefi/`: bindea 8086:9A0B, mapea
      CFGBAR, decodifica VMCAP/VMCONFIG/VMLOCK y enumera la ECAM con `Print()`.
      Equivale a la fase 2a del kext. **En curso**: CI EDK2 afinando el build.
- [ ] **F2 — Host bridge.** Publicar `EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL` +
      `EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION` sobre la ECAM → `PciBusDxe`
      enumera → `NvmExpressDxe` → `BlockIo` → OpenCore ve el disco.
- [ ] **F3 — Anti-cuelgue.** Declarar el driver en `UEFI.Drivers`; OpenCore lo
      prioriza (`OcRegisterDriversToHighestPriority`) → el driver VMD del firmware
      no bindea → fin del cuelgue. Verificar en hardware.
- [ ] **F4 — Kext macOS A1.** `IntelVMDController : IOPCIBridge` con los virtuals
      publicos (`configRead/Write`, `firstBusNum/lastBusNum`, `ioDeviceMemory`,
      `getBridgeSpace`, `addBridgeMemoryRange`, `configure`). `IOPCIAddressSpace`
      es union publica en `IOKit/pci/IOPCIDevice.h`.
- [ ] **F5 — MSI-X demux.** El 9A0B no permite bypass; hay que demuxear la tabla
      del VMD (equivalente al `irq_domain` de Linux). La parte mas dura, en ambos planos.
- [ ] **F6 — Boot real** desde el NVMe tras VMD.

## RE pendiente (solo donde Linux no cubre)

1. `iaStorVD.sys` (Windows) en Ghidra: bits de 0x40/0x44/0x70 en el stepping real.
2. `IOPCIFamily`/`IONVMeFamily` en Hopper: como Apple crea nubs PCI.

## Docs

- Spec: `docs/superpowers/specs/2026-10-08-intvmd-uefi-and-macos-design.md`
  (en el repo `Defaults Project`, que es donde vive la documentacion del agente).
