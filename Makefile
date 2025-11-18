CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -pthread

NM_CLIENT_DIR = name_client
NM_DIR = $(NM_CLIENT_DIR)/Name_server
CLIENT_DIR = $(NM_CLIENT_DIR)/Client_user

NM_SOURCES = $(NM_DIR)/listen.c \
             $(NM_DIR)/nm_metadata.c \
             $(NM_DIR)/commands.c
NM_OBJECTS = $(NM_SOURCES:.c=.o)
NM_TARGET = name_server

CLIENT_SOURCES = $(CLIENT_DIR)/client.c
CLIENT_OBJECTS = $(CLIENT_SOURCES:.c=.o)
CLIENT_TARGET = client_user

SS_SOURCE = storageserver.c
SS_TARGET = storage_server

.PHONY: all clean help run-nm run-client run-ss

all: $(NM_TARGET) $(CLIENT_TARGET) $(SS_TARGET)

$(NM_TARGET): $(NM_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(CLIENT_TARGET): $(CLIENT_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^

$(SS_TARGET): $(SS_SOURCE)
	$(CC) $(CFLAGS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(NM_OBJECTS) $(CLIENT_OBJECTS)
	rm -f $(NM_TARGET) $(CLIENT_TARGET) $(SS_TARGET)
	rm -f *.o *.log
	rm -rf nm_files storage
	rm -rf storage storage_[0-9]*

run-nm: $(NM_TARGET)
	./$(NM_TARGET)

run-client: $(CLIENT_TARGET)
	./$(CLIENT_TARGET)

run-ss: $(SS_TARGET)
	./$(SS_TARGET)
