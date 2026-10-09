/** @file
  Driver UEFI (DXE) de Intel VMD — Fase 1.

  OpenCore lo carga desde UEFI.Drivers. Engancha el controlador VMD
  (8086:9A0B), mapea la CFGBAR (BAR0), decodifica VMCAP/VMCONFIG/VMLOCK y
  recorre la ECAM del bus oculto imprimiendo cada dispositivo que encuentra.

  Es el equivalente UEFI de la Fase 2a del kext: valida sobre hardware real
  que el direccionamiento ECAM y el rango de buses son correctos.

  La logica VMD (matematica ECAM, decodificacion de VMCAP/VMCONFIG, reglas de
  MSI remap) vive en VMDCore/VMDLogic.*, la MISMA fuente que usa el kext de
  macOS. Aqui solo se pone el acceso MMIO/PCI del plano UEFI.

  Uso: config.plist -> UEFI -> Drivers, entrada IntelVMDUefi.efi (Enabled).
  Salida: por Print() (consola de OpenCore) y DEBUG().
**/

// Los headers de EDK2 son C puro: NO llevan `extern "C"` (comprobado en
// MdePkg/Include/Library/UefiLib.h). Si se incluyen tal cual desde un .cpp, en
// C++ se declaran con enlazado C++ y el linker busca el nombre manglado
// (`Print(unsigned short const*, ...)`), mientras que MdePkg exporta el
// simbolo C `Print` -> "undefined reference". Envolviendolos en extern "C" el
// enlazado vuelve a ser C y los simbolos casan.
extern "C" {
#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#include <IndustryStandard/Pci.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/PciIo.h>
}

#include "VMDLogic.hpp"

// En C++ (g++) el literal L"..." tiene tipo `const wchar_t*`, mientras que
// Print()/DEBUG() exigen `const CHAR16*` (unsigned short). Con -fshort-wchar
// la representacion es identica (2 bytes) y solo cambia el TIPO, por lo que
// basta una conversion. Envolvemos Print para no castear en cada llamada.
#define VMD_PRINT(fmt, ...)  Print ((CONST CHAR16 *)(fmt), __VA_ARGS__)
#define VMD_PRINT0(fmt)      Print ((CONST CHAR16 *)(fmt))

#define INTEL_VMD_VENDOR_ID  0x8086

// Controladores VMD soportados. i3-1115G4 (Tiger Lake) = 9A0B. Extensible a
// AD0B (Alder Lake), 7D0B, 28Cx (server) sin tocar la logica.
STATIC CONST UINT16  mSupportedDevices[] = { 0x9A0B };

// Registros del propio controlador VMD (Intel VMD Technical Document / vmd.c).
#define VMD_REG_VMCAP        0x40
#define VMD_REG_VMCONFIG     0x44
#define VMD_REG_VMLOCK       0x70
#define VMD_CFGBAR_INDEX     0    // BAR0 = CFGBAR (ventana ECAM)
#define VMD_CFGBAR_REG       0x10 // offset de BAR0 en config space

// Tamano observado de la CFGBAR en 9A0B (spec / dump de recursos Windows).
#define VMD_CFGBAR_SIZE      0x2000000ULL  // 32 MB

// ---------------------------------------------------------------------------
// Driver binding
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
EFIAPI
IntelVMDUefiSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                    ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL      *RemainingDevicePath OPTIONAL
  );

STATIC
EFI_STATUS
EFIAPI
IntelVMDUefiStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                    ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL      *RemainingDevicePath OPTIONAL
  );

STATIC
EFI_STATUS
EFIAPI
IntelVMDUefiStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                    ControllerHandle,
  IN UINTN                         NumberOfChildren,
  IN EFI_HANDLE                    *ChildHandleBuffer OPTIONAL
  );

