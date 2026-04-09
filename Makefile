CXX = g++
CXXFLAGS = -O2 -std=c++17 -Wall $(shell pkg-config --cflags sdl2 SDL2_ttf)
LDFLAGS = $(shell pkg-config --libs sdl2 SDL2_ttf) -lm

TARGET = bme688-monitor
SRCS = main.cpp bme688.cpp

# Optional BSEC2 support:
# 1. Download from Bosch (requires license registration)
# 2. Place libalgobsec.a in bsec2/lib/
# 3. Place headers in bsec2/inc/
# 4. Run: make BSEC2=1
ifdef BSEC2
CXXFLAGS += -DUSE_BSEC2 -Ibsec2/inc
LDFLAGS += -Lbsec2/lib -lalgobsec
endif

all: $(TARGET)

$(TARGET): $(SRCS) bme688.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
