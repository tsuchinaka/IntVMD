// IntelVMD.kext — Fase 2b-enum: puente PCI sintetico sobre el dominio VMD.
//
// Publica el bus oculto del VMD (8086:9A0B y familia) como hijos IOPCIDevice
// usando SOLO API publica de IOPCIFamily, con traza super-verbose (IOLog en
// cada paso) para validar sobre hardware real.
//
// Limite asumido y documentado: MSI/MSI-X no entregable con KPI publica.

#include "IntelVMD.h"

#include "VMDLogic.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/OSByteOrder.h>

OSDefineMetaClassAndStructors(IntelVMD, IOPCI2PCIBridge);

// ---------------------------------------------------------------------------
// matching / ciclo de vida
// ---------------------------------------------------------------------------

IOService *IntelVMD::probe(IOService *provider, SInt32 *score) {
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!super::probe(provider, score)) {
        IOLog("IntelVMD: probe: super rechaza al provider\n");
        return nullptr;
    }
    if (pci == nullptr) {
        IOLog("IntelVMD: probe: provider no es IOPCIDevice\n");
        return nullptr;
    }
    IOLog("IntelVMD: probe: provider %04x:%04x\n",
          pci->configRead16(kIOPCIConfigVendorID),
          pci->configRead16(kIOPCIConfigDeviceID));
    return this;
}

bool IntelVMD::start(IOService *provider) {
    IOLog("IntelVMD: start\n");

    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (fPCIDevice == nullptr) {
        IOLog("IntelVMD: start: sin IOPCIDevice\n");
        goto fail;
    }
    fPCIDevice->retain();

    // Necesario para acceder a las BARs MMIO (CFGBAR/MEMBARs).
    fPCIDevice->setMemoryEnable(true);

    {
        IOMemoryDescriptor *bar0 =
            fPCIDevice->getDeviceMemoryWithIndex(kCfgBarIndex);
        if (bar0 == nullptr) {
            IOLog("IntelVMD: start: sin BAR0 (CFGBAR)\n");
            goto fail;
        }
        fCfgBarMap = bar0->map();
        if (fCfgBarMap == nullptr) {
            IOLog("IntelVMD: start: BAR0 no mapeable\n");
            goto fail;
        }
        IOLog("IntelVMD: start: CFGBAR phys 0x%llx len %llu MB\n",
              (unsigned long long)fCfgBarMap->getPhysicalAddress(),
              (unsigned long long)(fCfgBarMap->getLength() >> 20));
    }

    {
        uint16_t vendor = fPCIDevice->configRead16(kIOPCIConfigVendorID);
        uint16_t device = fPCIDevice->configRead16(kIOPCIConfigDeviceID);
        uint8_t rev = fPCIDevice->configRead8(kIOPCIConfigRevisionID);
        uint16_t vmCap = fPCIDevice->configRead16(kRegVMCap);
        uint16_t vmConfig = fPCIDevice->configRead16(kRegVMConfig);
        uint32_t vmLock = fPCIDevice->configRead32(kRegVMLock);

        VMDRegs regs = vmd_decode(vmCap, vmConfig);
        fBusStart = vmd_bus_start(regs.bus_restrict_cap, regs.bus_restrict_cfg);
        fMsiRemapEnabled = regs.msi_remap_enabled;
        fCanBypassMsiRemap =
            vmd_can_bypass_msi_remap(((uint32_t)device << 16) | vendor);

        IOLog("IntelVMD: start: attach %04x:%04x rev 0x%02x\n",
              vendor, device, rev);
        IOLog("IntelVMD: start: VMCAP=0x%04x VMCONFIG=0x%04x VMLOCK=0x%08x\n",
              vmCap, vmConfig, vmLock);
        IOLog("IntelVMD: start: busStart=%u msiRemap=%s canBypass=%s "
              "busRestrictCap=%d cfg=%u\n",
              fBusStart, fMsiRemapEnabled ? "ON" : "OFF",
              fCanBypassMsiRemap ? "si" : "no",
              regs.bus_restrict_cap ? 1 : 0, regs.bus_restrict_cfg);

        if (fBusStart == 0xFF) {
            IOLog("IntelVMD: start: bus-restrict desconocido; no se publica\n");
            goto fail;
        }

        fBusCount = vmd_cfgbar_bus_count(fCfgBarMap->getLength(), fBusStart);
        IOLog("IntelVMD: start: publicando buses [%u..%u] (%u buses)\n",
              fBusStart, (unsigned)(fBusStart + fBusCount - 1), fBusCount);
        if (fBusCount == 0) {
            goto fail;
        }
    }

    // super::start hace configure() (heredado: copia hostBridgeData) y llama
    // a NUESTRO probeBus(), que publica los hijos. Despues registerService().
    if (!super::start(provider)) {
        IOLog("IntelVMD: start: super::start fallo\n");
        goto fail;
    }

    IOLog("IntelVMD: start: OK\n");
    return true;

fail:
    stop(provider);
    return false;
}

