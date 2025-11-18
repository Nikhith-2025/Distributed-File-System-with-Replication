#include "commands.h"
#include <ctype.h>

// Send a normal response to the client
void send_response(int sock, const char *message)
{
    send(sock, message, strlen(message), 0);
}

// Send an error to the client
void send_error(int sock, ErrorCode err)
{
    send_response(sock, get_error_message(err));
}

static int send_ss_command(StorageServerInfo *ss, const char *cmd, char *response, size_t resp_size)
{
    if (!ss || !ss->active)
    {
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        return -1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ss->nm_port);
    addr.sin_addr.s_addr = inet_addr(ss->ip);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }

    if (send(sock, cmd, strlen(cmd), 0) < 0)
    {
        close(sock);
        return -1;
    }

    ssize_t n = recv(sock, response, resp_size - 1, 0);
    if (n < 0)
    {
        close(sock);
        return -1;
    }

    response[n] = '\0';
    close(sock);
    return 0;
}

// Main command dispatcher
void handle_command(int client_sock, const char *username, const char *command)
{
    char cmd_copy[MAX_COMMAND];
    strncpy(cmd_copy, command, MAX_COMMAND - 1);
    cmd_copy[MAX_COMMAND - 1] = '\0';

    char *token = strtok(cmd_copy, " \t\n");
    if (!token)
    {
        send_error(client_sock, ERR_INVALID_COMMAND);
        return;
    }

    log_request(username, token, command);

    // ================= CREATE =================
    if (strcasecmp(token, "CREATE") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }

        if (find_file(&nm_state.file_map, filename))
        {
            send_error(client_sock, ERR_FILE_ALREADY_EXISTS);
            return;
        }

        int ss_index = find_least_loaded_ss();
        if (ss_index == -1)
        {
            send_error(client_sock, ERR_NO_STORAGE_SERVERS);
            return;
        }
        
        StorageServerInfo *ss = get_storage_server(ss_index);

        if (!ss)
        {
            send_error(client_sock, ERR_NO_STORAGE_SERVERS);
            return;
        }

        char ss_cmd[BUFFER_SIZE];
        snprintf(ss_cmd, sizeof(ss_cmd), "CREATE|%s\n", filename);

        char ack[256];
        if (send_ss_command(ss, ss_cmd, ack, sizeof(ack)) == 0)
        {
            if (strstr(ack, "ACK_CREATE OK"))
            {
                FileMetadata meta = {0};
                strncpy(meta.filename, filename, MAX_FILENAME - 1);
                strncpy(meta.owner, username, MAX_USERNAME - 1);

                meta.ss_index = ss - nm_state.storage_servers;
                meta.created_time = time(NULL);
                meta.last_modified = time(NULL);
                meta.last_accessed = time(NULL);
                strncpy(meta.last_accessed_by, username, MAX_USERNAME - 1);

                insert_file(&nm_state.file_map, &meta);

                increment_ss_file_count(meta.ss_index);

                // track this new file on the chosen storage server so
                // that load-based selection can work correctly.
                pthread_mutex_lock(&nm_state.ss_lock);
                if (meta.ss_index >= 0 && meta.ss_index < nm_state.storage_count)
                {
                    StorageServerInfo *tss = &nm_state.storage_servers[meta.ss_index];
                    if (tss->file_count < MAX_FILES)
                    {
                        strncpy(tss->files[tss->file_count], filename, MAX_FILENAME - 1);
                        tss->files[tss->file_count][MAX_FILENAME - 1] = '\0';
                        tss->file_count++;
                    }
                }
                pthread_mutex_unlock(&nm_state.ss_lock);

                char resp[256];
                snprintf(resp, sizeof(resp), "File '%s' created successfully!\n", filename);
                send_response(client_sock, resp);
                log_response(username, ERR_SUCCESS, filename);
                persist_metadata();
            }

            else
            {
                send_response(client_sock, "ERROR: Storage Server failed to create file\n");
            }
        }
        else
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
        }
    }

    // ================= VIEW =================
    else if (strcasecmp(token, "VIEW") == 0)
    {
        int show_all = 0;
        int show_details = 0;

        char *flag = strtok(NULL, " \t\n");
        if (flag && flag[0] == '-')
        {
            for (int i = 1; flag[i] != '\0'; i++)
            {
                if (flag[i] == 'a') show_all = 1;
                if (flag[i] == 'l') show_details = 1;
            }
        }

        cmd_view(client_sock, username, show_all, show_details);
    }

    // ================= READ =================
    else if (strcasecmp(token, "READ") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }

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

        if (file->ss_index < 0 || !nm_state.storage_servers[file->ss_index].active)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        StorageServerInfo *ss = &nm_state.storage_servers[file->ss_index];

        char resp[BUFFER_SIZE];
        snprintf(resp, sizeof(resp), "CONNECT_SS %s %d %s\n",
                 ss->ip, ss->client_port, filename);

        send_response(client_sock, resp);

        file->last_accessed = time(NULL);
        strncpy(file->last_accessed_by, username, MAX_USERNAME - 1);
        log_response(username, ERR_SUCCESS, filename);
        persist_metadata();
        persist_metadata();
    }

    // ================= LIST =================
    else if (strcasecmp(token, "LIST") == 0)
    {
        cmd_list(client_sock, username);
    }

    // ================= INFO =================
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

    // ================= DELETE =================
    else if (strcasecmp(token, "DELETE") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }

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

        if (file->ss_index < 0 || !nm_state.storage_servers[file->ss_index].active)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        StorageServerInfo *ss = &nm_state.storage_servers[file->ss_index];

        char ss_cmd[BUFFER_SIZE];
        snprintf(ss_cmd, sizeof(ss_cmd), "DELETE|%s\n", filename);

        char ack[256];
        if (send_ss_command(ss, ss_cmd, ack, sizeof(ack)) == 0)
        {
            if (strstr(ack, "ACK_DELETE OK"))
            {
                delete_file(&nm_state.file_map, filename);

                // remove the file from the storage server's local list
                // and decrement its file_count so future CREATEs can be
                // balanced across servers.
                pthread_mutex_lock(&nm_state.ss_lock);
                if (file->ss_index >= 0 && file->ss_index < nm_state.storage_count)
                {
                    StorageServerInfo *tss = &nm_state.storage_servers[file->ss_index];
                    for (int i = 0; i < tss->file_count; i++)
                    {
                        if (strcmp(tss->files[i], filename) == 0)
                        {
                            for (int j = i; j < tss->file_count - 1; j++)
                            {
                                strncpy(tss->files[j], tss->files[j + 1], MAX_FILENAME);
                            }
                            tss->file_count--;
                            break;
                        }
                    }
                }
                pthread_mutex_unlock(&nm_state.ss_lock);

                char resp[256];
                snprintf(resp, sizeof(resp), "File '%s' deleted successfully!\n", filename);
                send_response(client_sock, resp);
                log_response(username, ERR_SUCCESS, filename);
                persist_metadata();
            }
            else
            {
                send_response(client_sock, "ERROR: Storage Server failed to delete file\n");
            }
        }
        else
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
        }
    }

    // ================= ADDACCESS =================
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
        if (strcmp(flag, "-W") == 0) is_write = 1;
        else if (strcmp(flag, "-R") != 0)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }

        cmd_add_access(client_sock, username, filename, target_user, is_write);
    }

    // ================= REMACCESS =================
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

    // ================= WRITE =================
    else if (strcasecmp(token, "WRITE") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char *sentence_str = strtok(NULL, " \t\n");

        if (!filename || !sentence_str)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }

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

        if (file->ss_index < 0 || !nm_state.storage_servers[file->ss_index].active)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        StorageServerInfo *ss = &nm_state.storage_servers[file->ss_index];

        char resp[BUFFER_SIZE];
        snprintf(resp, sizeof(resp), "CONNECT_SS %s %d %s\n",
                 ss->ip, ss->client_port, filename);
        send_response(client_sock, resp);

        // Optimistically mark the file as modified; exact size/word/char
        // counts will be refreshed on the next VIEW -l / INFO.
        file->last_modified = time(NULL);
        file->last_accessed = file->last_modified;
        strncpy(file->last_accessed_by, username, MAX_USERNAME - 1);

        log_response(username, ERR_SUCCESS, filename);
        persist_metadata();
    }

    // ================= STREAM =================
    else if (strcasecmp(token, "STREAM") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }

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

        if (file->ss_index < 0 || !nm_state.storage_servers[file->ss_index].active)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        StorageServerInfo *ss = &nm_state.storage_servers[file->ss_index];

        char resp[BUFFER_SIZE];
        snprintf(resp, sizeof(resp), "CONNECT_SS %s %d %s\n",
                 ss->ip, ss->client_port, filename);
        send_response(client_sock, resp);

        file->last_accessed = time(NULL);
        strncpy(file->last_accessed_by, username, MAX_USERNAME - 1);
        log_response(username, ERR_SUCCESS, filename);
    }

    // ================= EXEC =================
    else if (strcasecmp(token, "EXEC") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }

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

        if (file->ss_index < 0 || !nm_state.storage_servers[file->ss_index].active)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        StorageServerInfo *ss = &nm_state.storage_servers[file->ss_index];

        int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (ss_sock < 0)
        {
            send_error(client_sock, ERR_INTERNAL_ERROR);
            return;
        }

        struct sockaddr_in ss_addr;
        ss_addr.sin_family = AF_INET;
        ss_addr.sin_port = htons(ss->client_port);
        ss_addr.sin_addr.s_addr = inet_addr(ss->ip);

        if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0)
        {
            close(ss_sock);
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        char ss_cmd[BUFFER_SIZE];
        snprintf(ss_cmd, sizeof(ss_cmd), "READ|%s\n", filename);
        send(ss_sock, ss_cmd, strlen(ss_cmd), 0);

        char file_content[BUFFER_SIZE * 4];
        file_content[0] = '\0';
        size_t content_len = 0;

        char recv_buf[BUFFER_SIZE];

        while (1)
        {
            ssize_t n = recv(ss_sock, recv_buf, sizeof(recv_buf) - 1, 0);
            if (n <= 0) break;

            recv_buf[n] = '\0';

            char *stop_pos = strstr(recv_buf, "--STOP--");
            if (stop_pos) *stop_pos = '\0';

            size_t copy_len = strlen(recv_buf);
            if (content_len + copy_len < sizeof(file_content) - 1)
            {
                memcpy(file_content + content_len, recv_buf, copy_len);
                content_len += copy_len;
                file_content[content_len] = '\0';
            }

            if (stop_pos) break;
        }

        close(ss_sock);

        trim_string(file_content);
        content_len = strlen(file_content);

        if (content_len == 0)
        {
            send_response(client_sock, "No commands to execute.\n");
            return;
        }

        FILE *exec_pipe = popen(file_content, "r");
        if (!exec_pipe)
        {
            send_response(client_sock, "ERROR: Execution failed\n");
            return;
        }

        char exec_output[BUFFER_SIZE];
        while (fgets(exec_output, sizeof(exec_output), exec_pipe) != NULL)
        {
            send_response(client_sock, exec_output);
        }

        int exit_status = pclose(exec_pipe);
        if (exit_status == 0)
        {
            log_response(username, ERR_SUCCESS, filename);
        }
        else
        {
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg),
                     "\nExecution completed with exit code: %d\n", exit_status);
            send_response(client_sock, err_msg);
        }
    }

    // ================= UNDO =================
    else if (strcasecmp(token, "UNDO") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        if (!filename)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }

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

        if (file->ss_index < 0 || !nm_state.storage_servers[file->ss_index].active)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        StorageServerInfo *ss = &nm_state.storage_servers[file->ss_index];

        char resp[BUFFER_SIZE];
        snprintf(resp, sizeof(resp),
                 "CONNECT_SS_UNDO %s %d %s\n",
                 ss->ip, ss->client_port, filename);

        send_response(client_sock, resp);

        file->last_modified = time(NULL);
        file->last_accessed = file->last_modified;
        strncpy(file->last_accessed_by, username, MAX_USERNAME - 1);

        log_response(username, ERR_SUCCESS, filename);
        persist_metadata();
    }

    // ================= INVALID COMMAND =================
    else
    {
        send_error(client_sock, ERR_INVALID_COMMAND);
    }
}

