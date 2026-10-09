#!/bin/bash
# Reconocimiento VMD en hardware real (Live USB Linux, ejecutar con sudo).
# Recoge todo lo necesario para portar la enumeracion a macOS.
set -u
OUT="vmd-recon-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"

echo "[*] Buscando dispositivo VMD..."
VMD=$(lspci -nn | grep -i "volume management\|vmd" | tee "$OUT/lspci-vmd.txt")
echo "$VMD"
BDF=$(echo "$VMD" | head -1 | awk '{print $1}')
if [ -z "${BDF:-}" ]; then
    echo "[!] No se encontro VMD. ¿Desactivado en BIOS o hardware sin VMD?"
    exit 1
fi
echo "[*] VMD en $BDF"

echo "[*] Volcado PCI extendido..."
lspci -vvv -s "$BDF" > "$OUT/lspci-vvv.txt" 2>&1
lspci -s "$BDF" -xxxx > "$OUT/lspci-xxxx.txt" 2>&1

echo "[*] Registros VMD (VMCAP 0x40 / VMCONFIG 0x44 / VMLOCK 0x70)..."
{
    echo -n "VMCAP(0x40.w)    = "; setpci -s "$BDF" 40.w
    echo -n "VMCONFIG(0x44.w) = "; setpci -s "$BDF" 44.w
    echo -n "VMLOCK(0x70.l)   = "; setpci -s "$BDF" 70.l
} > "$OUT/vmregs.txt" 2>&1
cat "$OUT/vmregs.txt"

echo "[*] Dispositivos tras el VMD (dominio 0x10000 en Linux)..."
lspci -nn | awk '$1 ~ /^10000:/' > "$OUT/lspci-dominio-vmd.txt" 2>&1
cat "$OUT/lspci-dominio-vmd.txt"

echo "[*] NVMe visibles..."
nvme list > "$OUT/nvme-list.txt" 2>&1 || lsblk -d -o NAME,MODEL,SIZE > "$OUT/lsblk.txt" 2>&1

echo "[*] dmesg VMD..."
dmesg | grep -i vmd > "$OUT/dmesg-vmd.txt" 2>&1

echo "[*] Recursos sysfs..."
ls "/sys/bus/pci/devices/0000:$BDF/resource" > "$OUT/sysfs-resource.txt" 2>&1
cat "/sys/bus/pci/devices/0000:$BDF/resource" > "$OUT/sysfs-resource.txt" 2>&1 || true

echo "[OK] Todo en ./$OUT — pásame ese directorio."
