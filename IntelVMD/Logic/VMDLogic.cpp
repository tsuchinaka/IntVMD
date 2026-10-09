#include "VMDLogic.hpp"

uint8_t vmd_bus_start(bool bus_restrict_cap, uint8_t bus_restrict_cfg) {
    if (!bus_restrict_cap) return 0;
    switch (bus_restrict_cfg) {
        case 0: return 0;
        case 1: return 128;
        case 2: return 224;
        case 3: return 224;
        default: return 0xFF;
    }
}

uint32_t vmd_ecam_offset(uint8_t bus, uint8_t bus_start, uint8_t devfn, uint16_t reg) {
    if (bus < bus_start) return 0xFFFFFFFFu;
    uint32_t bus_off = (uint32_t)(bus - bus_start) << 20;
    uint32_t dev_off = (uint32_t)devfn << 12;
    return bus_off | dev_off | (reg & 0x0FFFu);
}

bool vmd_ecam_in_range(uint32_t offset, uint32_t len, uint64_t cfgbar_size) {
    if (offset == 0xFFFFFFFFu) return false;
    return (uint64_t)offset + (uint64_t)len < cfgbar_size;
}

uint32_t vmd_cfgbar_bus_count(uint64_t cfgbar_size, uint8_t bus_start) {
    if (bus_start == 0xFF) return 0;
    uint32_t buses = (uint32_t)(cfgbar_size >> 20);
    return buses;
}

bool vmd_can_bypass_msi_remap(uint32_t device_id) {
    // Solo estos IDs de la tabla vmd_ids llevan VMD_FEAT_CAN_BYPASS_MSI_REMAP.
    // El 9A0B de esta maquina NO lo lleva -> el demux MSI-X es obligatorio.
    switch (device_id) {
        case 0x28C08086: return true;
        case 0x28C18086: return true;
        default: return false;
    }
}

VMDRegs vmd_decode(uint16_t vmcap, uint16_t vmconfig) {
    VMDRegs r;
    r.bus_restrict_cap = (vmcap & 0x1u) != 0;
    r.bus_restrict_cfg = (uint8_t)((vmconfig >> 8) & 0x3u);
    // Remap activo <=> bit a cero (vmd_set_msi_remapping, linux vmd.c:389).
    r.msi_remap_enabled = (vmconfig & 0x2u) == 0;
    return r;
}

bool vmd_config_is_empty(uint32_t vendor_device) {
    return vendor_device == 0xFFFFFFFFu;
}