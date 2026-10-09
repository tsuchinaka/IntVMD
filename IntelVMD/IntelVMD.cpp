#include "IntelVMD.h"

#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>

OSDefineMetaClassAndStructors(IntelVMD, IOService);

IOService *IntelVMD::probe(IOService *provider, SInt32 *score) {
    if (!super::probe(provider, score)) {
        return nullptr;
    }
    if (OSDynamicCast(IOPCIDevice, provider) == nullptr) {
        return nullptr;
    }
    return this;
}

bool IntelVMD::start(IOService *provider) {
    if (!super::start(provider)) {
        return false;
    }

    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (fPCIDevice == nullptr) {
        goto fail;
    }
    fPCIDevice->retain();

    // Necesario para acceder a las BARs MMIO (CFGBAR/MEMBARs).
    fPCIDevice->setMemoryEnable(true);

    {
        IOMemoryDescriptor *bar0 = fPCIDevice->getDeviceMemoryWithIndex(kCfgBarIndex);
        if (bar0 == nullptr) {
            IOLog("IntelVMD: no se pudo obtener BAR0 (CFGBAR)\n");
            goto fail;
        }
        fCfgBarMap = bar0->map();
        if (fCfgBarMap == nullptr) {
            IOLog("IntelVMD: no se pudo mapear BAR0\n");
            goto fail;
        }
    }

    {
        uint16_t vendor = fPCIDevice->configRead16(kIOPCIConfigVendorID);
        uint16_t device = fPCIDevice->configRead16(kIOPCIConfigDeviceID);
        uint8_t rev = fPCIDevice->configRead8(kIOPCIConfigRevisionID);
        IOLog("IntelVMD: attach a %04x:%04x rev 0x%02x, CFGBAR %llu KB\n",
              vendor, device, rev,
              fCfgBarMap->getLength() >> 10);
    }

    dumpCapabilities();
    registerService();
    return true;

fail:
    stop(provider);
    return false;
}

void IntelVMD::dumpCapabilities() {
    uint16_t vmCap    = fPCIDevice->configRead16(kRegVMCap);
    uint16_t vmConfig = fPCIDevice->configRead16(kRegVMConfig);
    uint32_t vmLock   = fPCIDevice->configRead32(kRegVMLock);

    uint8_t busStart = decodeBusStart(vmCap, vmConfig);
    // Semantica segun Linux vmd.c: bit 1 de VMCONFIG a 0 = remap activo.
    bool msiRemap = (vmConfig & 0x2) == 0;

    IOLog("IntelVMD: VMCAP=0x%04x VMCONFIG=0x%04x VMLOCK=0x%08x\n",
          vmCap, vmConfig, vmLock);
    IOLog("IntelVMD: bus Start=%u MSI-Remap=%s\n",
          busStart, msiRemap ? "ON" : "OFF");

    // TODO Fase 2: usar busStart + CFGBAR como ventana ECAM para enumerar
    // los root ports hijos (offset = ECAM(bus - busStart, devfn, reg)).
    // TODO Fase 3: si MSI-Remap=ON, reservar vectores MSI-X propios y
    // demultiplexar (equivalente al irq_domain de Linux).
}

uint8_t IntelVMD::decodeBusStart(uint16_t vmCap, uint16_t vmConfig) {
    if ((vmCap & 0x1) == 0) {
        return 0;
    }
    switch ((vmConfig >> 8) & 0x3) {
        case 0:  return 0;
        case 1:  return 128;
        default: return 224; // casos 2 y 3, igual que Linux
    }
}

void IntelVMD::stop(IOService *provider) {
    if (fCfgBarMap) {
        fCfgBarMap->release();
        fCfgBarMap = nullptr;
    }
    if (fPCIDevice) {
        fPCIDevice->release();
        fPCIDevice = nullptr;
    }
    super::stop(provider);
}

void IntelVMD::free() {
    super::free();
}
