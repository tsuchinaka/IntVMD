# Build del kext sin Xcode completo (solo Command Line Tools).
# - check: verificacion de sintaxis contra Kernel.framework
# - all: compila + linka + empaqueta IntelVMD.kext + firma ad-hoc
SDK ?= /Library/Developer/CommandLineTools/SDKs/MacOSX13.sdk
TARGET = x86_64-apple-macosx
CXX = clang++
KERN_INCL = $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
CXXFLAGS = -x c++ -std=c++11 -target $(TARGET) -mkernel \
	-isysroot $(SDK) \
	-fno-exceptions -fno-rtti -fno-builtin \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-Wno-inconsistent-missing-override -Wno-deprecated-declarations \
	-I$(KERN_INCL) -IIntelVMD
LDFLAGS = -target $(TARGET) -mkernel -nostdlib -static -Wl,-kext \
	-isysroot $(SDK) \
	-L$(SDK)/usr/lib -lkmodc++ -lkmod

SRCS = IntelVMD/IntelVMD.cpp IntelVMD/Logic/VMDLogic.cpp
# Los .o van junto a sus .cpp: el directorio Logic/ esta bajo IntelVMD/, no en
# la raiz (por eso la salida debe ser IntelVMD/Logic/VMDLogic.o).
OBJS = IntelVMD.o IntelVMD/Logic/VMDLogic.o

check:
	$(CXX) -fsyntax-only $(CXXFLAGS) IntelVMD/IntelVMD.cpp && echo "SINTAXIS OK"

IntelVMD.o: IntelVMD/IntelVMD.cpp IntelVMD/IntelVMD.h IntelVMD/Logic/VMDLogic.hpp
	$(CXX) $(CXXFLAGS) -c IntelVMD/IntelVMD.cpp -o IntelVMD.o

IntelVMD/Logic/VMDLogic.o: IntelVMD/Logic/VMDLogic.cpp IntelVMD/Logic/VMDLogic.hpp
	$(CXX) $(CXXFLAGS) -c IntelVMD/Logic/VMDLogic.cpp -o IntelVMD/Logic/VMDLogic.o

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
	rm -rf IntelVMD.o IntelVMD/Logic/VMDLogic.o IntelVMD.bin IntelVMD.kext build.log

.PHONY: check all clean
