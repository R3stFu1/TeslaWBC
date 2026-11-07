CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1

run: test_control test_login_guard
	./test_control
	./test_login_guard

test_control: test_control.cpp ../teslawbc/control.h
	$(CXX) $(CXXFLAGS) -o $@ test_control.cpp

test_login_guard: test_login_guard.cpp ../components/teslawbc_web/login_guard.h
	$(CXX) $(CXXFLAGS) -o $@ test_login_guard.cpp

clean:
	rm -f test_control test_login_guard

.PHONY: run clean
