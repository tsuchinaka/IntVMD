#pragma once
// stdint.h (C), NO <cstdint>: el wrapper de libc++ no resuelve al compilar un
// kext en modo kernel y rompe con "tried including <stdint.h> but didn't find
// libc++". stdint.h lo resuelve el SDK en ambos entornos (host y kext).
#include <stdint.h>

// Logica pura del controlador VMD de Intel, extraida de
// drivers/pci/controller/vmd.c de Linux (GPL) para poder testearla en el host
// sin macOS. El kext IntelVMD solo aporta el acceso MMIO y la publicacion.

struct VMDRegs {
    bool bus_restrict_cap = false; // VMCAP bit 0
    uint8_t bus_restrict_cfg = 0;  // VMCONFIG bits 9:8
    // OJO con la polaridad: en Linux vmd_set_msi_remapping() hace
    //   enable ? (reg & ~VMCONFIG_MSI_REMAP) : (reg | VMCONFIG_MSI_REMAP)
    // o sea remapping ACTIVO == bit a CERO. Verificado contra el master.
    bool msi_remap_enabled = false;
};

// Dispositivo encontrado en la ECAM del dominio VMD. POD, sin inicializadores
// por defecto, para que lo puedan usar tanto el kext (IOKit) como el driver
// UEFI (EDK2) sin duplicar la definicion.
struct VMDFoundDevice {
    uint8_t bus;        // numero de bus real (no el relativo a la ECAM)
    uint8_t devFn;      // (dev << 3) | fn
    uint16_t vendorId;
    uint16_t deviceId;
    uint8_t revision;
    uint8_t headerType;
    uint32_t classCode;
    bool multiFunction;
    uint8_t classCodeSub;
    uint8_t classCodeBase;
    uint8_t interruptLine;
    uint8_t interruptPin;
};

// busn_start segun VMCAP/VMCONFIG. 0xFF = configuracion invalida.
// Si BUS_RESTRICT_CAP es 0, Linux deduce el bus inicial de BAR4 (Base ID),
// no de VMCONFIG: por eso aqui se devuelve 0 y el kext debe usar BAR4.
uint8_t vmd_bus_start(bool bus_restrict_cap, uint8_t bus_restrict_cfg);

// Offset ECAM dentro del CFGBAR. 0xFFFFFFFF si bus < bus_start.
// offset = ((bus - bus_start) << 20) | (devfn << 12) | (reg & 0xFFF)
uint32_t vmd_ecam_offset(uint8_t bus, uint8_t bus_start, uint8_t devfn, uint16_t reg);

// Linux descarta el acceso si "offset + len >= resource_size(CFGBAR)"
// (vmd_cfg_addr). Mismo criterio, expressed sobre el tamano en bytes.
bool vmd_ecam_in_range(uint32_t offset, uint32_t len, uint64_t cfgbar_size);

// Number of ECAM buses que caben en el CFGBAR (Linux vmd_cfgbar_ecam_space).
uint32_t vmd_cfgbar_bus_count(uint64_t cfgbar_size, uint8_t bus_start);

// VMD_FEAT_CAN_BYPASS_MSI_REMAP se decide por device ID, NO leyendo VMCONFIG.
// Si el dispositivo NO puede bypassear, hay que montar el demux MSI-X siempre.
bool vmd_can_bypass_msi_remap(uint32_t device_id);

// Decodifica VMCAP (@0x40) y VMCONFIG (@0x44).
VMDRegs vmd_decode(uint16_t vmcap, uint16_t vmconfig);

// Clasica de configur space: function 0 con cabecera 0xFFFFFFFF es un hueco.
bool vmd_config_is_empty(uint32_t vendor_device);