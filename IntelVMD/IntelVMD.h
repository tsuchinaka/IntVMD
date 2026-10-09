#ifndef INTEL_VMD_H
#define INTEL_VMD_H

#include <IOKit/IOService.h>
// OJO: NO existe IOKit/IOMemoryMap.h en Kernel.framework. IOMemoryMap viene
// declarado en IOKit/IOMemoryDescriptor.h, que es el header correcto.
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIBridge.h>   // IOPCIBridge + IOPCI2PCIBridge
#include <IOKit/pci/IOPCIDevice.h>

#include "VMDLogic.hpp"   // nucleo compartido (tambien lo usa el driver UEFI)

// Fase 2b-enum: publica el dominio PCI que hay detras del VMD como un puente
// PCI sintetico con hijos IOPCIDevice.
//
// OJO DE DISENO (verificado en el fuente de Apple, IOPCIFamily):
// - Se hereda de IOPCI2PCIBridge, NO de IOPCIBridge a secas: solo el
//   configure() heredado inicializa el hostBridgeData privado, sin el cual
//   initializeNub revienta.
// - El probeBus() heredado NO sirve: itera estructuras privadas del
//   configurador. Se sobreescribe y se publican los nubs a mano con
//   createNub/init/initializeNub/publishNub (todo publico).
// - getNubResources() es virtual PRIVADO pero sobreescribirlo es legal en C++
//   y el despacho dinamico desde codigo Apple invoca el override.
// - LIMITE ASUMIDO: MSI/MSI-X no es entregable con solo KPI publica
//   (resolveMSIInterrupts es privado y no virtual). Este kext entrega
//   enumeracion + matching + BARs + config R/W, con traza super-verbose para
//   validar el hardware real. Las interrupciones quedan para mas adelante.
class IntelVMD : public IOPCI2PCIBridge {
    OSDeclareDefaultStructors(IntelVMD);
    typedef IOPCI2PCIBridge super;

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    // Rango de buses del dominio VMD (de VMCAP@0x40 / VMCONFIG@0x44).
    UInt8 firstBusNum(void) override;
    UInt8 lastBusNum(void) override;

    // Acceso a config-space por la ECAM del CFGBAR (BAR0 del VMD).
    UInt32 configRead32(IOPCIAddressSpace space, UInt8 offset) override;
    UInt16 configRead16(IOPCIAddressSpace space, UInt8 offset) override;
    UInt8 configRead8(IOPCIAddressSpace space, UInt8 offset) override;
    void configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data) override;
    void configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data) override;
    void configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data) override;

    // Publicacion de hijos desde la ECAM (el heredado no sirve: lee
    // estructuras privadas del configurador).
    void probeBus(IOService *provider, UInt8 busNum) override;

    // Adopcion de BARs por ECAM (sin reubicar: el firmware ya los asigno).
    IOReturn getNubResources(IOService *nub) override;

    // Stubs obligatorios (virtuales puros): un dominio VMD no tiene link PCIe
    // que reentrenar desde aqui.
    IOReturn setLinkSpeed(tIOPCILinkSpeed linkSpeed, bool retrain) override;
    IOReturn getLinkSpeed(tIOPCILinkSpeed *linkSpeed) override;

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

    // Lectura/escritura dword cruda a la ECAM. false si fuera de la BAR0.
    bool ecamRead32(uint8_t bus, uint8_t devFn, uint16_t reg, uint32_t &value) const;
    void ecamWrite32(uint8_t bus, uint8_t devFn, uint16_t reg, uint32_t value);

    // Publica un hijo presente en (bus, dev, fn). Devuelve true si se publico.
    bool publishChild(uint8_t bus, uint8_t dev, uint8_t fn, uint32_t &index);

    // Tamano de un BAR por sizing estandar (salvar, escribir todo-unos,
    // leer mascara, restaurar + read-back). 0 si no se puede medir.
    uint64_t sizedBarLength(uint8_t bus, uint8_t devFn, uint16_t barOff,
                            bool is64);
};

#endif
