# HANDOFF

Estado del proyecto al cierre de esta sesion. Leer esto primero.
Actualizar SIEMPRE al terminar cada bloque de trabajo.

## Que es esto

**IntVMD en dos planos**, un solo conocimiento VMD:

1. **Driver UEFI (keystone)** — `IntelVMDUefi/`. OpenCore lo carga desde
   `UEFI.Drivers`. Expone el NVMe tras Intel VMD **y** evita el cuelgue de
   `OcConnectDrivers()` que hoy impide arrancar macOS. Sin esto no arranca nada.
2. **Kext de macOS** — `IntelVMD/`. Hace que `IONVMeFamily` use el NVMe tras VMD.

Restriccion dura: **VMD queda encendido** (Windows instalado con RST/VMD sigue
arrancando). Nada de "apagar VMD".

El VMD no es un controlador de almacenamiento: es una **apertura PCI**. BAR0 =
ventana ECAM al bus oculto; registro 0x44 = MSI remapping. Especificacion
ejecutable: `drivers/pci/controller/vmd.c` de Linux (GPL). Intel hace lo mismo en
BIOS con `VMDVROC_1.efi` + `VMDVROC_2.efi` (VMD Technical Document rev 1.2).

## Estado

| Plano | Que | Estado |
|---|---|---|
| Kext macOS | Match + BAR0 + registros VMD + **enumeracion ECAM con log** | Hecho, compila (CI verde) |
| Kext macOS | `IOPCIBridge` (publicar nubs) | Pendiente (Fase 4) — interfaz publica ya verificada |
| UEFI | Nucleo compartido `VMDCore/` | Hecho |
| UEFI | Driver DXE F1 (bind + BAR0 + VMCAP/VMCONFIG + enumera) | **En curso**; CI EDK2 iterando |
| UEFI | Host bridge (`ROOT_BRIDGE_IO` + `HOST_BRIDGE_RESOURCE_ALLOCATION`) | Pendiente (Fase 2) |
| UEFI | Anti-cuelgue via prioridad de OpenCore | Pendiente (Fase 3) |
| Ambos | Demux MSI-X | Pendiente (Fase 5). La parte mas dura |
| Ambos | Boot real desde el NVMe | Pendiente (Fase 6) |

## Hallazgo clave del cuelgue (2026-10-08)

El arranque se detiene SIEMPRE en `OcConnectDrivers()`
(`Library/OcDriverConnectionLib`): el log DEBUG acaba en
`OC: Connecting drivers...` y nunca llega a `done`. No es video ni HFS+.
Quitar `OpenPartitionDxe.efi` **no** lo arregla. Sospechoso: el firmware al
conectar el almacenamiento interno (NVMe tras VMD).

`ConnectDrivers=False` lo esquiva pero `ocvalidate` lo rechaza (HFS+/particiones
sin conectar) — no es solucion.

**Palanca elegida:** `OcRegisterDriversToHighestPriority` engancha
`EFI_PLATFORM_DRIVER_OVERRIDE_PROTOCOL.GetDriver`. Un driver declarado en
`UEFI.Drivers` se prioriza al conectar, de modo que el driver VMD del firmware
**no bindea** → desaparece el cuelgue, y en su lugar bindea el nuestro.

## Como construir

Kext (macOS, Xcode/SDK versionado):
```sh
make all     # -> IntelVMD.kext (Mach-O KEXTBUNDLE x86_64, ad-hoc)
make check   # solo sintaxis
```

Driver UEFI (EDK2, cualquier host con clang/GCC + Python):
```sh
export WORKSPACE=$PWD
export EDK_TOOLS_PATH=$PWD/edk2/BaseTools
export PACKAGES_PATH=$PWD/edk2:$(dirname $PWD)   # el paquete ES la raiz del repo
source edk2/edksetup.sh BaseTools
build -p IntVMD.dsc -a X64 -t GCC5 -b RELEASE
```
Ambos los compila CI (`.github/workflows/build-kext.yml` y `build-uefi.yml`).

## Entorno de pruebas

**No se puede probar aqui.** La maquina de trabajo es una VM de VMware
(`Vendor ID 0x15ad`, SVGA) sin dispositivo VMD. Toda validacion de runtime necesita
la maquina destino con VMD real (i3-1115G4, `8086:9A0B`, family CLIENT).

## Proximo paso concreto

1. Cerrar la F1 del driver UEFI: que CI produzca `IntelVMDUefi.efi`.
2. En la maquina destino: `IntelVMDUefi.efi` en `EFI/OC/Drivers`, declarado en
   `UEFI.Drivers`, y arrancar con log DEBUG. Debe verse la linea
   `IntelVMD-UEFI: attach 8086:9a0b ... busStart=...`.
3. Fase 2: publicar el host bridge → `PciBusDxe` enumera → `NvmExpressDxe` →
   dispositivo de bloque → OpenCore lo ve. **Y comprobar que el cuelgue desaparece.**

## CI

`.github/workflows/`:
- `build-kext.yml` (macos-15-intel): compila y empaqueta el kext.
- `build-uefi.yml` (ubuntu): compila el driver con EDK2 (X64).

Sin token, los logs de job dan 403. Se leen por anotaciones del check-run
(`/commits/{sha}/check-runs` → `/check-runs/{id}/annotations`) y por el estado por
paso (`/actions/jobs/{id}`). El workflow UEFI publica la salida del build como
anotaciones a proposito.
