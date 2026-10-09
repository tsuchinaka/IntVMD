# Proyecto VMD en macOS

Objetivo: driver que exponga los NVMe ocultos tras Intel VMD (8086:09AB/28Cx/467f/7d0b/ad0b/9A0B/...) para que `IONVMeFamily` de Apple los reclame. Despues: Xe.

## Por que es viable

El VMD no es un controlador de almacenamiento: es una **apertura PCI**.
BAR0 = CFGBAR (ventana ECAM al bus oculto), BAR2/4 = MEMBAR1/2 (ventanas de
memoria), registro 0x44 = MSI remapping. El driver Linux `vmd.c` (GPL) es la
especificacion ejecutable de referencia — ya lo tenemos. El trabajo es
portar enumeracion + interrupciones a IOKit, no RE a ciegas.

## Vehiculo: KEXT, no dext

Un DriverKit dext (userspace) **no puede** publicar nubs en el plano IOPCI
del kernel, asi que `IONVMeFamily` nunca se adjuntaria. El VMD exige KEXT
clásico (`IOPCIDevice` + nubs hijos). En Hackintosh/OpenCore esto es viable:
inyeccion desde el bootloader. Requiere SIP reducido para kexts sin firmar.

## Fases

- [x] **Fase 0 — Reconocimiento.** `vmd.c` descargado, IDs en `Info.plist`.
      Falta: ejecutar `recon/recon_vmd.sh` en hardware real (esta VM de
      VMware no tiene VMD) y confirmar el Device ID (en i3-1115G4 lo
      esperado es `8086:9A0B`, familia CLIENT: sin bypass MSI).
- [x] **Fase 1 — Kext esqueleto.** `IntelVMD/` hace match, mapea BAR0,
      vuelca VMCAP/VMCONFIG/VMLOCK y calcula bus Start. **Compilado y
      empaquetado: `IntelVMD.kext` (Mach-O KEXTBUNDLE x86_64, firma
      ad-hoc, `make all`).** Falta probar carga en hardware real:
      copiar a `EFI/OC/Kexts`, declarar en `config.plist`, arrancar
      con `-v` y comprobar `log show --predicate 'sender == IntelVMD'`.
      Requiere SIP reducido (`csr-active-config`) por ser kext sin
      firmar por Apple.
- [ ] **Fase 2 — Enumeracion.** Nubs `IOPCIDevice` hijos con config-space
      redirigido a CFGBAR (`offset = ECAM(bus - busStart, devfn, reg)`),
      ventanas MEMBAR1/2 como `IODeviceMemory`. Exito = `IONVMeFamily`
      hace match en el NVMe hijo.
- [ ] **Fase 3 — Interrupciones.** Con MSI-Remap=ON (caso 9A0B), reservar
      vectores MSI-X propios y demultiplexar hacia los hijos (equivalente
      al `irq_domain` de Linux). Es la parte mas dura.
- [ ] **Fase 4 — Boot.** Inyeccion en la collection de OpenCore para
      arrancar desde el NVMe tras VMD.
- [ ] **Fase 5 — Xe.** Proyecto aparte y mucho mayor (firmware GuC/HuC,
      display). Ni tocar hasta que VMD arranque.

## RE pendiente (solo donde Linux no cubre)

1. `iaStorVD.sys` (Windows) en Ghidra: buscar accesos a 0x40/0x44/0x70
   para confirmar semantica de bits en tu stepping concreto.
2. `IOPCIFamily`/`IONVMeFamily` en Hopper: como Apple crea nubs PCI y
   que espera `IONVMeController` del provider.
