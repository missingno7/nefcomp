# Build with MSYS2 MinGW-w64 g++ (or any C++17 compiler):  make
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDFLAGS ?= -static
HE_PUB := third_party/nikon-he-public
JXL := third_party/libjxl
JXL_LIBS := -L$(JXL)/lib -ljxl -ljxl_cms -lhwy -lbrotlienc -lbrotlidec -lbrotlicommon -ljpeg -lz

SRCS := src/main.cpp src/tiff.cpp src/nikon_lossless.cpp src/he_encoder.cpp src/nef_convert.cpp src/he_decode.cpp src/nef_rebuild.cpp src/dng_jxl.cpp src/hash.cpp src/jxl_util.cpp src/nef_embed.cpp src/preview.cpp
PUB_SRCS := $(wildcard $(HE_PUB)/*.cpp)

all: nefcomp.exe

nefcomp.exe: $(SRCS) $(PUB_SRCS) $(wildcard src/*.h) $(wildcard $(HE_PUB)/*.h)
	$(CXX) $(CXXFLAGS) -DJXL_STATIC_DEFINE -DJXL_CMS_STATIC_DEFINE -I$(HE_PUB) -I$(JXL)/include $(SRCS) $(PUB_SRCS) -o $@ $(JXL_LIBS) $(LDFLAGS)

tools: wicdecode.exe
wicdecode.exe: tools/wicdecode.cpp
	$(CXX) -O2 $< -o $@ -lole32 -lwindowscodecs -luuid -static

clean:
	rm -f nefcomp.exe wicdecode.exe

.PHONY: all clean tools
