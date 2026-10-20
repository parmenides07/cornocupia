CC     = gcc
CFLAGS = -Wall -Wextra -O2
TARGET = tracker
SRC    = tracker.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) -lm

clean:
	rm -f $(TARGET) tracker.json

.PHONY: clean