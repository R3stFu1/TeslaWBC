CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1

run: test_control
	./test_control

test_control: test_control.cpp ../teslawbc/control.h
	$(CXX) $(CXXFLAGS) -o $@ test_control.cpp

clean:
	rm -f test_control

.PHONY: run clean
