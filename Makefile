CXX      = g++
X11_PREFIX ?= $(if $(filter Darwin,$(shell uname -s)),/opt/X11,/usr)
CXXFLAGS = -Wall -Wextra -g -std=c++17 -pthread -I$(X11_PREFIX)/include -Isrc
LDFLAGS  = -L$(X11_PREFIX)/lib -Wl,-rpath,$(X11_PREFIX)/lib -lX11 -pthread
SRC      = $(shell find src -type f -name '*.cpp' | sort)
OBJ      = $(SRC:.cpp=.o)

terminator: $(OBJ)
	$(CXX) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	find src -type f -name '*.o' -delete
	rm -f terminator
.PHONY: clean
