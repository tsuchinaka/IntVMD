## @file
# DSC de IntVMD: construye el driver UEFI (X64) que OpenCore carga desde
# UEFI.Drivers. Requiere EDK2 en PACKAGES_PATH.
#
#   export PACKAGES_PATH=<edk2>:<este repo>
#   build -p IntVMD.dsc -a X64 -t GCC5 -b RELEASE
#
##
[Defines]
  PLATFORM_NAME           = IntVMD
  PLATFORM_GUID           = 9d8c7b6a-5e4f-4321-8765-0123456789ab
  PLATFORM_VERSION        = 0.1
  DSC_SPECIFICATION       = 0x00010005
  OUTPUT_DIRECTORY        = Build/IntVMD
  SUPPORTED_ARCHITECTURES = X64
  BUILD_TARGETS           = DEBUG|RELEASE
  SKUID_IDENTIFIER        = DEFAULT

[LibraryClasses]
  DebugLib
  BaseLib
  BaseMemoryLib
  MemoryAllocationLib
  UefiBootServicesTableLib
  UefiDriverEntryPoint
  UefiLib
  PcdLib
  DevicePathLib
  IoLib
  VMDCore|VMDCore/VMDCore.inf

[Components]
  IntelVMDUefi/IntelVMDUefi.inf
