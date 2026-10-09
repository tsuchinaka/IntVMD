#ifndef INTEL_VMD_H
#define INTEL_VMD_H

#include <IOKit/IOService.h>
// OJO: NO existe IOKit/IOMemoryMap.h en Kernel.framework. IOMemoryMap viene
// declarado en IOKit/IOMemoryDescriptor.h, que es el header correcto.
// (El include equivocado hacia fallar el build entero con
//  "'IOKit/IOMemoryMap.h' file not found".)
#include <IOKit/IOMemoryDescriptor.h>

#include "VMDLogic.hpp"   // nucleo compartido (tambien lo usa el driver UEFI)

class IOPCIDevice;

class IntelVMD : public IOService {
    OSDeclareDefaultStructors(IntelVMD);
    typedef IOService super;

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

private:
    static constexpr uint8_t kCfgBarIndex = 0;   // VMD_CFGBAR en Linux vmd.c
    static constexpr uint16_t kRegVMCap = 0x40;
    static constexpr uint16_t kRegVMConfig = 0x44;
    static constexpr uint16_t kRegVMLock = 0x70;

    IOPCIDevice *fPCIDevice = nullptr;
    IOMemoryMap *fCfgBarMap = nullptr;

    uint8_t fBusStart = 0;
    uint32_t fBusCount = 0;
    bool fMsiRemapEnabled = false;
    bool fCanBypassMsiRemap = false;

    void dumpCapabilities();

    // Fase 2a: recorre la ventana ECAM y registra en el log del sistema cada
    // dispositivo presente, para validar sobre hardware real que el
    // direccionamiento y el rango de buses son correctos.
    // @return numero de dispositivos encontrados.
    uint32_t enumerateChildren();

    // Lee la cabecera PCI desde la ECAM del VMD. Devuelve false si el
    // dispositivo esta ausente (vendor 0xFFFF) o si el offset cae fuera de la
    // BAR0 mapeada.
    bool readHeader(uint32_t ecamOffset, VMDFoundDevice &out) const;

    // Acceso dword a la ECAM. Devuelve false si se sale de la BAR0.
    bool ecamRead32(uint32_t offset, uint32_t &value) const;

    void logDevice(const VMDFoundDevice &dev) const;
};

#endif