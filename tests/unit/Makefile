# Host unit tests: make -C tests/unit && ./tests/unit/build/unit_tests
CC ?= cc
B := build
$(B)/unit_tests: test_main.c ../../lib/dimmer.c ../../lib/meter.c ../../lib/dimmer.h ../../lib/meter.h
	@mkdir -p $(B)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -Wdouble-promotion -I../../lib test_main.c ../../lib/dimmer.c ../../lib/meter.c -lm -o $@
clean:
	rm -rf $(B)
