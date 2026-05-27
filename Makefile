CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread
LIBS     = -lsfml-graphics -lsfml-window -lsfml-audio -lsfml-network -lsfml-system -lrt
TARGETS  = arbiters hips asps

all: clean $(TARGETS)
	@echo "Build complete."

arbiters: arbiter/arbiter.cpp arbiter/renderer.h arbiter/ipc.h
	$(CXX) $(CXXFLAGS) arbiter/arbiter.cpp -o arbiters $(LIBS)

hips: hip/hip.cpp hip/ipc.h
	$(CXX) $(CXXFLAGS) hip/hip.cpp -o hips $(LIBS)

asps: asp/asp.cpp asp/ipc.h
	$(CXX) $(CXXFLAGS) asp/asp.cpp -o asps $(LIBS)

clean:
	rm -f arbiters hips asps

.PHONY: all clean