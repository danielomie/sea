CC     = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC -shared
SEA_SRC = ../../src

.PHONY: all clean

all: xml.so

xml.so: xml.c $(wildcard ../../src/*.h)
	$(CC) $(CFLAGS) -I$(SEA_SRC) -o $@ $<

clean:
	rm -f xml.so
