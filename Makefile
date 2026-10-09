# Build del kext sin Xcode completo (solo Command Line Tools).
# - check: verificacion de sintaxis contra Kernel.framework
# - all: compila + linka + empaqueta IntelVMD.kext + firma ad-hoc
SDK ?= /Library/Developer/CommandLineTools/SDKs/MacOSX13.sdk
TARGET = x86_64-apple-macosx
CXX = clang++
KERN_INCL = $(SDK)/System/Library/Frameworks/Kernel.framework/Headers
CXXFLAGS = -x c++ -std=c++11 -target $(TARGET) -mkernel \
	-fno-exceptions -fno-rtti -fno-builtin \
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
	-Wno-inconsistent-missing-override -Wno-deprecated-declarations \
	-I$(KERN_INCL)
LDFLAGS = -target $(TARGET) -mkernel -nostdlib -static -Wl,-kext \
	-L$(SDK)/usr/lib -lkmodc++ -lkmod

check:
	$(CXX) -fsyntax-only $(CXXFLAGS) IntelVMD/IntelVMD.cpp && echo "SINTAXIS OK"

IntelVMD.o: IntelVMD/IntelVMD.cpp IntelVMD/IntelVMD.h
	$(CXX) $(CXXFLAGS) -c IntelVMD/IntelVMD.cpp -o IntelVMD.o

IntelVMD.bin: IntelVMD.o
	$(CXX) $(LDFLAGS) -o IntelVMD.bin IntelVMD.o

all: IntelVMD.bin
	rm -rf IntelVMD.kext
	mkdir -p IntelVMD.kext/Contents/MacOS
	cp IntelVMD/Info.plist IntelVMD.kext/Contents/Info.plist
	cp IntelVMD.bin IntelVMD.kext/Contents/MacOS/IntelVMD
	codesign -s - --force IntelVMD.kext
	@echo "KEXT LISTO: IntelVMD.kext"

clean:
	rm -rf IntelVMD.o IntelVMD.bin IntelVMD.kext

.PHONY: check all clean
