CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -pthread

NM_DIR = Name_server
CLIENT_DIR = Client_user

NM_SOURCES = $(NM_DIR)/listen.c $(NM_DIR)/nm_metadata.c $(NM_DIR)/commands.c
NM_OBJECTS = $(NM_SOURCES:.c=.o)
NM_TARGET = name_server

CLIENT_SOURCES = $(CLIENT_DIR)/client.c
CLIENT_OBJECTS = $(CLIENT_SOURCES:.c=.o)
CLIENT_TARGET = client_user

all: $(NM_TARGET) $(CLIENT_TARGET)

$(NM_TARGET): $(NM_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^


$(CLIENT_TARGET): $(CLIENT_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^


%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(NM_OBJECTS) $(CLIENT_OBJECTS)
	rm -f $(NM_TARGET) $(CLIENT_TARGET) 2>/dev/null || true
	rm -f *.log
	rm -rf nm_files

run-nm: $(NM_TARGET)
	./$(NM_TARGET)

run-client: $(CLIENT_TARGET)
	./$(CLIENT_TARGET)

.PHONY: all clean run-nm run-client