// ================= VIEW =================
void cmd_view(int client_sock, const char *username, int show_all, int show_details)
{
    FileMetadata **files;
    int count = get_all_files(&nm_state.file_map, &files);

    if (count == 0)
    {
        send_response(client_sock, "No files in system.\n");
        return;
    }

    char response[BUFFER_SIZE * 4];
    response[0] = '\0';

    char content[BUFFER_SIZE * 3];
    content[0] = '\0';
    
    int accessible_files = 0;

    for (int i = 0; i < count; i++)
    {
        FileMetadata *file = files[i];
        AccessLevel access = check_access(file->filename, username);

        if (!show_all && access == ACCESS_NONE)
        {
            continue;
        }

        accessible_files++;

        if (show_details)
        {

            // Ensure metadata like size/word/char counts are up-to-date
            // before displaying detailed information
            if (refresh_file_metadata(file) != 0)
            {
                char warn_msg[512];
                snprintf(warn_msg, sizeof(warn_msg), "Using cached metadata for %s", file->filename);
                log_message("WARN", NULL, 0, username, warn_msg);
            }
            char time_str[64];
            format_time(file->last_accessed, time_str, sizeof(time_str));

            char line[512];
            snprintf(line, sizeof(line), "| %-10s | %5d | %5d | %19s | %-5s |\n", file->filename, file->word_count, file->char_count, time_str, file->owner);
            strcat(content, line);
        }

        else
        {
            char line[300];
            snprintf(line, sizeof(line), "--> %s\n", file->filename);
            strcat(response, line);
        }
    }

    if (accessible_files == 0)
    {
        send_response(client_sock, "No accessible files found.\n");
    }

    else
    {
        if (show_details)
        {
            strcat(response, "------------------------------------------------------------\n");
            strcat(response, "|  Filename  | Words | Chars |   Last Access Time  | Owner |\n");
            strcat(response, "|------------|-------|-------|---------------------|-------|\n");
        }
        
        strcat(response, content);

        if (show_details)
        {
            strcat(response, "------------------------------------------------------------\n");
        }

        send_response(client_sock, response);
    }

    free(files);
}

