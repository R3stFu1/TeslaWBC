CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1

run: test_control test_login_guard test_recovery
	./test_control
	./test_login_guard
	./test_recovery

test_control: test_control.cpp ../teslawbc/control.h
	$(CXX) $(CXXFLAGS) -o $@ test_control.cpp

test_login_guard: test_login_guard.cpp ../components/teslawbc_web/login_guard.h
	$(CXX) $(CXXFLAGS) -o $@ test_login_guard.cpp

test_recovery: test_recovery.cpp ../components/teslawbc_web/recovery.h ../components/teslawbc_web/blackbox.h
	$(CXX) $(CXXFLAGS) -o $@ test_recovery.cpp

clean:
	rm -f test_control test_login_guard test_recovery

.PHONY: run clean
