/** @file
  Driver UEFI (DXE) de Intel VMD — Fase 1 + diagnostico.

  OpenCore lo carga desde UEFI.Drivers. Engancha el controlador VMD
  (8086:9A0B), mapea la CFGBAR, decodifica VMCAP/VMCONFIG/VMLOCK y recorre la
  ECAM del bus oculto.

  TRAZAS: la salida de consola de un driver NO se guarda en el fichero de log
  de OpenCore (OcConsoleLib solo reenvia al ConOut original), asi que aqui se
  escribe DIRECTO a gST->ConOut (visible en pantalla/foto).
  NO usar el protocolo OcLog de OpenCore: probado en hardware que llamar a
  OcLog->AddEntry desde el StartImage de un driver cargado por OpenCore
  CONGELA la maquina (puntero y revision del protocolo verificados correctos:
  VL-oclog=ptr rev=1000B, VL-calling-AddEntry = ultima marca). Causa raiz no
  determinada; no se necesita para el objetivo (la consola basta).

  MODO DIAGNOSTICO (UEFI.Drivers[i].Arguments = "diag"): recorre TODOS los
  handles con device-path y llama a ConnectController uno a uno, registrando en
  el log antes y despues de cada uno. Si el arranque se cuelga, la ULTIMA linea
  del log identifica el handle exacto que cuelga (y el driver que intenta
  bindearlo). Sin el argumento no hace nada de esto.

  La logica VMD (matematica ECAM, decodificacion de VMCAP/VMCONFIG, reglas de
  MSI remap) vive en VMDCore/VMDLogic.*, la MISMA fuente que usa el kext de
  macOS. Aqui solo se pone el acceso MMIO/PCI del plano UEFI.
**/

// Los headers de EDK2 son C puro: NO llevan `extern "C"` (verificado en
// MdePkg/Include/Library/UefiLib.h). Incluidos tal cual desde un .cpp, en C++
// se declaran con enlazado C++ y el linker busca el nombre manglado
// (`Print(unsigned short const*, ...)`) mientras que MdePkg exporta el simbolo
// C `Print` -> "undefined reference". Envolviendolos, el enlazado vuelve a C.
extern "C" {
#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#include <IndustryStandard/Pci.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DevicePathToText.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PciIo.h>
#include <Protocol/SimpleFileSystem.h>
}

#include "VMDLogic.hpp"

#define INTEL_VMD_VENDOR_ID  0x8086

STATIC BOOLEAN  mDiag = FALSE;

// Fichero de traza en la ESP del USB (se lee desde Windows sin fotos).
// Se abre una vez en el entry, se escribe + Flush por linea (a prueba de
// cuelgues) y no se cierra: el contenido ya esta en disco pase lo que pase.
STATIC EFI_FILE_PROTOCOL  *mLogFile = NULL;

// Declaracion adelantada: VmdTrace se define mas abajo, pero VmdLog la usa.
STATIC
VOID
VmdTrace (
  IN CONST CHAR8  *Step
  );

// Declaracion adelantada: VmdFileWrite se define tras VmdTrace, pero VmdLog
// y VmdTrace la usan.
STATIC
VOID
VmdFileWrite (
  IN CONST CHAR8  *Ascii
  );

/** Escribe una linea a la consola del firmware (ConOut directo).
OJO ABI (verificado): en X64 EDK2 define VA_LIST como __builtin_ms_va_list y
Base.h EXIGE que toda funcion que llame a VA_START() sea EFIAPI
(= __attribute__((ms_abi))). Sin EFIAPI esta funcion compila en SysV y el
VA_LIST resultante apunta a basura -> AsciiVSPrint itera punteros salvajes y
la maquina se congela. AsciiSPrint directo funciona porque SU VA_START esta
dentro de EDK2 (compilado con EFIAPI). Por eso esta declaracion lleva EFIAPI
en definicion (no hay declaracion separada). */
STATIC
VOID
EFIAPI
VmdLog (
  IN CONST CHAR8  *Format,
  ...
  )
{
  VA_LIST  Marker;
  CHAR8    Ascii[256];
  CHAR16   Wide[256];

  VA_START (Marker, Format);

  VmdTrace ("VL-vsprint-in");
  AsciiVSPrint (Ascii, sizeof (Ascii), Format, Marker);
  VmdTrace ("VL-vsprint-out");
  VA_END (Marker);

  AsciiStrToUnicodeStrS (Ascii, Wide, ARRAY_SIZE (Wide));
  VmdTrace ("VL-unicode-done");
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, Wide);
  }
  VmdFileWrite (Ascii);
  VmdTrace ("VL-log-done");
}

