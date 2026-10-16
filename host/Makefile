# PC build of the emulator core, for testing. Not used by PlatformIO.
#   make            builds ./nestool
#   make test       runs nestest against its golden log

CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c++17 -DNES_TRACE

SRC = main.cpp $(wildcard ../src/nes/*.cpp) $(wildcard ../src/nes/opl/*.cpp)
HDR = $(wildcard ../src/nes/*.h) $(wildcard ../src/nes/opl/*.h)

nestool: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

test: nestool
	./nestool nestest ../testroms/nestest/nestest.nes ../testroms/nestest/nestest.log

clean:
	rm -f nestool

.PHONY: test clean
