CC ?= gcc
CFLAGS = -Wall -Werror
TARGET = aesdsocket

.PHONY: all default clean

all: default

default: $(TARGET)

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) -o $(TARGET) aesdsocket.c

clean:
	rm -f $(TARGET) *.o
