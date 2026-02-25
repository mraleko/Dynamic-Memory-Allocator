CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g

.PHONY: all clean benchmark

all: benchmark

benchmark:
	./benchmark.sh

clean:
	rm -f benchmark_umalloc benchmark_libc benchmark_umalloc.out benchmark_libc.out
	rm -f demo demo.o umalloc.o
	rm -rf demo_asan.dSYM
