CC ?= clang
CFLAGS = -O2 -Wall
TARGET = wrapper

all: $(TARGET)

$(TARGET): wrapper.c
	$(CC) $(CFLAGS) -o $@ $< -s

clean:
	rm -f $(TARGET)

.PHONY: all clean
