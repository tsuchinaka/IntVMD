# HANDOFF

Estado del proyecto al cierre de esta sesion. Leer esto primero.
Actualizar SIEMPRE al terminar cada bloque de trabajo.

## Que es esto

**IntVMD en dos planos**, un solo conocimiento VMD:

1. **Driver UEFI** — `IntelVMDUefi/`. OpenCore lo carga desde `UEFI.Drivers`.
2. **Kext de macOS** — `IntelVMD/`. Para que `IONVMeFamily` use el NVMe tras VMD.

Restriccion dura: **VMD queda encendido** (Windows instalado con RST/VMD sigue
arrancando). Nada de "apagar VMD".

El VMD no es un controlador de almacenamiento: es una **apertura PCI**. BAR0 =
ventana ECAM al bus oculto; registro 0x44 = MSI remapping. Especificacion
ejecutable: `drivers/pci/controller/vmd.c` de Linux (GPL). Intel hace lo mismo en
BIOS con `VMDVROC_1.efi` + `VMDVROC_2.efi` (VMD Technical Document rev 1.2).

---

## ESTADO REAL (2026-10-09) — leer con cuidado

### Lo que funciona

- **El picker de OpenCore aparece.** Era el bloqueador principal. Config actual
  del USB: `UEFI.Drivers = OpenRuntime.efi + OpenHfsPlus.efi` **y nada mas**.
- `IntelVMD.kext` (macOS): Fase 2a (enumeracion ECAM + log), compila en CI.
- Infraestructura de diagnostico UEFI propia y muy util: el driver escribe su
  traza linea a linea (con flush) a `IntelVMDUefi.log` en la raiz del USB. Se lee
  desde Windows sin fotos ni video. Ver `IntelVMDUefi/IntelVMDUefi.cpp`
  (`VmdLogOpen` / `VmdFileWrite`).

### Lo que NO funciona, con causa identificada

- **El instalador no arranca.** Log de OC:
  `OCB: LoadImage failed - Unsupported`.
  OpenCore no puede leer el interior del `BaseSystem.dmg` **sin driver HFS+**
  (`OpenHfsPlus.efi` es obligatorio, no opcional).

### El conflicto sin resolver (este es el problema real)

```
Con OpenHfsPlus  -> el instalador carga, PERO OcConnectDrivers se cuelga
Sin OpenHfsPlus  -> el picker aparece, PERO el instalador no carga
```

Nunca se probo la combinacion que queda: **OpenHfsPlus ON + IntelVMDUefi OFF**
(el driver UEFI propio FUERA). La USB quedo desplegada con esa combinacion pero
**nunca se booteo**. Es el primer test que hay que hacer.

### Evidencia sobre el cuelgue de OcConnectDrivers

Hallazgos verificados, no conjeturas:

1. El diag conecto los **58 handles** uno a uno sin colgar ninguno, incluido
   `Pci(0xE,0x0)` (VMD) y los `NVMe(0x1,...)/HD(1..4)`. **Ninguno colgó.**
   Pero ese diag corria sin nuestro binding instalado y sin el
   `PlatformDriverOverride` de OpenCore.
2. Nuestro `Supported` abria y cerraba `PciIo` en cada handle. Con el override de
   OpenCore dandole prioridad, eso es mucho trafico sobre handles de firmware.
   Las trazas siempre morian justo despues de `supported 8086:9A0D`
   (`Pci(0xA,0x0)`), de forma identica en 4 builds distintas.
3. Quitar OpenHfsPlus **y** nuestro driver a la vez -> el picker aparece.

Conclusion: el cuelgue se disputa entre (a) nuestro driver y el override de OC, o
(b) OpenHfsPlus. El test pendiente lo dirime.

### Sobre el plano UEFI de VMD: no aplica en esta maquina

- **Bind imposible:** `start: OpenProtocol -> Access Denied`. El firmware de HP ya
  gestiona el VMD antes de que OpenCore cargue nada. No hay ventana.
- **El NVMe tras VMD YA esta publicado en UEFI**, con sus 4 particiones:
  `PciRoot(0x0)/Pci(0xE,0x0)/NVMe(0x1,02-27-A7-B4-1A-18-7C-70)/HD(1..4,GPT,...)`.

Es decir: en el plano UEFI, VMD **no oculta nada** en esta HP (Windows arranca
desde ahi). El driver UEFI de VMD no tiene trabajo que hacer. **No insistir por
esa via.**

---

## Como construir

Kext (macOS, Xcode/SDK versionado):
```sh
make all     # -> IntelVMD.kext (Mach-O KEXTBUNDLE x86_64, ad-hoc)
make check   # solo sintaxis
```

Driver UEFI (EDK2):
```sh
export WORKSPACE=$PWD
export EDK_TOOLS_PATH=$PWD/edk2/BaseTools
export PACKAGES_PATH=$PWD/edk2:$(dirname $PWD)   # el paquete ES la raiz del repo
source edk2/edksetup.sh BaseTools
build -p IntVMD.dsc -a X64 -t GCC5 -b RELEASE
```
Ambos los compila CI (`build-kext.yml`, `build-uefi.yml`). Release publica
`IntelVMDUefi.efi`.

Lecciones EDK2 por las que se paso: `PACKAGES_PATH` con el **padre** del repo;
DSC con instancias `Clase|ruta`; `RegisterFilterLibNull`,
`DebugPrintErrorLevelLib`; `extern "C"` en los includes C++; `VA_ARG` exige
`EFIAPI` en X64.

## Entorno de pruebas

La maquina de trabajo es una VM de VMware sin VMD. Toda validacion de runtime
necesita la maquina destino (i3-1115G4, `8086:9A0B`, family CLIENT).

## Proximo paso concreto (uno solo)

Bootear el USB tal como esta desplegado (`OpenHfsPlus` + `OpenRuntime`, sin
`IntelVMDUefi.efi`) y seleccionar el instalador.

- Si aparece el picker y el instalador arranca -> el cuelgue era nuestro driver;
  se retira del USB y el proyecto continua por el kext de macOS.
- Si sigue el cuelgue -> es OpenHfsPlus contra este firmware; siguiente via:
  `HfsPlusLegacy.efi` o `VBoxHfs.efi` (OcBinaryData), mismo sitio en `UEFI.Drivers`.

## CI

`.github/workflows/`:
- `build-kext.yml` (macos-15-intel): compila y empaqueta el kext.
- `build-uefi.yml` (ubuntu): compila el driver con EDK2 (X64).

Sin token, los logs de job dan 403. Se leen por anotaciones del check-run
(`/commits/{sha}/check-runs` → `/check-runs/{id}/annotations`) y por el estado
por paso (`/actions/jobs/{id}`).