/**
  Traza de biseccion: escribe SIEMPRE a la consola del firmware (ConOut
  directo), que no depende de OpenCore. Sirve para localizar un cuelgue dentro
  del propio entry point: cada paso deja su marca en pantalla (foto) aunque el
  log de OpenCore no avance.
**/
STATIC
VOID
VmdTrace (
  IN CONST CHAR8  *Step
  )
{
  CHAR8   Ascii[128];
  CHAR16  Wide[128];

  AsciiSPrint (Ascii, sizeof (Ascii), "IntelVMD-UEFI: [trace] %a\r\n", Step);
  AsciiStrToUnicodeStrS (Ascii, Wide, ARRAY_SIZE (Wide));
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, Wide);
  }
  VmdFileWrite (Ascii);
}

/**
  Traza persistente: IntelVMDUefi.log en la raiz de la ESP del USB.
  Se abre una vez en el entry (borrando el arranque anterior), se escribe +
  Flush por linea (a prueba de cuelgues) y no se cierra: el contenido ya esta
  en disco pase lo que pase. Asi el diagnostico se lee desde Windows sin
  depender de fotos.
**/
STATIC
VOID
VmdFileWrite (
  IN CONST CHAR8  *Ascii
  )
{
  UINTN  Len;

  if ((mLogFile == NULL) || (Ascii == NULL)) {
    return;
  }
  Len = AsciiStrLen (Ascii);
  if (Len == 0) {
    return;
  }
  mLogFile->Write (mLogFile, &Len, (VOID *)Ascii);
  mLogFile->Flush (mLogFile);
}

STATIC
VOID
VmdLogCreateOnVolume (
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs,
  IN UINTN                             VolIndex
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *Old;
  EFI_FILE_PROTOCOL  *Marker;

  if (Fs == NULL) {
    return;
  }
  Status = Fs->OpenVolume (Fs, &Root);
  VmdTrace ("LO-vol");
  if (EFI_ERROR (Status) || (Root == NULL)) {
    return;
  }
  // Solo vale el volumen que tiene \EFI\OC: asi el log cae seguro en el USB
  // de OpenCore y no en la ESP interna de Windows.
  Status = Root->Open (
                    Root,
                    &Marker,
                    (CHAR16 *)L"\\EFI\\OC\\OpenCore.efi",
                    EFI_FILE_MODE_READ,
                    0
                    );
  VmdTrace ("LO-marker");
  if (EFI_ERROR (Status) || (Marker == NULL)) {
    return;
  }
  Marker->Close (Marker);
  VmdTrace ("LO-usb-found");
  // Borrar el log del arranque anterior para un fichero limpio por boot.
  Status = Root->Open (
                    Root,
                    &Old,
                    (CHAR16 *)L"IntelVMDUefi.log",
                    EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                    0
                    );
  if (!EFI_ERROR (Status) && (Old != NULL)) {
    Old->Delete (Old);
  }
  Status = Root->Open (
                    Root,
                    &mLogFile,
                    (CHAR16 *)L"IntelVMDUefi.log",
                    EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ |
                    EFI_FILE_MODE_WRITE,
                    0
                    );
  VmdTrace ("LO-created");
  if (EFI_ERROR (Status)) {
    mLogFile = NULL;
  }
  // Root se deja abierto a proposito: el fichero debe sobrevivir a un
  // cuelgue posterior y su contenido ya esta vaciado con Flush por linea.
  (VOID)VolIndex;
}

STATIC
VOID
VmdLogOpen (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_HANDLE                   *FsHandles;
  UINTN                        FsCount;
  UINTN                        Index;

  // Via 1: el volumen del que se cargo nuestra imagen. OpenCore carga con
  // LoadImage(FilePath=NULL, buffer en memoria), asi que este DeviceHandle no
  // es fiable: si falla se pasa a la via 2 (barrido de todos los volumenes).
  LoadedImage = NULL;
  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  VmdTrace ("LO-img");
  if (!EFI_ERROR (Status) && (LoadedImage != NULL) &&
      (LoadedImage->DeviceHandle != NULL)) {
    Status = gBS->HandleProtocol (
                    LoadedImage->DeviceHandle,
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Fs
                    );
    VmdTrace ("LO-fs");
    if (!EFI_ERROR (Status) && (Fs != NULL)) {
      VmdLogCreateOnVolume (Fs, 0);
      if (mLogFile != NULL) {
        VmdTrace ("LO-via1-ok");
        return;
      }
    }
  }
  // Via 2: barrer todos los volumenes y quedarse con el que tiene \EFI\OC.
  FsHandles = NULL;
  FsCount   = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &FsCount,
                  &FsHandles
                  );
  VmdTrace ("LO-sweep");
  if (EFI_ERROR (Status) || (FsHandles == NULL)) {
    return;
  }
  for (Index = 0; Index < FsCount; ++Index) {
    Status = gBS->HandleProtocol (
                    FsHandles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Fs
                    );
    if (EFI_ERROR (Status) || (Fs == NULL)) {
      continue;
    }
    VmdLogCreateOnVolume (Fs, Index + 1);
    if (mLogFile != NULL) {
      VmdTrace ("LO-via2-ok");
      break;
    }
  }
  gBS->FreePool (FsHandles);
}

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

