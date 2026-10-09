# HANDOFF

Estado del proyecto al cierre de esta sesion. Leer esto primero.
Actualizar SIEMPRE al terminar cada bloque de trabajo.

## Que es esto

Driver Intel VMD (Volume Management Device) para macOS. Expone los NVMe
ocultos tras el VMD para que `IONVMeFamily` de Apple los reclame.
Hackintosh / OpenCore. Repo: `tsuchinaka/IntVMD` (privada).

El VMD no es un controlador de almacenamiento: es una apertura PCI.
BAR0 = ventana ECAM al bus oculto, BAR2/4 = ventanas de memoria,
registro 0x44 = MSI remapping. Especificacion ejecutable de referencia:
`drivers/pci/controller/vmd.c` de Linux (GPL).

## Estado

| Fase | Que | Estado |
|---|---|---|
| 0 | Reconocimiento (`recon_vmd.sh`) | Script listo. **Sin ejecutar: falta hardware real** |
| 1 | Kext esqueleto, match + BAR0 + registros VMD | Hecho, compila, kext empaquetado y firmado ad-hoc |
| 2 | Enumeracion del bus oculto (nubs hijos ECAM) | Pendiente |
| 3 | Demux MSI-X (irq_domain equivalente) | Pendiente. La parte mas dura |
| 4 | Inyeccion en OpenCore, boot desde NVMe tras VMD | Pendiente |
| 5 | Xe (GPU) | Proyecto aparte. No tocar hasta cerrar VMD |

## Como construir

```sh
make all     # -> IntelVMD.kext (Mach-O KEXTBUNDLE x86_64, ad-hoc)
make check   # solo sintaxis
```

**Trampa importante:** hay que usar un SDK *versionado* (`MacOSX13.sdk` o
`14.x`). El `MacOSX.sdk` sin version redirige `IOKit/pci/IOPCIDevice.h` a
`PCIDriverKit/IOPCIDevice.h` y la compilacion falla con `OSClassLoadInformation`
desconocido. El workflow de CI ya lo resuelve; en local, pasar `SDK=`.

En C++ de kext hay que declarar `typedef IOService super;` a mano: este SDK
no lo genera.

## Entorno de pruebas

**No se puede probar aqui.** La maquina de trabajo es una VM de VMware
(`Vendor ID 0x15ad`, SVGA) sin dispositivo VMD. Todo test necesita la
maquina destino con VMD real (i3-1115G4, Device ID esperado `8086:9A0B`,
familia CLIENT, sin bypass de MSI).

## Proximo paso concreto

1. En la maquina destino: Live USB Linux, `sudo ./recon/recon_vmd.sh`,
   confirmar Device ID y VMCAP/VMCONFIG/VMLOCK.
2. Probar carga de la Fase 1: kext en `EFI/OC/Kexts`, declarado en
   `config.plist`, SIP reducido (`csr-active-config`, p. ej. `03080000`,
   obligatorio por ser kext sin firmar por Apple). Boot con `-v`.
   Logs: `log show --predicate 'sender == IntelVMD' --last boot`.
   Debe verse `attach a 8086:... VMCAP=... bus Start=...`.
3. Fase 2: escanear ECAM en BAR0 con
   `offset = ECAM(bus - busStart, devfn, reg)`, crear nubs hijos
   `IOPCIDevice` y publicar MEMBAR1/2. Exito = `IONVMeFamily` hace match
   en el NVMe hijo. Todo se lee en runtime, asi que se puede escribir sin
   esperar al recon.

## CI

`.github/workflows/build-kext.yml`: push a `main`, PRs y manual.
Runner `macos-13` (el ultimo Intel; los demas son ARM y el kext es x86_64).
Sube el `.kext` como artefacto. Primer run en cola al cerrar esta sesion.
