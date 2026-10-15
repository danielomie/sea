CC     = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC -shared
SEA_SRC = ../../src

.PHONY: all clean

all: http_native.so

http_native.so: http_native.c
	$(CC) $(CFLAGS) -I$(SEA_SRC) -o $@ $<

clean:
	rm -f http_native.so