void IntelVMD::stop(IOService *provider) {
    IOLog("IntelVMD: stop\n");
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

// ---------------------------------------------------------------------------
// rango de buses
// ---------------------------------------------------------------------------

UInt8 IntelVMD::firstBusNum(void) {
    return fBusStart;
}

UInt8 IntelVMD::lastBusNum(void) {
    if (fBusCount == 0) {
        return fBusStart;
    }
    uint32_t last = (uint32_t)fBusStart + fBusCount - 1;
    return (last > 255) ? 255 : (UInt8)last;
}

// ---------------------------------------------------------------------------
// acceso ECAM crudo (como Linux vmd_cfg_addr: offset+len dentro de BAR0,
// bajo el mapa; el llamante serializa si hace falta)
// ---------------------------------------------------------------------------

bool IntelVMD::ecamRead32(uint8_t bus, uint8_t devFn, uint16_t reg,
                          uint32_t &value) const {
    if (fCfgBarMap == nullptr) {
        return false;
    }
    uint32_t offset = vmd_ecam_offset(bus, fBusStart, devFn, reg & 0xFFCu);
    if (offset == 0xFFFFFFFFu) {
        return false;
    }
    if (!vmd_ecam_in_range(offset, sizeof(uint32_t),
                           fCfgBarMap->getLength())) {
        return false;
    }
    value = *(volatile const uint32_t *)((volatile const uint8_t *)
                                             fCfgBarMap->getAddress() +
                                         offset);
    return true;
}

void IntelVMD::ecamWrite32(uint8_t bus, uint8_t devFn, uint16_t reg,
                           uint32_t value) {
    uint32_t offset;
    volatile uint32_t *addr;
    uint32_t back;

    if (fCfgBarMap == nullptr) {
        return;
    }
    offset = vmd_ecam_offset(bus, fBusStart, devFn, reg & 0xFFCu);
    if (offset == 0xFFFFFFFFu) {
        return;
    }
    if (!vmd_ecam_in_range(offset, sizeof(uint32_t),
                           fCfgBarMap->getLength())) {
        return;
    }
    addr = (volatile uint32_t *)((volatile uint8_t *)fCfgBarMap->getAddress() +
                                 offset);
    *addr = value;
    // read-back obligatorio (como Linux): el HW postea el write y el read
    // fuerza la complecion antes de retornar.
    back = *addr;
    (void)back;
}

// ---------------------------------------------------------------------------
// configRead/configWrite del puente (los hijos reenvian aqui)
// ---------------------------------------------------------------------------

static inline void vmdSpaceToBdf(IOPCIAddressSpace space, uint8_t &bus,
                                 uint8_t &devFn) {
    bus = space.s.busNum;
    devFn = (uint8_t)((space.s.deviceNum << 3) | space.s.functionNum);
}

UInt32 IntelVMD::configRead32(IOPCIAddressSpace space, UInt8 offset) {
    uint8_t bus, devFn;
    uint32_t value = 0xFFFFFFFFu;

    vmdSpaceToBdf(space, bus, devFn);
    if (bus < fBusStart || bus > lastBusNum()) {
        return 0xFFFFFFFFu;
    }
    if (!ecamRead32(bus, devFn, offset, value)) {
        return 0xFFFFFFFFu;
    }
    return value;
}

UInt16 IntelVMD::configRead16(IOPCIAddressSpace space, UInt8 offset) {
    uint32_t dword = configRead32(space, offset & 0xFCu);
    return (UInt16)((dword >> ((offset & 3) * 8)) & 0xFFFFu);
}

UInt8 IntelVMD::configRead8(IOPCIAddressSpace space, UInt8 offset) {
    uint32_t dword = configRead32(space, offset & 0xFCu);
    return (UInt8)((dword >> ((offset & 3) * 8)) & 0xFFu);
}

void IntelVMD::configWrite32(IOPCIAddressSpace space, UInt8 offset,
                             UInt32 data) {
    uint8_t bus, devFn;

    vmdSpaceToBdf(space, bus, devFn);
    if (bus < fBusStart || bus > lastBusNum()) {
        return;
    }
    IOLog("IntelVMD: configWrite32 bus %u devfn 0x%02x reg 0x%02x <- 0x%08x\n",
          bus, devFn, offset, data);
    ecamWrite32(bus, devFn, offset, data);
}

void IntelVMD::configWrite16(IOPCIAddressSpace space, UInt8 offset,
                             UInt16 data) {
    uint8_t bus, devFn;
    uint32_t dword, mask, merged;

    vmdSpaceToBdf(space, bus, devFn);
    if (bus < fBusStart || bus > lastBusNum()) {
        return;
    }
    if (!ecamRead32(bus, devFn, offset & 0xFCu, dword)) {
        return;
    }
    mask = 0xFFFFu << ((offset & 2) * 8);
    merged = (dword & ~mask) | (((uint32_t)data << ((offset & 2) * 8)) & mask);
    ecamWrite32(bus, devFn, offset & 0xFCu, merged);
}

void IntelVMD::configWrite8(IOPCIAddressSpace space, UInt8 offset,
                            UInt8 data) {
    uint8_t bus, devFn;
    uint32_t dword, shift, mask, merged;

    vmdSpaceToBdf(space, bus, devFn);
    if (bus < fBusStart || bus > lastBusNum()) {
        return;
    }
    if (!ecamRead32(bus, devFn, offset & 0xFCu, dword)) {
        return;
    }
    shift = (offset & 3) * 8;
    mask = 0xFFu << shift;
    merged = (dword & ~mask) | (((uint32_t)data << shift) & mask);
    ecamWrite32(bus, devFn, offset & 0xFCu, merged);
}

// ---------------------------------------------------------------------------
// publicacion de hijos
// ---------------------------------------------------------------------------

uint64_t IntelVMD::sizedBarLength(uint8_t bus, uint8_t devFn, uint16_t barOff,
                                  bool is64) {
    uint32_t origLo = 0, origHi = 0, maskLo = 0, maskHi = 0;
    uint64_t mask, len;

    if (!ecamRead32(bus, devFn, barOff, origLo)) {
        return 0;
    }
    if (is64 && !ecamRead32(bus, devFn, barOff + 4, origHi)) {
        return 0;
    }

    ecamWrite32(bus, devFn, barOff, 0xFFFFFFFFu);
    if (is64) {
        ecamWrite32(bus, devFn, barOff + 4, 0xFFFFFFFFu);
    }
    if (!ecamRead32(bus, devFn, barOff, maskLo)) {
        maskLo = 0;
    }
    if (is64 && !ecamRead32(bus, devFn, barOff + 4, maskHi)) {
        maskHi = 0;
    }

    // Restaurar (ecamWrite32 ya hace read-back).
    ecamWrite32(bus, devFn, barOff, origLo);
    if (is64) {
        ecamWrite32(bus, devFn, barOff + 4, origHi);
    }

    if (is64) {
        mask = ((uint64_t)(maskHi) << 32) | (maskLo & 0xFFFFFFF0u);
        if (mask == 0) {
            return 0;
        }
        len = (~mask) + 1;
    } else {
        mask = maskLo & 0xFFFFFFF0u;
        if (mask == 0) {
            return 0;
        }
        len = (~(mask | 0xFFFFFFFF00000000ULL)) + 1;
    }
    return len;
}

bool IntelVMD::publishChild(uint8_t bus, uint8_t dev, uint8_t fn,
                            uint32_t &index) {
    IOPCIAddressSpace space;
    UInt16 vid, did;
    UInt8 rev, progIf, clsSub, clsBase;
    UInt32 subsys;
    char name[32];
    uint32_t regCell;
    OSDictionary *dict = nullptr;
    OSData *reg = nullptr;
    IOPCIDevice *nub = nullptr;
    bool ok = false;

    space.bits = 0;
    space.s.busNum = bus;
    space.s.deviceNum = dev;
    space.s.functionNum = fn;
    space.s.space = 0;

    vid = configRead16(space, kIOPCIConfigVendorID);
    if (vid == 0xFFFFu) {
        return false;
    }
    did = configRead16(space, kIOPCIConfigDeviceID);
    rev = configRead8(space, kIOPCIConfigRevisionID);
    progIf = configRead8(space, 0x09);
    clsSub = configRead8(space, 0x0A);
    clsBase = configRead8(space, 0x0B);
    subsys = configRead32(space, 0x2C);

    IOLog("IntelVMD:   hijo bus %u dev %u fn %u: %04x:%04x rev 0x%02x "
          "class %02x%02x%02x subsys %08x\n",
          bus, dev, fn, vid, did, rev, clsBase, clsSub, progIf, subsys);

    dict = OSDictionary::withCapacity(8);
    if (dict == nullptr) {
        goto done;
    }

    // "reg" en formato OF: bit24 = config-space, B/D/F empaquetados.
    regCell = 0x01000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
              ((uint32_t)fn << 8);
    reg = OSData::withBytes(&regCell, sizeof(regCell));
    if (reg == nullptr) {
        goto done;
    }
    dict->setObject("reg", reg);

    dict->setObject("vendor-id", OSNumber::withNumber(vid, 32));
    dict->setObject("device-id", OSNumber::withNumber(did, 32));
    dict->setObject("revision-id", OSNumber::withNumber(rev, 32));
    dict->setObject("class-code",
                    OSNumber::withNumber(((uint32_t)clsBase << 16) |
                                             ((uint32_t)clsSub << 8) | progIf,
                                         32));
    dict->setObject("subsystem-vendor-id",
                    OSNumber::withNumber(subsys & 0xFFFFu, 32));
    dict->setObject("subsystem-id",
                    OSNumber::withNumber((subsys >> 16) & 0xFFFFu, 32));

    nub = createNub(dict);
    if (nub == nullptr) {
        IOLog("IntelVMD:   createNub fallo para %04x:%04x\n", vid, did);
        goto done;
    }
    if (!nub->init(dict)) {
        IOLog("IntelVMD:   init fallo para %04x:%04x\n", vid, did);
        goto done;
    }
    snprintf(name, sizeof(name), "pci%04x,%04x", vid, did);
    nub->setName(name);

    if (!initializeNub(nub, dict)) {
        IOLog("IntelVMD:   initializeNub fallo para %s\n", name);
        goto done;
    }
    if (!publishNub(nub, index)) {
        IOLog("IntelVMD:   publishNub fallo para %s\n", name);
        goto done;
    }

    IOLog("IntelVMD:   publicado %s (idx %u)\n", name, index);
    index++;
    ok = true;

done:
    if (nub != nullptr) {
        nub->release();
    }
    if (reg != nullptr) {
        reg->release();
    }
    if (dict != nullptr) {
        dict->release();
    }
    return ok;
}

void IntelVMD::probeBus(IOService *provider, UInt8 busNum) {
    uint32_t published = 0;
    uint32_t scanned = 0;
    uint32_t idx = 0;

    (void)provider;
    (void)busNum;

    IOLog("IntelVMD: probeBus: barriendo buses [%u..%u]\n", firstBusNum(),
          lastBusNum());

    for (uint32_t b = firstBusNum(); b <= lastBusNum(); b++) {
        uint8_t bus = (uint8_t)b;

        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t fn = 0; fn < 8; fn++) {
                IOPCIAddressSpace space;
                UInt16 vid;
                UInt8 hdr;

                space.bits = 0;
                space.s.busNum = bus;
                space.s.deviceNum = dev;
                space.s.functionNum = fn;
                space.s.space = 0;

                scanned++;
                vid = configRead16(space, kIOPCIConfigVendorID);
                if (vid == 0xFFFFu) {
                    if (fn == 0) {
                        break;  // fn0 ausente: no hay mas funciones
                    }
                    continue;
                }

                hdr = configRead8(space, 0x0E);
                if (publishChild(bus, dev, fn, idx)) {
                    published++;
                }

                if (fn == 0 && !(hdr & 0x80)) {
                    break;  // no multifuncion: solo fn0
                }
            }
        }
    }

    IOLog("IntelVMD: probeBus: %u leidos, %u publicados\n", scanned,
          published);
}

