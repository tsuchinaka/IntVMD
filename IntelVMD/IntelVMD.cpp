#include "IntelVMD.h"

#include "VMDLogic.hpp"

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
        IOMemoryDescriptor *bar0 =
            fPCIDevice->getDeviceMemoryWithIndex(kCfgBarIndex);
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
              vendor, device, rev, (uint64_t)(fCfgBarMap->getLength() >> 10));
        fCanBypassMsiRemap = vmd_can_bypass_msi_remap((uint32_t)device << 16 | vendor);
    }

    dumpCapabilities();

    {
        uint32_t found = enumerateChildren();
        IOLog("IntelVMD: %u dispositivos encontrados en la ECAM\n", found);
    }

    registerService();
    return true;

fail:
    stop(provider);
    return false;
}

void IntelVMD::dumpCapabilities() {
    uint16_t vmCap = fPCIDevice->configRead16(kRegVMCap);
    uint16_t vmConfig = fPCIDevice->configRead16(kRegVMConfig);
    uint32_t vmLock = fPCIDevice->configRead32(kRegVMLock);

    VMDRegs regs = vmd_decode(vmCap, vmConfig);
    fBusStart = vmd_bus_start(regs.bus_restrict_cap, regs.bus_restrict_cfg);
    fMsiRemapEnabled = regs.msi_remap_enabled;

    IOLog("IntelVMD: VMCAP=0x%04x VMCONFIG=0x%04x VMLOCK=0x%08x\n",
          vmCap, vmConfig, vmLock);
    IOLog("IntelVMD: busStart=%u msiRemap=%s canBypass=%s busRestrictCap=%d cfg=%u\n",
          fBusStart, fMsiRemapEnabled ? "ON" : "OFF",
          fCanBypassMsiRemap ? "si" : "no",
          regs.bus_restrict_cap ? 1 : 0, regs.bus_restrict_cfg);

    if (fBusStart == 0xFF) {
        IOLog("IntelVMD: VMCONFIG con bus-restrict desconocido; no se enumera\n");
        return;
    }

    fBusCount = vmd_cfgbar_bus_count(fCfgBarMap->getLength(), fBusStart);
    IOLog("IntelVMD: ECAM cubre %u buses a partir de %u (CFGBAR %llu MB)\n",
          fBusCount, fBusStart,
          (uint64_t)(fCfgBarMap->getLength() >> 20));
}

bool IntelVMD::ecamRead32(uint32_t offset, uint32_t &value) const {
    if (fCfgBarMap == nullptr) return false;
    if ((uint64_t)offset + sizeof(uint32_t) > fCfgBarMap->getLength()) {
        return false;
    }
    volatile uint32_t *addr =
        (volatile uint32_t *)((volatile uint8_t *)fCfgBarMap->getAddress() + offset);
    value = *addr;
    return true;
}

bool IntelVMD::readHeader(uint32_t ecamOffset, VMDFoundDevice &out) const {
    uint32_t id = 0;
    if (!ecamRead32(ecamOffset, id)) return false;

    // Dispositivo ausente: Linux trata vendor 0xFFFF como hueco.
    if ((id & 0xFFFFu) == 0xFFFFu || vmd_config_is_empty(id)) {
        return false;
    }

    out.vendorId = (uint16_t)(id & 0xFFFFu);
    out.deviceId = (uint16_t)((id >> 16) & 0xFFFFu);

    uint32_t d1 = 0;
    if (!ecamRead32(ecamOffset + 0x04, d1)) return false;
    out.revision = (uint8_t)(d1 & 0xFFu);        // offset 0x08
    out.headerType = (uint8_t)((d1 >> 16) & 0xFFu); // offset 0x0E

    uint32_t d2 = 0;
    if (!ecamRead32(ecamOffset + 0x08, d2)) return false;
    out.interruptLine = (uint8_t)(d2 & 0xFFu);        // offset 0x3C
    out.interruptPin = (uint8_t)((d2 >> 8) & 0xFFu);  // offset 0x3D

    uint32_t d3 = 0;
    if (!ecamRead32(ecamOffset + 0x0C, d3)) return false;
    // offset 0x09..0x0B = revision(0x08 byte alto), prog-if, subclass, class
    out.classCodeSub = (uint8_t)((d3 >> 8) & 0xFFu);  // offset 0x0A
    out.classCodeBase = (uint8_t)((d3 >> 16) & 0xFFu); // offset 0x0B
    out.classCode = ((d3 >> 8) & 0x00FFFF00u) | ((d3 >> 16) & 0xFFu);
    out.multiFunction = ((d3 >> 24) & 0x80u) != 0;    // offset 0x0E bit 7

    return true;
}

void IntelVMD::logDevice(const VMDFoundDevice &dev) const {
    const char *kind = "otro";
    if (dev.classCodeBase == 0x01) kind = "controller";
    else if (dev.classCodeBase == 0x02) kind = "red";
    else if (dev.classCodeBase == 0x03) kind = "disco/almacen";
    else if (dev.classCodeBase == 0x06) kind = "puente";
    else if (dev.classCodeBase == 0x08) kind = "almacen generico";
    else if (dev.classCodeBase == 0x0C) kind = "serial/USB";
    else if (dev.classCodeBase == 0x01 && dev.classCodeSub == 0x06) kind = "SATA";

    IOLog("IntelVMD:   bus %3u devfn 0x%02x  %04x:%04x rev 0x%02x "
          "class %02x%02x hdr %u irq %u/%u mf %d  [%s]\n",
          dev.bus, dev.devFn, dev.vendorId, dev.deviceId, dev.revision,
          dev.classCodeBase, dev.classCodeSub, dev.headerType,
          dev.interruptLine, dev.interruptPin,
          dev.multiFunction ? 1 : 0, kind);
}

uint32_t IntelVMD::enumerateChildren() {
    uint32_t found = 0;

    if (fCfgBarMap == nullptr || fBusCount == 0) {
        IOLog("IntelVMD: sin ventana ECAM utilizable\n");
        return 0;
    }

    for (uint32_t busIdx = 0; busIdx < fBusCount; busIdx++) {
        uint8_t busNum = (uint8_t)(fBusStart + busIdx);

        for (uint8_t dev = 0; dev < 32; dev++) {
            bool fn0Absent = false;

            for (uint8_t fn = 0; fn < 8; fn++) {
                uint8_t devfn = (uint8_t)((dev << 3) | fn);
                uint32_t ecamOff = vmd_ecam_offset(busNum, fBusStart, devfn, 0x0000);
                if (ecamOff == 0xFFFFFFFFu) {
                    continue;
                }

                VMDFoundDevice info = {};
                if (!readHeader(ecamOff, info)) {
                    // Regla PCI: si la funcion 0 no esta, el resto del
                    // dispositivo tampoco. Solo se puede continuar si el
                    // header de la fn0 era multi-function.
                    if (fn == 0) fn0Absent = true;
                    break;
                }

                info.bus = busNum;
                info.devFn = devfn;
                logDevice(info);
                found++;

                // Un puente solo puede tener funciones adicionales con
                // header type 1 (PCI-to-PCI bridge).
                if (info.headerType == 0x01 && fn > 0) {
                    break;
                }
            }

            (void)fn0Absent;
        }
    }

    return found;
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