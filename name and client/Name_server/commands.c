#include "commands.h"
#include <ctype.h>

// this is the function that sends response to the client
void send_response(int sock, const char *message) 
{
    send(sock, message, strlen(message), 0);
}

// this will send error message to the client
void send_error(int sock, ErrorCode err) 
{
    send_response(sock, get_error_message(err));
}

// parser for the commands
void handle_command(int client_sock, const char *username, const char *command) 
{
    // copy so that we dont lose the original command
    char cmd_copy[MAX_COMMAND];
    strncpy(cmd_copy, command, MAX_COMMAND - 1);
    cmd_copy[MAX_COMMAND - 1] = '\0';
    
    // Get first word (the command)
    char *token = strtok(cmd_copy, " \t\n");
    if (!token) 
    {
        send_error(client_sock, ERR_INVALID_COMMAND);
        return;
    }
    
    // this will log the request
    log_request(username, token, command);
    
// this is the command that is executed along with the storage server
    if (strcasecmp(token, "CREATE") == 0) 
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename) 
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        // // ///////// implement this send this to the server so that the server would create the file and remove the error below
        send_response(client_sock, "ERROR: need to merge the storage\n");
    }
    else if (strcasecmp(token, "VIEW") == 0) 
    {
        int show_all = 0;
        int show_details = 0;
        
        // check whether -a or -l or -la or -al is there or not
        char *flag = strtok(NULL, " \t\n");
        if (flag && flag[0] == '-') 
        {
            for (int i = 1; flag[i] != '\0'; i++) 
            {
                if (flag[i] == 'a') 
                {
                    show_all = 1;
                }
                if (flag[i] == 'l') 
                {
                    show_details = 1;
                }
            }
        }
        
        cmd_view(client_sock, username, show_all, show_details);
    }
// this is also execute after merging with the storage server
    else if (strcasecmp(token, "READ") == 0) 
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename) 
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        // Check file exists and access
        FileMetadata *file = find_file(&nm_state.file_map, filename);
        if (!file) 
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }
        AccessLevel access = check_access(filename, username);
        if (access == ACCESS_NONE) 
        {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }
        // here we need to forward the read request to the storage server
        send_response(client_sock, "ERROR: Storage Server not implemented yet\n");
    }
    
    else if (strcasecmp(token, "LIST") == 0) 
    {
        cmd_list(client_sock, username);
    }
   
    else if (strcasecmp(token, "INFO") == 0) 
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename) 
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        cmd_info(client_sock, username, filename);
    }
    
// this should also be handled by the storage server after merging
    else if (strcasecmp(token, "DELETE") == 0) 
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename) 
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        // Check if this is the owner or not
        FileMetadata *file = find_file(&nm_state.file_map, filename);
        if (!file) 
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }
        if (strcmp(file->owner, username) != 0) 
        {
            send_error(client_sock, ERR_NOT_FILE_OWNER);
            return;
        }
        // forward to storage server to delete when we get the acknowlegement from the storage server we will delete the metadata from the name server
        send_response(client_sock, "ERROR: Storage Server not implemented yet\n");
    }
    
    else if (strcasecmp(token, "ADDACCESS") == 0) 
    {
        char *flag = strtok(NULL, " \t\n");
        char *filename = strtok(NULL, " \t\n");
        char *target_user = strtok(NULL, " \t\n");
        
        if (!flag || !filename || !target_user) 
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        
        int is_write = 0;
        if (strcmp(flag, "-W") == 0) 
        {
            is_write = 1;
        } 
        else if (strcmp(flag, "-R") != 0) 
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        
        cmd_add_access(client_sock, username, filename, target_user, is_write);
    }
    
    else if (strcasecmp(token, "REMACCESS") == 0) 
    {
        char *filename = strtok(NULL, " \t\n");
        char *target_user = strtok(NULL, " \t\n");
        
        if (!filename || !target_user) 
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        
        cmd_remove_access(client_sock, username, filename, target_user);
    }
    
