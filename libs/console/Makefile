CC     = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC -shared
SEA_SRC = ../../src

.PHONY: all clean

all: console.so

console.so: console.c
	$(CC) $(CFLAGS) -I$(SEA_SRC) -o $@ $< -lm -lpthread

clean:
	rm -f console.so
