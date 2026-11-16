# Directed tests for validating the Pin tool's event log.
# -O0 keeps the code a direct translation of the source, so the expected
# events can be read off the source and checked against objdump.
# -no-pie gives fixed addresses, so the log matches `objdump -d` exactly.
CC     = gcc
CFLAGS = -O0 -g -no-pie
TESTS  = t_cond t_calls t_indirect t_patterns

all: $(TESTS)

%: %.c
	$(CC) $(CFLAGS) $< -o $@

# The bonus's pattern test is compiled like the workloads (-O2).
t_patterns: t_patterns.c ../workloads/rng.h
	$(CC) -O2 -g -no-pie $< -o $@

clean:
	rm -f $(TESTS)

.PHONY: all clean