// Controladores VMD soportados. i3-1115G4 (Tiger Lake) = 9A0B. Extensible a
// AD0B (Alder Lake), 7D0B, 28Cx (server) sin tocar la logica.
STATIC CONST UINT16  mSupportedDevices[] = { 0x9A0B };

// Registros del propio controlador VMD (Intel VMD Technical Document / vmd.c).
#define VMD_REG_VMCAP        0x40
#define VMD_REG_VMCONFIG     0x44
#define VMD_REG_VMLOCK       0x70
#define VMD_CFGBAR_REG       0x10 // offset de BAR0 en config space
#define VMD_CFGBAR_INDEX     0    // BAR0 en PciIo->Mem.Read (indice de BAR)

// Tamano observado de la CFGBAR en 9A0B (spec / dump de recursos Windows).
#define VMD_CFGBAR_SIZE      0x2000000ULL  // 32 MB

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

/** Devuelve el device-path como ASCII (o NULL). El llamante libera con FreePool. */
STATIC
CHAR8 *
VmdDevicePathToAscii (
  IN EFI_DEVICE_PATH_PROTOCOL  *Dp
  )
{
  EFI_DEVICE_PATH_TO_TEXT_PROTOCOL  *DpToText;
  CHAR16                            *Text;
  CHAR8                             *Ascii;
  UINTN                             Size;

  if ((Dp == NULL) || (gBS == NULL)) {
    return NULL;
  }

  if (EFI_ERROR (
        gBS->LocateProtocol (
               &gEfiDevicePathToTextProtocolGuid,
               NULL,
               (VOID **)&DpToText
               )
        )
      || (DpToText == NULL))
  {
    return NULL;
  }

  Text = DpToText->ConvertDevicePathToText (Dp, TRUE, TRUE);
  if (Text == NULL) {
    return NULL;
  }

  Size  = StrLen (Text) + 1;
  Ascii = (CHAR8 *)AllocatePool (Size);
  if (Ascii != NULL) {
    UnicodeStrToAsciiStrS (Text, Ascii, Size);
  }

  FreePool (Text);
  return Ascii;
}

