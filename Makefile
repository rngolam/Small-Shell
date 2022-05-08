GCC=gcc
CFLAGS=-std=gnu99 -Wall -Wextra -Wpedantic -Werror

smallsh: smallsh.c
	$(GCC) -O3 $(CFLAGS) $^ -o $@