// ================= LIST =================
void cmd_list(int client_sock, const char *username)
{
    (void)username;

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

// ================= INFO =================
void cmd_info(int client_sock, const char *username, const char *filename)
{
    (void)username;

    FileMetadata *file = find_file(&nm_state.file_map, filename);
    if (!file)
    {
        send_error(client_sock, ERR_FILE_NOT_FOUND);
        return;
    }

    // refresh size/word/char statistics before showing INFO
    if (refresh_file_metadata(file) != 0)
    {
        char warn_msg[512];
        snprintf(warn_msg, sizeof(warn_msg), "Using cached metadata for %s", file->filename);
        log_message("WARN", NULL, 0, username, warn_msg);
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

// ================= ADDACCESS =================
void cmd_add_access(int client_sock, const char *username,
                    const char *filename, const char *target_user,
                    int is_write)
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

    send_response(client_sock, "Access granted successfully!\n");
    log_response(username, ERR_SUCCESS, filename);
    persist_metadata();
}

// ================= REMACCESS =================
void cmd_remove_access(int client_sock, const char *username,
                       const char *filename, const char *target_user)
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

    ErrorCode result = remove_access(filename, target_user);

    if (result != ERR_SUCCESS)
    {
        send_error(client_sock, result);
        return;
    }

    send_response(client_sock, "Access removed successfully!\n");
    log_response(username, ERR_SUCCESS, filename);
    persist_metadata();
}
