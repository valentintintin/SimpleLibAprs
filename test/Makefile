CC = gcc
CFLAGS = -Wall -Wextra -pedantic
LDFLAGS = -lm
DFLAGS = -DNATIVE

TARGET = app

SRC = test.cpp

INCLUDES = ../Aprs.cpp

all: clean $(TARGET) run

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $^ $(LDFLAGS) $(DFLAGS) -o $@

run:
	./$(TARGET)

clean:
	rm -f $(TARGET)