/**
  Recorre la ECAM del dominio VMD y registra cada dispositivo presente.
  El acceso a config-space es un read de dword por PciIo->Mem.Read (NO se
  desreferencia la direccion fisica de la BAR: en UEFI no tiene por que estar
  mapeada 1:1 y el deref crudo puede colgar).
**/
STATIC
UINT32
EnumerateVmdBus (
  IN EFI_PCI_IO_PROTOCOL  *PciIo,
  IN UINT8                BusStart
  )
{
  UINT32  Found;
  UINT32  BusCount;
  UINT32  BusIndex;
  UINT8   Dev;
  UINT8   Fn;

  Found    = 0;
  BusCount = vmd_cfgbar_bus_count (VMD_CFGBAR_SIZE, BusStart);

  VmdLog ("IntelVMD-UEFI: ECAM cubre %u buses desde %u\n", BusCount, BusStart);

  for (BusIndex = 0; BusIndex < BusCount; ++BusIndex) {
    UINT8  Bus = (UINT8)(BusStart + BusIndex);

    for (Dev = 0; Dev < 32; ++Dev) {
      for (Fn = 0; Fn < 8; ++Fn) {
        UINT8        DevFn;
        UINT32       Offset;
        UINT32       Id;
        EFI_STATUS   Status;

        DevFn  = (UINT8)((Dev << 3) | Fn);
        Offset = vmd_ecam_offset (Bus, BusStart, DevFn, 0);
        if (Offset == 0xFFFFFFFFU) {
          continue;
        }

        Id     = 0xFFFFFFFFU;
        Status = PciIo->Mem.Read (
                          PciIo,
                          EfiPciIoWidthUint32,
                          VMD_CFGBAR_INDEX, // BAR0 = CFGBAR: el offset ya es relativo a ella
                          Offset,
                          1,
                          &Id
                          );
        if (EFI_ERROR (Status)) {
          VmdLog ("IntelVMD-UEFI:   mem.read fallo %u/%02x -> %r\n", Bus, DevFn, Status);
          if (Fn == 0) {
            break;
          }

          continue;
        }

        if (vmd_config_is_empty (Id)) {
          if (Fn == 0) {
            break;
          }

          continue;
        }

        VmdLog (
          "IntelVMD-UEFI:   bus %3u devfn %02x  %04x:%04x\n",
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
// Modo diagnostico: conectar los handles uno a uno, trazeando cada paso.
// ---------------------------------------------------------------------------

STATIC
VOID
VmdDiagnoseConnect (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *HandleBuffer;
  UINTN       HandleCount;
  UINTN       Index;

  HandleBuffer = NULL;
  HandleCount  = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiDevicePathProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    VmdLog ("IntelVMD-UEFI: DIAG LocateHandleBuffer -> %r\n", Status);
    return;
  }

  VmdLog ("IntelVMD-UEFI: DIAG %u handles con device-path\n", HandleCount);

  for (Index = 0; Index < HandleCount; ++Index) {
    EFI_DEVICE_PATH_PROTOCOL  *Dp;
    CHAR8                     *Text;

    Dp = NULL;
    gBS->HandleProtocol (HandleBuffer[Index], &gEfiDevicePathProtocolGuid, (VOID **)&Dp);

    Text = VmdDevicePathToAscii (Dp);
    VmdLog (
      "IntelVMD-UEFI: DIAG connect [%u/%u] %a\n",
      Index,
      HandleCount,
      (Text != NULL) ? Text : "<sin devpath>"
      );
    if (Text != NULL) {
      FreePool (Text);
    }

    // Si el cuelgue esta aqui, la ultima linea del log es la de arriba.
    Status = gBS->ConnectController (HandleBuffer[Index], NULL, NULL, TRUE);
    VmdLog ("IntelVMD-UEFI: DIAG   -> %r\n", Status);
  }

  FreePool (HandleBuffer);
  VmdLog ("IntelVMD-UEFI: DIAG fin del recorrido\n");
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

  // Traza de entrada: si el cuelgue esta DENTRO de este Supported (OpenProtocol,
  // Pci.Read o la conversion del device-path), esta es la ultima linea.
  VmdTrace ("S-enter");

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  VmdTrace ("S-opened");
  if (Status == EFI_ALREADY_STARTED) {
    VmdLog ("IntelVMD-UEFI: supported: handle %p ya gestionado\n", ControllerHandle);
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
  VmdTrace ("S-read");
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
  // Traza SIEMPRE la evaluacion con su device-path: si el cuelgue de OC esta
  // en nuestro Supported/Start, el fichero muestra el ultimo handle evaluado.
  {
    EFI_DEVICE_PATH_PROTOCOL  *Dp;
    CHAR8                     *DpText;

    Dp     = NULL;
    DpText = NULL;
    if (!EFI_ERROR (
          gBS->HandleProtocol (
                 ControllerHandle,
                 &gEfiDevicePathProtocolGuid,
                 (VOID **)&Dp
                 ))) {
      VmdTrace ("S-gotdp");
      DpText = VmdDevicePathToAscii (Dp);
      VmdTrace ("S-dptext");
    }
    VmdLog (
      "IntelVMD-UEFI: supported %04x:%04x -> %r [%a]\n",
      Pci.Hdr.VendorId,
      Pci.Hdr.DeviceId,
      Status,
      (DpText != NULL) ? DpText : "?"
      );
    if (DpText != NULL) {
      FreePool (DpText);
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

  VmdLog ("IntelVMD-UEFI: start()\n");

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    VmdLog ("IntelVMD-UEFI: start: OpenProtocol -> %r\n", Status);
    return Status;
  }

  // Habilitar el decoding de memoria: sin esto la CFGBAR no responde.
  Status = PciIo->Attributes (
                    PciIo,
                    EfiPciIoAttributeOperationEnable,
                    EFI_PCI_IO_ATTRIBUTE_MEMORY,
                    NULL
                    );
  VmdLog ("IntelVMD-UEFI: start: Attributes -> %r\n", Status);

  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, PCI_DEVICE_ID_OFFSET, 1, &DeviceId);
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, VMD_REG_VMCAP, 1, &VmCap);
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, VMD_REG_VMCONFIG, 1, &VmConfig);
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, VMD_REG_VMLOCK, 1, &VmLock);

  Regs     = vmd_decode (VmCap, VmConfig);
  BusStart = vmd_bus_start (Regs.bus_restrict_cap, Regs.bus_restrict_cfg);

  VmdLog (
    "IntelVMD-UEFI: attach 8086:%04x VMCAP=%04x VMCONFIG=%04x VMLOCK=%08x\n",
    DeviceId,
    VmCap,
    VmConfig,
    VmLock
    );
  VmdLog (
    "IntelVMD-UEFI: busStart=%u msiRemap=%u canBypass=%u busRestrictCap=%u cfg=%u\n",
    BusStart,
    Regs.msi_remap_enabled ? 1U : 0U,
    vmd_can_bypass_msi_remap ((UINT32)DeviceId << 16 | INTEL_VMD_VENDOR_ID) ? 1U : 0U,
    Regs.bus_restrict_cap ? 1U : 0U,
    Regs.bus_restrict_cfg
    );

  if (BusStart == 0xFF) {
    VmdLog ("IntelVMD-UEFI: configuracion desconocida (busStart=0xFF); no se enumera\n");
    return EFI_SUCCESS;
  }

  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, VMD_CFGBAR_REG, 1, &Bar0);
  CfgBarBase = (UINT64)(Bar0 & 0xFFFFFFF0U);

  VmdLog ("IntelVMD-UEFI: CFGBAR base = 0x%Lx (BAR0=%08x)\n", CfgBarBase, Bar0);

  if (CfgBarBase == 0) {
    VmdLog ("IntelVMD-UEFI: BAR0 sin asignar; no se enumera\n");
    return EFI_SUCCESS;
  }

  Found = EnumerateVmdBus (PciIo, BusStart);
  VmdLog ("IntelVMD-UEFI: %u dispositivos en la ECAM del VMD\n", Found);

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

STATIC
VOID
ReadOwnArguments (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  EFI_STATUS                 Status;

  LoadedImage = NULL;
  Status      = gBS->OpenProtocol (
                       ImageHandle,
                       &gEfiLoadedImageProtocolGuid,
                       (VOID **)&LoadedImage,
                       ImageHandle,
                       NULL,
                       EFI_OPEN_PROTOCOL_GET_PROTOCOL
                       );
  if (EFI_ERROR (Status) || (LoadedImage == NULL)) {
    return;
  }
  if (LoadedImage->LoadOptions == NULL || LoadedImage->LoadOptionsSize == 0) {
    return;
  }

  // OpenCore pasa Arguments como UTF-16 (OcAppendArgumentsToLoadedImage hace
  // AsciiStrToUnicodeStrS). Comparar "diag" en ASCII contra UTF-16 falla
  // (d\0i\0a\0g\0): hay que buscar en CHAR16.
  {
    STATIC CONST CHAR16 mDiagStr[] = { 0x64, 0x69, 0x61, 0x67, 0 };
    if (StrStr((CONST CHAR16 *)LoadedImage->LoadOptions, mDiagStr) != NULL) {
      mDiag = TRUE;
    }
  }
}

extern "C" EFI_STATUS EFIAPI
IntelVMDUefiEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  // Cada paso deja marca en consola + fichero. Si el arranque se cuelga,
  // la ultima linea de IntelVMDUefi.log dice exactamente que llamada cuelga.
  VmdLogOpen (ImageHandle);
  VmdTrace ("0-enter");
  ReadOwnArguments (ImageHandle);
  VmdTrace ("1-read-args");

  VmdLog ("IntelVMD-UEFI: entry point (%a)\n", mDiag ? "diag" : "normal");
  VmdTrace ("2-entry-logged");

  // Modo diagnostico (Arguments="diag" en UEFI.Drivers): conecta los handles
  // uno a uno con traza antes/despues. Si el cuelgue original esta en el
  // connect, la ultima linea del fichero identifica el handle culpable.
  // Sin pausa: el fichero ya guarda todo, el arranque sigue solo.
  if (mDiag) {
    VmdTrace ("3-diag-start");
    VmdDiagnoseConnect ();
    VmdTrace ("3-diag-done");
  }

  Status = EfiLibInstallDriverBinding (
             ImageHandle,
             SystemTable,
             &mIntelVMDUefiDriverBinding,
             ImageHandle
             );
  VmdTrace ("4-binding-installed");
  return Status;
}
