#ifndef INTEL_VMD_H
#define INTEL_VMD_H

#include <IOKit/IOService.h>
#include <IOKit/IOMemoryMap.h>

class IOPCIDevice;

// Nub que representa un dispositivo hijo de VMD. Hereda de IOPCIDevice para
// que IONVMeFamily pueda adjuntarse, pero REDIRIGE todas las operaciones de
// configur space a la ventana ECAM del VMD en vez de al config space del bus
// PCI del host.
//
// Por que hace falta: el NVMe esta detras del VMD, en un rango de buses que
// AppleACPIPlatform no publica (de ahi que macOS no lo vea). El kext tiene que
// "traducir" esos buses a offsets dentro del CFGBAR del VMD.
class VMDPciDevice : public IOPCIDevice {
    OSDeclareDefaultStructors(VMDPciDevice);
    typedef IOPCIDevice super;

public:
    // Configura que ventana ECAM atiende a este nodo.
    // @param ecamBase   BAR0 del VMD ya mapeado (ventana ECAM).
    // @param ecamOffset Offset de este nodo DENTRO de ecamBase.
    // @param ecamLimit  Tamano de la ventana util a partir de ecamOffset.
    bool attachToECAM(IOMemoryMap *ecamBase, uint32_t ecamOffset,
                      uint32_t ecamLimit, uint8_t bus, uint8_t devfn);

    // Fase 2: publica la cabecera real leida de la ECAM para que el matching
    // de IONVMeFamily (IOPCIPrimaryMatch) pueda compararla.
    bool primeIdentityFromECAM();

    // Lecturas de configur space sobre la ECAM. Las de 8/16 bits se
    // desdoblan a 32 porque la ECAM solo tiene accesos de dword.
    uint32_t configRead32(uint64_t offset) override;
    void configWrite32(uint64_t offset, uint32_t value) override;
    uint8_t configRead8(uint64_t offset) override;
    uint16_t configRead16(uint64_t offset) override;
    void configWrite8(uint64_t offset, uint8_t value) override;
    void configWrite16(uint64_t offset, uint16_t value) override;

private:
    bool ecamRead(uint64_t offset, uint32_t &value) const;
    bool ecamWrite(uint64_t offset, uint32_t value) const;

    IOMemoryMap *fECAMBase = nullptr;
    uint32_t fECAMOffset = 0;
    uint32_t fECAMLimit = 0;
    uint8_t fBus = 0;
    uint8_t fDevFn = 0;
};

class IntelVMD : public IOService {
    OSDeclareDefaultStructors(IntelVMD);
    typedef IOService super;

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    static constexpr uint8_t kCfgBarIndex = 0;  // VMD_CFGBAR en Linux vmd.c
    static constexpr uint8_t kMemBar1Index = 1; // VMD_MEMBAR1
    static constexpr uint8_t kMemBar2Index = 2; // VMD_MEMBAR2
    static constexpr uint8_t kMemBar3Index = 3; // VMD_MEMBAR3
    static constexpr uint8_t kCfgBar4Index = 4; // BAR4 (Base ID / shadows)
    static constexpr uint16_t kRegVMCap = 0x40;
    static constexpr uint16_t kRegVMConfig = 0x44;
    static constexpr uint16_t kRegVMLock = 0x70;
    // Registro Base ID dentro de BAR4 (solo para chips 28C1; ver Linux
    // vmd_get_bus_info_from_bar4). Dejamos la constante por si hace falta.
    static constexpr uint64_t kBaseIdReg = 0x2840;

    IOPCIDevice *fPCIDevice = nullptr;
    IOMemoryMap *fCfgBarMap = nullptr;

    uint8_t fBusStart = 0;
    uint32_t fBusCount = 0;
    bool fMsiRemapEnabled = false;
    bool fCanBypassMsiRemap = false;

    void dumpCapabilities();
    uint8_t decodeBusStart(uint16_t vmCap, uint16_t vmConfig);

    // Fase 2: recorre la ECAM y publica un nub por dispositivo presente.
    // @return numero de nubs publicados.
    uint32_t enumerateChildren();
    bool publishChild(uint8_t bus, uint8_t devfn, uint8_t headerType);
    void publishNvmeShim(uint8_t bus, uint8_t devfn);
};

#endif