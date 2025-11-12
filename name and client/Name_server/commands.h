#ifndef COMMANDS_H
#define COMMANDS_H

#include "nm_metadata.h"

// Process a command from a client
// Returns response string to send back to client
void handle_command(int client_sock, const char *username, const char *command);

// Individual command handlers - NM responsibilities only
// (VIEW, INFO, LIST users, Access control)
void cmd_view(int client_sock, const char *username, int show_all, int show_details);
void cmd_list(int client_sock, const char *username);
void cmd_info(int client_sock, const char *username, const char *filename);
void cmd_add_access(int client_sock, const char *username, const char *filename, 
                    const char *target_user, int is_write);
void cmd_remove_access(int client_sock, const char *username, const char *filename, 
                       const char *target_user);

// Note: CREATE, DELETE, UNDO are forwarded to SS
// Note: READ, WRITE, STREAM send "CONNECT_SS <ip> <port>" to client
// Note: EXEC requests file from SS, executes on NM, sends output to client

// Helper to send response to client
void send_response(int sock, const char *message);
void send_error(int sock, ErrorCode err);

#endif