// ---------------------------------------------------------------------------
// recursos: adopcion de BARs por ECAM (sin reubicar)
// ---------------------------------------------------------------------------

IOReturn IntelVMD::getNubResources(IOService *nub) {
    IOPCIDevice *dev = OSDynamicCast(IOPCIDevice, nub);
    OSArray *mem = nullptr;
    uint32_t adopted = 0;
    uint8_t bus, d, f;

    if (dev == nullptr) {
        IOLog("IntelVMD: getNubResources: nub no es IOPCIDevice\n");
        return kIOReturnBadArgument;
    }

    bus = dev->getBusNumber();
    d = dev->getDeviceNumber();
    f = dev->getFunctionNumber();
    IOLog("IntelVMD: getNubResources bus %u dev %u fn %u\n", bus, d, f);

    mem = OSArray::withCapacity(6);
    if (mem == nullptr) {
        return kIOReturnNoMemory;
    }

    for (uint16_t barOff = 0x10; barOff <= 0x24; barOff += 4) {
        uint32_t bar = 0;
        uint64_t phys = 0;
        uint64_t len = 0;
        IODeviceMemory *m = nullptr;

        if (!ecamRead32(bus, (uint8_t)((d << 3) | f), barOff, bar)) {
            continue;
        }
        if (bar == 0) {
            continue;  // BAR no asignada: se omite
        }
        if (bar & 0x1u) {
            IOLog("IntelVMD:   BAR 0x%02x es I/O (0x%08x): se omite\n", barOff,
                  bar);
            continue;
        }

        if ((bar & 0x6u) == 0x4u) {
            // 64-bit: par alto en el siguiente offset.
            uint32_t hi = 0;
            if (!ecamRead32(bus, (uint8_t)((d << 3) | f), barOff + 4, hi)) {
                continue;
            }
            phys = (bar & 0xFFFFFFF0ULL) | ((uint64_t)hi << 32);
            len = sizedBarLength(bus, (uint8_t)((d << 3) | f), barOff, true);
            barOff += 4;
            IOLog("IntelVMD:   BAR64 0x%02x phys 0x%llx len 0x%llx\n",
                  (unsigned)(barOff - 4), phys, len);
        } else {
            phys = bar & 0xFFFFFFF0ULL;
            len = sizedBarLength(bus, (uint8_t)((d << 3) | f), barOff, false);
            IOLog("IntelVMD:   BAR32 0x%02x phys 0x%llx len 0x%llx\n", barOff,
                  phys, len);
        }

        if (len == 0) {
            IOLog("IntelVMD:   BAR 0x%02x sin tamano: se omite\n", barOff);
            continue;
        }

        m = IODeviceMemory::withRange((IOPhysicalAddress)phys,
                                      (IOPhysicalLength)len);
        if (m == nullptr) {
            continue;
        }
        // El tag casa BAR<->registro: getDeviceMemoryWithRegister compara
        // reg == (tag & 0xff).
        m->setTag((IOOptionBits)barOff);
        mem->setObject(m);
        m->release();
        adopted++;
    }

    nub->setProperty("IODeviceMemory", mem);
    nub->setProperty("IOPCIResourced", kOSBooleanTrue);
    mem->release();

    IOLog("IntelVMD: getNubResources: %u BARs adoptadas\n", adopted);
    return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// stubs obligatorios
// ---------------------------------------------------------------------------

IOReturn IntelVMD::setLinkSpeed(tIOPCILinkSpeed linkSpeed, bool retrain) {
    (void)linkSpeed;
    (void)retrain;
    return kIOReturnUnsupported;
}

IOReturn IntelVMD::getLinkSpeed(tIOPCILinkSpeed *linkSpeed) {
    if (linkSpeed != nullptr) {
        *linkSpeed = 0;
    }
    return kIOReturnUnsupported;
}
