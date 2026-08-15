CC ?= gcc
CFLAGS ?= -O3 -march=native -Wall -Wextra -Werror

.PHONY: all test bench objects clean

all: libbraid58.a

objects: encode_r6.o decode_r6.o

libbraid58.a: encode_r6.o decode_r6.o
	$(AR) rcs $@ $^

encode_r6.o: src/encode_r6.c include/braid58.h
	$(CC) $(CFLAGS) -DBRAID58_NO_MAIN -c $< -o $@

decode_r6.o: src/decode_r6.c include/braid58.h
	$(CC) $(CFLAGS) -c $< -o $@

encode_r6_test: src/encode_r6.c
	$(CC) $(CFLAGS) $< -o $@

decode_r6_test: src/decode_r6.c
	$(CC) $(CFLAGS) -DBRAID58_TEST $< -o $@

test: encode_r6_test decode_r6_test
	./encode_r6_test
	./decode_r6_test

bench:
	./bench/run.sh

clean:
	rm -f libbraid58.a encode_r6.o decode_r6.o encode_r6_test decode_r6_test
	rm -rf build
