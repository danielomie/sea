CC     = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
         -fno-strict-aliasing -D_GNU_SOURCE -fPIC -shared
SEA_SRC = ../../src

SRCS = parser.c \
       $(SEA_SRC)/sea_lexer.c \
       $(SEA_SRC)/sea_parser.c \
       $(SEA_SRC)/sea_ast.c

.PHONY: all clean

all: parser.so

parser.so: $(SRCS)
	$(CC) $(CFLAGS) -I$(SEA_SRC) -o $@ $^ -lm

clean:
	rm -f parser.so
