#include "IntelVMD.h"

#include "Logic/VMDLogic.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>

OSDefineMetaClassAndStructors(VMDPciDevice, IOPCIDevice);

// ---------------------------------------------------------------------
// VMDPciDevice: nub hijo con config space redirigido a la ECAM del VMD.
// ---------------------------------------------------------------------

bool VMDPciDevice::attachToECAM(IOMemoryMap *ecamBase, uint32_t ecamOffset,
                                 uint32_t ecamLimit, uint8_t bus, uint8_t devfn) {
    if (ecamBase == nullptr || ecamOffset == 0xFFFFFFFFu) return false;

    // El ECAM limit debe caber en la ventana mapeada; si no, recortamos al
    // mapa real para no leer fuera de la BAR0 asignada al driver.
    uint64_t mapLen = ecamBase->getLength();
    if ((uint64_t)ecamOffset >= mapLen) return false;

    uint32_t limit = ecamLimit;
    if ((uint64_t)ecamOffset + (uint64_t)limit > mapLen) {
        limit = (uint32_t)(mapLen - (uint64_t)ecamOffset);
    }

    fECAMBase = ecamBase;
    fECAMOffset = ecamOffset;
    fECAMLimit = limit;
    fBus = bus;
    fDevFn = devfn;
    return true;
}

bool VMDPciDevice::ecamRead(uint64_t offset, uint32_t &value) const {
    if (fECAMBase == nullptr) return false;
    // Un byte/dword tiene que caber entero en la ventana del nodo.
    if (offset + sizeof(uint32_t) > (uint64_t)fECAMLimit) return false;
    volatile uint32_t *addr =
        (volatile uint32_t *)((uint8_t *)fECAMBase->getAddress() +
                              (uint64_t)fECAMOffset + offset);
    value = *addr;
    return true;
}

bool VMDPciDevice::ecamWrite(uint64_t offset, uint32_t value) const {
    if (fECAMBase == nullptr) return false;
    if (offset + sizeof(uint32_t) > (uint64_t)fECAMLimit) return false;
    volatile uint32_t *addr =
        (volatile uint32_t *)((uint8_t *)fECAMBase->getAddress() +
                              (uint64_t)fECAMOffset + offset);
    *addr = value;
    return true;
}

// La ECAM solo soporta accesos de 32 bits; los de 8/16 se desdoblan.
uint32_t VMDPciDevice::configRead32(uint64_t offset) {
    offset &= 0xFFCu; // la ECAM ignora los bits bajos (no hay sub-dword)
    uint32_t v = 0;
    if (!ecamRead(offset, v)) return 0xFFFFFFFFu;
    return v;
}

void VMDPciDevice::configWrite32(uint64_t offset, uint32_t value) {
    offset &= 0xFFCu;
    ecamWrite(offset, value);
}

uint8_t VMDPciDevice::configRead8(uint64_t offset) {
    uint32_t v = configRead32(offset);
    return (uint8_t)((v >> ((offset & 0x3) * 8)) & 0xFFu);
}

uint16_t VMDPciDevice::configRead16(uint64_t offset) {
    uint32_t v = configRead32(offset);
    return (uint16_t)((v >> ((offset & 0x2) * 8)) & 0xFFFFu);
}

void VMDPciDevice::configWrite8(uint64_t offset, uint8_t value) {
    uint32_t shift = (offset & 0x3) * 8;
    uint32_t mask = 0xFFu << shift;
    uint32_t v = configRead32(offset);
    configWrite32(offset, (v & ~mask) | ((uint32_t)value << shift));
}

void VMDPciDevice::configWrite16(uint64_t offset, uint16_t value) {
    uint32_t shift = (offset & 0x2) * 8;
    uint32_t mask = 0xFFFFu << shift;
    uint32_t v = configRead32(offset);
    configWrite32(offset, (v & ~mask) | ((uint32_t)value << shift));
}

bool VMDPciDevice::primeIdentityFromECAM() {
    uint32_t id = configRead32(0x00);
    if (vmd_config_is_empty(id)) return false;

    uint8_t header = configRead8(0x0E);
    // Header type 2 (CardBus) no esta soportado por el camino de ECAM.
    if (header == 0x02) return false;

    // setConfigMapIgnored / setBusNumberCorre el registro interno de
    // IOPCIDevice para que el matching de kexts use estos valores.
    setVendorID(id & 0xFFFFu);
    setDeviceID((id >> 16) & 0xFFFFu);
    setRevisionID(configRead8(0x08));
    setClassCode(configRead32(0x09));

    return true;
}