STATIC EFI_DRIVER_BINDING_PROTOCOL  mIntelVMDUefiDriverBinding = {
  IntelVMDUefiSupported,
  IntelVMDUefiStart,
  IntelVMDUefiStop,
  0x10,
  NULL,
  NULL
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

STATIC
BOOLEAN
IsSupportedDevice (
  IN UINT16  DeviceId
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mSupportedDevices); ++Index) {
    if (DeviceId == mSupportedDevices[Index]) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Recorre la ECAM del dominio VMD y registra cada dispositivo presente.
  El acceso a config-space es un read de dword en la ventana mapeada:
    addr = cfgbar + vmd_ecam_offset(bus, busStart, devfn, 0)
**/
STATIC
UINT32
EnumerateVmdBus (
  IN UINT64   CfgBarBase,
  IN UINT8    BusStart
  )
{
  UINT32  Found;
  UINT32  BusCount;
  UINT32  BusIndex;
  UINT8   Dev;
  UINT8   Fn;

  Found    = 0;
  BusCount = vmd_cfgbar_bus_count (VMD_CFGBAR_SIZE, BusStart);

  VMD_PRINT (L"IntelVMD-UEFI: ECAM cubre %u buses desde %u\n", BusCount, BusStart);

  for (BusIndex = 0; BusIndex < BusCount; ++BusIndex) {
    UINT8  Bus = (UINT8)(BusStart + BusIndex);

    for (Dev = 0; Dev < 32; ++Dev) {
      for (Fn = 0; Fn < 8; ++Fn) {
        UINT8                       DevFn;
        UINT32                      Offset;
        volatile UINT32             *Reg;
        UINT32                      Id;

        DevFn  = (UINT8)((Dev << 3) | Fn);
        Offset = vmd_ecam_offset (Bus, BusStart, DevFn, 0);
        if (Offset == 0xFFFFFFFFU) {
          continue;
        }

        Reg = (volatile UINT32 *)(UINTN)(CfgBarBase + Offset);
        Id  = *Reg;

        if ((Id & 0xFFFFU) == 0xFFFFU || vmd_config_is_empty (Id)) {
          // Funcion ausente: si es la fn0, el resto del dispositivo tampoco.
          if (Fn == 0) {
            break;
          }

          continue;
        }

        VMD_PRINT (
          L"IntelVMD-UEFI:   bus %3u devfn %02x  %04x:%04x\n",
          Bus,
          DevFn,
          (UINT16)(Id & 0xFFFFU),
          (UINT16)(Id >> 16)
          );
        ++Found;
      }
    }
  }

  return Found;
}

// ---------------------------------------------------------------------------
// Driver binding: Supported / Start / Stop
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
EFIAPI
IntelVMDUefiSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                    ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL      *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  PCI_TYPE00           Pci;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (Status == EFI_ALREADY_STARTED) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint32,
                        0,
                        sizeof (Pci) / sizeof (UINT32),
                        &Pci
                        );
  if (EFI_ERROR (Status)) {
    gBS->CloseProtocol (
           ControllerHandle,
           &gEfiPciIoProtocolGuid,
           This->DriverBindingHandle,
           ControllerHandle
           );
    return EFI_UNSUPPORTED;
  }

  Status = EFI_UNSUPPORTED;
  if (Pci.Hdr.VendorId == INTEL_VMD_VENDOR_ID) {
    if (IsSupportedDevice (Pci.Hdr.DeviceId)) {
      Status = EFI_SUCCESS;
    }
  }

  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
IntelVMDUefiStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                    ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL      *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT16               DeviceId;
  UINT16               VmCap;
  UINT16               VmConfig;
  UINT32               VmLock;
  UINT32               Bar0;
  UINT64               CfgBarBase;
  VMDRegs              Regs;
  UINT8                BusStart;
  UINT32               Found;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Habilitar el decoding de memoria: sin esto la CFGBAR no responde.
  PciIo->Attributes (
           PciIo,
           EfiPciIoAttributeOperationEnable,
           EFI_PCI_IO_ATTRIBUTE_MEMORY,
           NULL
           );

  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, PCI_DEVICE_ID_OFFSET, 1, &DeviceId);
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, VMD_REG_VMCAP, 1, &VmCap);
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, VMD_REG_VMCONFIG, 1, &VmConfig);
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, VMD_REG_VMLOCK, 1, &VmLock);

  Regs     = vmd_decode (VmCap, VmConfig);
  BusStart = vmd_bus_start (Regs.bus_restrict_cap, Regs.bus_restrict_cfg);

  VMD_PRINT (
    L"IntelVMD-UEFI: attach 8086:%04x VMCAP=%04x VMCONFIG=%04x VMLOCK=%08x\n",
    DeviceId,
    VmCap,
    VmConfig,
    VmLock
    );
  VMD_PRINT (
    L"IntelVMD-UEFI: busStart=%u msiRemap=%u canBypass=%u busRestrictCap=%u cfg=%u\n",
    BusStart,
    Regs.msi_remap_enabled ? 1U : 0U,
    vmd_can_bypass_msi_remap ((UINT32)DeviceId << 16 | INTEL_VMD_VENDOR_ID) ? 1U : 0U,
    Regs.bus_restrict_cap ? 1U : 0U,
    Regs.bus_restrict_cfg
    );

  if (BusStart == 0xFF) {
    VMD_PRINT0 (L"IntelVMD-UEFI: configuracion desconocida (busStart=0xFF); no se enumera\n");
    return EFI_SUCCESS;
  }

  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, VMD_CFGBAR_REG, 1, &Bar0);
  CfgBarBase = (UINT64)(Bar0 & 0xFFFFFFF0U);   // CFGBAR es de 32 MB, alineada

  VMD_PRINT (L"IntelVMD-UEFI: CFGBAR base = 0x%Lx\n", CfgBarBase);

  if (CfgBarBase == 0) {
    VMD_PRINT0 (L"IntelVMD-UEFI: BAR0 sin asignar; no se enumera\n");
    return EFI_SUCCESS;
  }

  Found = EnumerateVmdBus (CfgBarBase, BusStart);
  VMD_PRINT (L"IntelVMD-UEFI: %u dispositivos en la ECAM del VMD\n", Found);

  // Fase 2 (pendiente): publicar EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL +
  // EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION para que PciBusDxe enumere y
  // NvmExpressDxe exponga el NVMe como dispositivo de bloque.

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
IntelVMDUefiStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                    ControllerHandle,
  IN UINTN                         NumberOfChildren,
  IN EFI_HANDLE                    *ChildHandleBuffer OPTIONAL
  )
{
  return gBS->CloseProtocol (
                ControllerHandle,
                &gEfiPciIoProtocolGuid,
                This->DriverBindingHandle,
                ControllerHandle
                );
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

extern "C" EFI_STATUS EFIAPI
IntelVMDUefiEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EfiLibInstallDriverBinding (
           ImageHandle,
           SystemTable,
           &mIntelVMDUefiDriverBinding,
           ImageHandle
           );
}
