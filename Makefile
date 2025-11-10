# Compiler
CC = gcc

# Compiler flags
# -g: Adds debugging information
# -Wall: Turns on all common warnings
# -pthread: Links the POSIX threads library (CRITICAL!)
CFLAGS = -g -Wall -pthread

# Executable name
TARGET = storage_server

# Source file
SRC = storageserver.c

# Default rule: builds the target
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

# Rule to clean up build files
clean:
	rm -f $(TARGET)
	rm -f *.o

# Rule to run the server
run: $(TARGET)
	./$(TARGET)