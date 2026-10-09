#ifndef INTEL_VMD_H
#define INTEL_VMD_H

#include <IOKit/IOService.h>

class IOPCIDevice;
class IOMemoryMap;

// Fase 1: attach al dispositivo VMD, mapear CFGBAR (BAR0) y volcar
// VMCAP (0x40) / VMCONFIG (0x44) / VMLOCK (0x70).
// Fase 2 (TODO): publicar nubs hijos con config-space redirigido a CFGBAR
// para que IONVMeFamily se adjunte al NVMe oculto.
// Fase 3 (TODO): demux MSI-X cuando el remapping esta activo.
class IntelVMD : public IOService {
    OSDeclareDefaultStructors(IntelVMD);
    typedef IOService super;

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    static constexpr uint8_t kCfgBarIndex = 0; // VMD_CFGBAR en Linux vmd.c
    static constexpr uint16_t kRegVMCap    = 0x40;
    static constexpr uint16_t kRegVMConfig = 0x44;
    static constexpr uint16_t kRegVMLock   = 0x70;

    IOPCIDevice *fPCIDevice = nullptr;
    IOMemoryMap *fCfgBarMap = nullptr;

    void dumpCapabilities();
    uint8_t decodeBusStart(uint16_t vmCap, uint16_t vmConfig);
};

#endif
