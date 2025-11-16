#ifndef COMMANDS_H
#define COMMANDS_H

#include "nm_metadata.h"

void handle_command(int client_sock, const char *username, const char *command);
void cmd_view(int client_sock, const char *username, int show_all, int show_details);
void cmd_list(int client_sock, const char *username);
void cmd_info(int client_sock, const char *username, const char *filename);
void cmd_add_access(int client_sock, const char *username, const char *filename,const char *target_user, int is_write);
void cmd_remove_access(int client_sock, const char *username, const char *filename,const char *target_user);
void send_response(int sock, const char *message);
void send_error(int sock, ErrorCode err);

#endif
