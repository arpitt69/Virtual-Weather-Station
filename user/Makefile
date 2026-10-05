# User-space build: vwsd (the station) and vwsctl (configuration / faults).
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -pthread
LDLIBS   := -lncursesw -pthread

COMMON   := VwsDevice.o Filters.o SensorHealth.o DerivedMetrics.o FusionEngine.o
VWSD_OBJ := $(COMMON) SensorReader.o CsvLogger.o Dashboard.o main.o
CTL_OBJ  := $(COMMON) Cli.o
TEST_OBJ := $(COMMON) tests.o

BINS := vwsd vwsctl vwstest

all: $(BINS)

vwsd: $(VWSD_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

vwsctl: $(CTL_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ -pthread

vwstest: $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ -pthread

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(BINS) *.o

-include $(wildcard *.d)
CXXFLAGS += -MMD -MP

.PHONY: all clean
