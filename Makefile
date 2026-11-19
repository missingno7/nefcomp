# Build nefcomp with g++ / clang++ (C++17):  make
#   Windows: MSYS2 MinGW-w64 (mingw32-make), static nefcomp.exe
#   Linux:   static nefcomp (needs static libjpeg and zlib)
#   macOS:   nefcomp, third-party libraries linked statically
# JXL      = prefix of a static libjxl build (include/, lib/); see third_party/build_libjxl.sh
# JPEG_LIB = how to link libjpeg (default -ljpeg; e.g. /path/libjpeg.a), JPEG_CFLAGS = its -I
# VERSION  = version string (default: git describe)
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
HE_PUB := third_party/nikon-he-public
JXL ?= third_party/libjxl
JPEG_LIB ?= -ljpeg
JPEG_CFLAGS ?=
ZLIB ?= -lz
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

ifeq ($(OS),Windows_NT)
  EXE := nefcomp.exe
  LDFLAGS ?= -static
  SYS_LIBS := -lshell32
else
  EXE := nefcomp
  UNAME := $(shell uname -s)
  ifeq ($(UNAME),Darwin)
    LDFLAGS ?=
    SYS_LIBS :=
  else
    LDFLAGS ?= -static -pthread
    SYS_LIBS :=
  endif
endif

JXL_LIBS := -L$(JXL)/lib -ljxl -ljxl_cms -lhwy -lbrotlienc -lbrotlidec -lbrotlicommon

SRCS := src/main.cpp src/tiff.cpp src/nikon_lossless.cpp src/he_encoder.cpp src/nef_convert.cpp src/he_decode.cpp src/nef_rebuild.cpp src/dng_jxl.cpp src/hash.cpp src/jxl_util.cpp src/nef_embed.cpp src/preview.cpp src/platform.cpp
PUB_SRCS := $(wildcard $(HE_PUB)/*.cpp)

all: $(EXE)

$(EXE): $(SRCS) $(PUB_SRCS) $(wildcard src/*.h) $(wildcard $(HE_PUB)/*.h)
	$(CXX) $(CXXFLAGS) -DNEFCOMP_VERSION=\"$(VERSION)\" -DJXL_STATIC_DEFINE -DJXL_CMS_STATIC_DEFINE -I$(HE_PUB) -I$(JXL)/include $(JPEG_CFLAGS) $(SRCS) $(PUB_SRCS) -o $@ $(JXL_LIBS) $(JPEG_LIB) $(ZLIB) $(SYS_LIBS) $(LDFLAGS)

tools: wicdecode.exe
wicdecode.exe: tools/wicdecode.cpp
	$(CXX) -O2 $< -o $@ -lole32 -lwindowscodecs -luuid -static

clean:
	rm -f nefcomp nefcomp.exe wicdecode.exe

.PHONY: all clean tools
