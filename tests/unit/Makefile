# Unit tests for the predictor code. Needs only g++, not Pin.
# The flags mirror the ones Pin uses for tools (C++17, no exceptions, no RTTI),
# plus -Werror so any warning is caught here first.
CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Werror -fno-exceptions -fno-rtti -I../../pintool
HEADERS  = $(wildcard ../../pintool/*.h)

all: test_predictor replay

test_predictor: test_predictor.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $< -o $@

replay: replay.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $< -o $@

run: test_predictor
	./test_predictor

clean:
	rm -f test_predictor replay

.PHONY: all run clean
