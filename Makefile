# wtop - htop for Windows, pure C, Win32 API only.
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  :=
SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:.c=.o)
BIN     := wtop.exe

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
