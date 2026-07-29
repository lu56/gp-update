CXX = g++
RC = windres
CXXFLAGS = -std=c++17 -municode -O2 -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-format
LDFLAGS = -municode -mwindows -static -static-libgcc -static-libstdc++
LIBS = -lws2_32 -lwinhttp -liphlpapi -lbcrypt -lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32

SRC_DIR = src
SRC = $(wildcard $(SRC_DIR)/*.cpp)
OBJ = $(patsubst $(SRC_DIR)/%.cpp, build/%.o, $(SRC))
RES = build/resource.o
TARGET = GatewayPolicy.exe

all: $(TARGET)

$(TARGET): $(OBJ) $(RES)
	$(CXX) $(LDFLAGS) -o $@ $(OBJ) $(RES) $(LIBS)
	@echo "Build complete: $(TARGET)"
	@echo "Size: $$(stat -c%s $(TARGET) 2>/dev/null || wc -c < $(TARGET)) bytes"

build/%.o: $(SRC_DIR)/%.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(RES): resource.rc app.manifest | build
	$(RC) resource.rc -O coff -o $@

build:
	@if not exist build mkdir build

clean:
	@if exist build rmdir /s /q build
	@if exist $(TARGET) del /q $(TARGET)

.PHONY: all clean
