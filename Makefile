# Build del kext sin Xcode completo (solo Command Line Tools).
# - check: verificacion de sintaxis contra Kernel.framework
# - all: compila + linka + empaqueta IntelVMD.kext + firma ad-hoc
#
# El nucleo VMD (matematica ECAM, decodificacion VMCAP/VMCONFIG, reglas de
# MSI-remap) vive en VMDCore/ y es la MISMA fuente que usa el driver UEFI
# (IntelVMDUefi/). No duplicar logica: si cambia el nucleo, cambia en un sitio.
SDK ?= /Library/Developer/CommandLineTools/SDKs/MacOSX13.sdk
TARGET = x86_64-apple-macosx
CXX = clang++
KERN_INCL = $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
CXXFLAGS = -x c++ -std=c++11 -target $(TARGET) -mkernel \
	-isysroot $(SDK) \
	-fno-exceptions -fno-rtti -fno-builtin \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-Wno-inconsistent-missing-override -Wno-deprecated-declarations \
	-I$(KERN_INCL) -IIntelVMD -IVMDCore
LDFLAGS = -target $(TARGET) -mkernel -nostdlib -static -Wl,-kext \
	-isysroot $(SDK) \
	-L$(SDK)/usr/lib -lkmodc++ -lkmod

SRCS = IntelVMD/IntelVMD.cpp VMDCore/VMDLogic.cpp
# Los .o van junto a sus .cpp (el include de VMDLogic.hpp es relativo al
# directorio de su .cpp, por eso VMDCore/VMDLogic.o vive en VMDCore/).
OBJS = IntelVMD.o VMDCore/VMDLogic.o

check:
	$(CXX) -fsyntax-only $(CXXFLAGS) IntelVMD/IntelVMD.cpp && echo "SINTAXIS OK"

IntelVMD.o: IntelVMD/IntelVMD.cpp IntelVMD/IntelVMD.h VMDCore/VMDLogic.hpp
	$(CXX) $(CXXFLAGS) -c IntelVMD/IntelVMD.cpp -o IntelVMD.o

VMDCore/VMDLogic.o: VMDCore/VMDLogic.cpp VMDCore/VMDLogic.hpp
	$(CXX) $(CXXFLAGS) -c VMDCore/VMDLogic.cpp -o VMDCore/VMDLogic.o

IntelVMD.bin: $(OBJS)
	$(CXX) $(LDFLAGS) -o IntelVMD.bin $(OBJS)

all: IntelVMD.bin
	rm -rf IntelVMD.kext
	mkdir -p IntelVMD.kext/Contents/MacOS
	cp IntelVMD/Info.plist IntelVMD.kext/Contents/Info.plist
	cp IntelVMD.bin IntelVMD.kext/Contents/MacOS/IntelVMD
	codesign -s - --force IntelVMD.kext
	@echo "KEXT LISTO: IntelVMD.kext"

clean:
	rm -rf IntelVMD.o VMDCore/VMDLogic.o IntelVMD.bin IntelVMD.kext build.log

.PHONY: check all clean
