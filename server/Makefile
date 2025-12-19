CC ?= gcc
CFLAGS = -Wall -Werror
LDFLAGS = -pthread -lrt
TARGET = aesdsocket

.PHONY: all default clean

all: default

default: $(TARGET)

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) -o $(TARGET) aesdsocket.c $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o