// this will also be handled by the storage server after merging
    else if (strcasecmp(token, "WRITE") == 0) 
    {
        char *filename = strtok(NULL, " \t\n");
        char *sentence_str = strtok(NULL, " \t\n");
        
        if (!filename || !sentence_str) 
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        
        // Check file exists and write access
        FileMetadata *file = find_file(&nm_state.file_map, filename);
        if (!file) 
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }
        AccessLevel access = check_access(filename, username);
        if (access != ACCESS_OWNER && access != ACCESS_WRITE) 
        {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }
        // connect to the storage server to handle the write request
        send_response(client_sock, "ERROR: Storage Server not implemented yet\n");
    }
    
// this will also be handled by the storage server after merging
    else if (strcasecmp(token, "STREAM") == 0) 
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename) 
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        // Check file exists and read access
        FileMetadata *file = find_file(&nm_state.file_map, filename);
        if (!file) 
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }
        AccessLevel access = check_access(filename, username);
        if (access == ACCESS_NONE) 
        {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }
        // this will forward the stream request to the storage server
        send_response(client_sock, "ERROR: Storage Server not implemented yet\n");
    }
    
// here the name server will execute the file and send the output to the client
    else if (strcasecmp(token, "EXEC") == 0) 
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename) {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        // Check file exists and read access
        FileMetadata *file = find_file(&nm_state.file_map, filename);
        if (!file) 
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }
        AccessLevel access = check_access(filename, username);
        if (access == ACCESS_NONE) {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }
        // this will forward the exec request to the storage server
        send_response(client_sock, "ERROR: Storage Server not implemented yet\n");
    }
    
// here also the storage server will handle the undo request after merging
    else if (strcasecmp(token, "UNDO") == 0) 
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename) 
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        // Check file exists and write access
        FileMetadata *file = find_file(&nm_state.file_map, filename);
        if (!file) {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }
        AccessLevel access = check_access(filename, username);
        if (access != ACCESS_OWNER && access != ACCESS_WRITE) {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }
        // the undo history is maintained by the storage server so forward the request to the storage server
        send_response(client_sock, "ERROR: Storage Server not implemented yet\n");
    }
    
    else 
    {
        send_error(client_sock, ERR_INVALID_COMMAND);
    }
}



void cmd_view(int client_sock, const char *username, int show_all, int show_details) 
{
    FileMetadata **files;
    int count = get_all_files(&nm_state.file_map, &files);
    
    if (count == 0) 
    {
        send_response(client_sock, "No files in system.\n");
        return;
    }
    
    char response[BUFFER_SIZE * 2];
    response[0] = '\0';
    
    // Show detailed listing with table header
    if (show_details) 
    {
        strcat(response, "---------------------------------------------------------\n");
        strcat(response, "|  Filename  | Words | Chars | Last Access Time | Owner |\n");
        strcat(response, "|------------|-------|-------|------------------|-------|\n");
    }
    
    // Go through all files
    for (int i = 0; i < count; i++) 
    {
        FileMetadata *file = files[i];
        
        // Check access
        AccessLevel access = check_access(file->filename, username);
        
        // If not showing all files, skip files user can't access
        if (!show_all && access == ACCESS_NONE) 
        {
            continue;
        }
    
        if (show_details) 
        {
            char time_str[64];
            format_time(file->last_accessed, time_str, sizeof(time_str));
            
            char line[512];
            snprintf(line, sizeof(line), "| %-10s | %5d | %5d | %16s | %-5s |\n",
                    file->filename, file->word_count, file->char_count, 
                    time_str, file->owner);
            strcat(response, line);
        } 
        else 
        {
            char line[300];
            snprintf(line, sizeof(line), "--> %s\n", file->filename);
            strcat(response, line);
        }
    }
    
    if (show_details) 
    {
        strcat(response, "---------------------------------------------------------\n");
    }
    
    send_response(client_sock, response);
    free(files);
}


