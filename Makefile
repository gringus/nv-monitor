CC      ?= cc
VERSION  = $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
CFLAGS   = -O3 -march=native -flto -Wall -Wextra -std=gnu11 -DVERSION='"$(VERSION)"'
CFLAGS_PORTABLE = -O3 -flto -Wall -Wextra -std=gnu11 -DVERSION='"$(VERSION)"'
LDFLAGS  = -ldl -lpthread
PREFIX   ?= /usr/local
TARGET   = nv-monitor

all: $(TARGET) demo-load

$(TARGET): nv-monitor.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

demo-load: demo-load.c
	$(CC) -O2 -Wall -Wextra -o demo-load demo-load.c -lpthread -ldl -lm

portable:
	$(CC) $(CFLAGS_PORTABLE) -o $(TARGET) nv-monitor.c $(LDFLAGS)

test: test_meminfo.c test_thermal.c
	$(CC) -O0 -Wall -Wextra -o test_meminfo test_meminfo.c
	./test_meminfo
	# -Wno-format-truncation: pre-existing snprintfs in nv-monitor.c trip the
	# -O0-only analyzer; the release build (-O3 -flto) is warning-clean
	$(CC) -O0 -Wall -Wextra -Wno-format-truncation -o test_thermal test_thermal.c $(LDFLAGS)
	./test_thermal

lint:
	@echo "Running cppcheck..."
	cppcheck --enable=all --suppress=missingIncludeSystem nv-monitor.c
	@echo "Running clang-tidy..."
	clang-tidy-20 nv-monitor.c -- --std=gnu11 -ldl -lpthread -Wall -Wextra

clean:
	rm -f $(TARGET) demo-load test_meminfo test_thermal

install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 755 $(TARGET) $(PREFIX)/bin/

install-user: $(TARGET)
	install -d $(HOME)/.local/bin
	install -m 755 $(TARGET) $(HOME)/.local/bin/

.PHONY: all portable test clean install install-user lint