// ---------------------------------------------------------------------
// IntelVMD
// ---------------------------------------------------------------------

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
              vendor, device, rev, fCfgBarMap->getLength() >> 10);
        fCanBypassMsiRemap = vmd_can_bypass_msi_remap((uint32_t)device << 16 | vendor);
    }

    dumpCapabilities();

    // Fase 2: publicar los dispositivos hijos visibles a traves de la ECAM.
    {
        uint32_t published = enumerateChildren();
        IOLog("IntelVMD: %u dispositivos hijos publicados en la ECAM\n", published);
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
    fBusStart = decodeBusStart(vmCap, vmConfig);
    fMsiRemapEnabled = regs.msi_remap_enabled;

    IOLog("IntelVMD: VMCAP=0x%04x VMCONFIG=0x%04x VMLOCK=0x%08x\n",
          vmCap, vmConfig, vmLock);
    IOLog("IntelVMD: bus Start=%u MSI-Remap=%s CAN_BYPASS=%s\n",
          fBusStart, fMsiRemapEnabled ? "ON" : "OFF",
          fCanBypassMsiRemap ? "si" : "no");

    if (fBusStart == 0xFF) {
        IOLog("IntelVMD: VMCONFIG con bus-restrict desconocido, abortando\n");
    }

    // Cuantos buses caben en el CFGBAR (Linux vmd_cfgbar_ecam_space).
    fBusCount = vmd_cfgbar_bus_count(fCfgBarMap->getLength(), fBusStart);
    IOLog("IntelVMD: ECAM cubre %u buses desde %u\n", fBusCount, fBusStart);

    // Fase 3 pendiente: si MSI-Remap=ON y no se puede bypassear (caso 9A0B),
    // hay que crear un dominio de interrupciones propio antes de que
    // IONVMeFamily intente usar MSI-X; si no, el dispositivo no genera IRQ.
}

uint8_t IntelVMD::decodeBusStart(uint16_t vmCap, uint16_t vmConfig) {
    VMDRegs regs = vmd_decode(vmCap, vmConfig);
    if (!regs.bus_restrict_cap) return 0;
    switch (regs.bus_restrict_cfg) {
        case 0: return 0;
        case 1: return 128;
        default: return 224; // cases 2 y 3, igual que Linux
    }
}

uint32_t IntelVMD::enumerateChildren() {
    uint32_t published = 0;

    if (fCfgBarMap == nullptr || fBusCount == 0) {
        return 0;
    }

    for (uint32_t bus = 0; bus < fBusCount; bus++) {
        uint8_t busNum = (uint8_t)(fBusStart + bus);

        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint8_t devfn = (uint8_t)((dev << 3) | fn);

                uint32_t ecamOff =
                    vmd_ecam_offset(busNum, fBusStart, devfn, 0x0000);
                // El chequeo de rango incluye el espacio de config del header
                // tipo 0 (256 bytes) y el de tipo 1 (4096).
                if (!vmd_ecam_in_range(ecamOff, 4096, fCfgBarMap->getLength())) {
                    continue;
                }

                // Lee el vendor/device directamente del mapa del VMD para
                // decidir rapido si hay algo antes de crear el nub.
                volatile uint32_t *addr =
                    (volatile uint32_t *)((uint8_t *)fCfgBarMap->getAddress() +
                                          ecamOff);
                uint32_t id = *addr;
                if (vmd_config_is_empty(id)) {
                    // Function 0 vacio -> la funcion siguiente tambien lo
                    // estara (regla PCI). Salimos del bucle de funciones.
                    break;
                }

                uint8_t header = (uint8_t)((addr[0x0E / 4] >> ((0x0E & 0x3) * 8)) & 0xFF);
                if (header == 0x02) {
                    // CardBus: fuera de alcance de la ECAM simple.
                    continue;
                }

                if (publishChild(busNum, devfn, header)) {
                    published++;
                }

                // Un puente solo puede tener function 0 con.header 1.
                if (header == 0x01 && fn > 0) {
                    break;
                }
            }
        }
    }
    return published;
}

bool IntelVMD::publishChild(uint8_t bus, uint8_t devfn, uint8_t headerType) {
    uint32_t ecamOff = vmd_ecam_offset(bus, fBusStart, devfn, 0x0000);
    if (ecamOff == 0xFFFFFFFFu) return false;

    // El limite util depende del tipo de cabecera.
    uint32_t window = (headerType == 0x01) ? 4096 : 256;

    VMDPciDevice *dev = OSTypeAlloc(VMDPciDevice);
    if (dev == nullptr) return false;

    if (!dev->attachToECAM(fCfgBarMap, ecamOff, window, bus, devfn)) {
        dev->release();
        return false;
    }

    if (!dev->primeIdentityFromECAM()) {
        dev->release();
        return false;
    }

    // Publica como hijo del propio VMD para que el matching de IONVMeFamily
    // (IOProviderClass IOPCIDevice) lo encuentre en el plano de servicios.
    if (!dev->attach(this)) {
        dev->release();
        return false;
    }

    IOLog("IntelVMD: nub %04x:%04x en bus %u devfn 0x%02x (ECAM 0x%08x)\n",
          dev->configRead16(kIOPCIConfigVendorID),
          dev->configRead16(kIOPCIConfigDeviceID),
          bus, devfn, ecamOff);

    return true;
}

void IntelVMD::publishNvmeShim(uint8_t bus, uint8_t devfn) {
    // Reservado: publicar el NVMe con su BAR de EIO para que IONVMeFamily lo
    // abra. Depende de Fase 3 (dominio de IRQ) para que las I/O funcionen.
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