void cmd_list(int client_sock, const char *username) 
{
    char users[MAX_USERS][MAX_USERNAME];
    int count = get_all_users(users, MAX_USERS);
    
    if (count == 0) 
    {
        send_response(client_sock, "No users in system.\n");
        return;
    }
    
    char response[BUFFER_SIZE];
    strcpy(response, "Users in system:\n");
    
    for (int i = 0; i < count; i++) 
    {
        char line[128];
        snprintf(line, sizeof(line), "--> %s\n", users[i]);
        strcat(response, line);
    }
    
    send_response(client_sock, response);
}

void cmd_info(int client_sock, const char *username, const char *filename) 
{
    
    FileMetadata *file = find_file(&nm_state.file_map, filename);
    if (!file) 
    {
        send_error(client_sock, ERR_FILE_NOT_FOUND);
        return;
    }
    char response[BUFFER_SIZE];
    char time_str[64];
    snprintf(response, sizeof(response), "--> File: %s\n", file->filename);
    
    char line[256];
    snprintf(line, sizeof(line), "--> Owner: %s\n", file->owner);
    strcat(response, line);
    
    format_time(file->created_time, time_str, sizeof(time_str));
    snprintf(line, sizeof(line), "--> Created: %s\n", time_str);
    strcat(response, line);
    
    format_time(file->last_modified, time_str, sizeof(time_str));
    snprintf(line, sizeof(line), "--> Last Modified: %s\n", time_str);
    strcat(response, line);
    
    snprintf(line, sizeof(line), "--> Size: %zu bytes\n", file->file_size);
    strcat(response, line);
    
    strcat(response, "--> Access: ");
    snprintf(line, sizeof(line), "%s (RW)", file->owner);
    strcat(response, line);
    
    for (int i = 0; i < file->writer_count; i++) 
    {
        snprintf(line, sizeof(line), ", %s (RW)", file->writers[i]);
        strcat(response, line);
    }
    
    for (int i = 0; i < file->reader_count; i++) 
    {
        snprintf(line, sizeof(line), ", %s (R)", file->readers[i]);
        strcat(response, line);
    }
    strcat(response, "\n");
    
    format_time(file->last_accessed, time_str, sizeof(time_str));
    snprintf(line, sizeof(line), "--> Last Accessed: %s by %s\n", 
            time_str, file->last_accessed_by);
    strcat(response, line);
    
    send_response(client_sock, response);
}


void cmd_add_access(int client_sock, const char *username, const char *filename, 
                    const char *target_user, int is_write) 
{
    FileMetadata *file = find_file(&nm_state.file_map, filename);
    if (!file) 
    {
        send_error(client_sock, ERR_FILE_NOT_FOUND);
        return;
    }
   
    if (strcmp(file->owner, username) != 0) 
    {
        send_error(client_sock, ERR_NOT_FILE_OWNER);
        return;
    }
    
    if (!user_exists(target_user)) 
    {
        send_error(client_sock, ERR_USER_NOT_FOUND);
        return;
    }
    
    ErrorCode result;
    if (is_write) 
    {
        result = add_write_access(filename, target_user);
    } 
    else 
    {
        result = add_read_access(filename, target_user);
    }
    
    if (result != ERR_SUCCESS) 
    {
        send_error(client_sock, result);
        return;
    }
    
    send_response(client_sock, "Access granted\n");
    log_response(username, ERR_SUCCESS, filename);
}

void cmd_remove_access(int client_sock, const char *username, const char *filename, 
                       const char *target_user) 
{
    FileMetadata *file = find_file(&nm_state.file_map, filename);
    if (!file) 
    {
        send_error(client_sock, ERR_FILE_NOT_FOUND);
        return;
    }
    
    // Only owner can change access
    if (strcmp(file->owner, username) != 0) 
    {
        send_error(client_sock, ERR_NOT_FILE_OWNER);
        return;
    }
    
    ErrorCode result = remove_access(filename, target_user);
    if (result != ERR_SUCCESS) 
    {
        send_error(client_sock, result);
        return;
    }
    
    send_response(client_sock, "Access removed successfully!\n");
    log_response(username, ERR_SUCCESS, filename);
}




