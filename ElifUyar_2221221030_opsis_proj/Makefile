CC = gcc
CFLAGS = -Wall -Wextra -std=c99
LDFLAGS = -lrt -pthread

TARGET = procx
SOURCE = procx.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)