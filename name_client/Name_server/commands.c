#include "commands.h"
#include <ctype.h>
#include <stdarg.h>

#define STOP_PACKET "\n--STOP--\n"

static int normalize_file_path_or_error(int client_sock, const char *input, char *output, size_t out_size);
static int normalize_folder_path_or_error(int client_sock, const char *input, char *output, size_t out_size, int allow_root);
static void extract_parent_path(const char *path, char *parent, size_t parent_size);
static const char *path_basename(const char *path);
static int has_parent_path(const char *path, const char *parent);
static void append_response_line(char *buffer, size_t buf_size, size_t *used, const char *fmt, ...);
static int is_valid_checkpoint_tag(const char *tag);
static int connect_to_ss(StorageServerInfo *ss);
static int read_line(int sock, char *buffer, size_t buf_size);
static const char *find_sequence(const char *haystack, size_t hay_len, const char *needle, size_t needle_len);
static int forward_until_stop(int src_sock, int dst_sock);


static int normalize_path_common(const char *input, char *output, size_t out_size, int allow_root)
{
    if (!input || !output || out_size == 0)
    {
        return 0;
    }

    char temp[MAX_PATH];
    strncpy(temp, input, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    trim_string(temp);

    if (temp[0] == '\0')
    {
        if (allow_root)
        {
            output[0] = '\0';
            return 1;
        }
        return 0;
    }

    if (strstr(temp, "..") != NULL)
    {
        return 0;
    }

    for (char *p = temp; *p; ++p)
    {
        if (*p == '\\')
        {
            return 0;
        }
    }

    char compact[MAX_PATH];
    size_t idx = 0;
    for (size_t i = 0; temp[i] && idx < sizeof(compact) - 1; ++i)
    {
        compact[idx++] = temp[i];
        if (temp[i] == '/')
        {
            while (temp[i + 1] == '/')
            {
                i++;
            }
        }
    }
    compact[idx] = '\0';

    size_t len = strlen(compact);
    while (len > 1 && compact[len - 1] == '/')
    {
        compact[--len] = '\0';
    }

    if (strcmp(compact, "/") == 0)
    {
        if (allow_root)
        {
            output[0] = '\0';
            return 1;
        }
        return 0;
    }

    if (compact[0] == '/')
    {
        memmove(compact, compact + 1, strlen(compact));
    }

    if (compact[0] == '\0')
    {
        if (allow_root)
        {
            output[0] = '\0';
            return 1;
        }
        return 0;
    }

    strncpy(output, compact, out_size - 1);
    output[out_size - 1] = '\0';
    return 1;
}

static int normalize_file_path_or_error(int client_sock, const char *input, char *output, size_t out_size)
{
    if (!input)
    {
        send_error(client_sock, ERR_INVALID_ARGUMENTS);
        return 0;
    }
    if (!normalize_path_common(input, output, out_size, 0))
    {
        send_error(client_sock, ERR_INVALID_PATH);
        return 0;
    }
    return 1;
}

static int normalize_folder_path_or_error(int client_sock, const char *input, char *output, size_t out_size, int allow_root)
{
    if (!input)
    {
        send_error(client_sock, ERR_INVALID_ARGUMENTS);
        return 0;
    }
    if (!normalize_path_common(input, output, out_size, allow_root))
    {
        send_error(client_sock, ERR_INVALID_PATH);
        return 0;
    }
    return 1;
}

static void extract_parent_path(const char *path, char *parent, size_t parent_size)
{
    if (!path || !parent || parent_size == 0)
    {
        return;
    }
    const char *slash = strrchr(path, '/');
    if (!slash)
    {
        parent[0] = '\0';
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len >= parent_size)
    {
        len = parent_size - 1;
    }
    strncpy(parent, path, len);
    parent[len] = '\0';
}

static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int has_parent_path(const char *path, const char *parent)
{
    char extracted[MAX_FOLDER_PATH];
    extract_parent_path(path, extracted, sizeof(extracted));
    return strcmp(extracted, parent) == 0;
}

static void append_response_line(char *buffer, size_t buf_size, size_t *used, const char *fmt, ...)
{
    if (*used >= buf_size)
    {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buffer + *used, buf_size - *used, fmt, args);
    va_end(args);
    if (written < 0)
    {
        return;
    }
    if ((size_t)written >= buf_size - *used)
    {
        *used = buf_size - 1;
    }
    else
    {
        *used += (size_t)written;
    }
}

static int is_valid_checkpoint_tag(const char *tag)
{
    if (!tag || tag[0] == '\0')
    {
        return 0;
    }
    if (strstr(tag, "..") != NULL)
    {
        return 0;
    }
    for (const char *p = tag; *p; ++p)
    {
        if (*p == '/' || *p == '\\' || *p == '|' || *p == '\n' || *p == '\r')
        {
            return 0;
        }
    }
    return 1;
}

static int connect_to_ss(StorageServerInfo *ss)
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
    return sock;
}

static int read_line(int sock, char *buffer, size_t buf_size)
{
    if (!buffer || buf_size == 0)
    {
        return -1;
    }
    size_t pos = 0;
    while (pos < buf_size - 1)
    {
        char c;
        ssize_t n = recv(sock, &c, 1, 0);
        if (n <= 0)
        {
            return -1;
        }
        buffer[pos++] = c;
        if (c == '\n')
        {
            break;
        }
    }
    buffer[pos] = '\0';
    return 0;
}

static const char *find_sequence(const char *haystack, size_t hay_len, const char *needle, size_t needle_len)
{
    if (!haystack || !needle || needle_len == 0 || hay_len < needle_len)
    {
        return NULL;
    }
    for (size_t i = 0; i + needle_len <= hay_len; ++i)
    {
        if (memcmp(haystack + i, needle, needle_len) == 0)
        {
            return haystack + i;
        }
    }
    return NULL;
}

static int forward_until_stop(int src_sock, int dst_sock)
{
    char buffer[BUFFER_SIZE * 2];
    size_t buffered = 0;
    const size_t stop_len = strlen(STOP_PACKET);

    while (1)
    {
        ssize_t n = recv(src_sock, buffer + buffered, sizeof(buffer) - buffered, 0);
        if (n <= 0)
        {
            return -1;
        }
        buffered += (size_t)n;
        const char *stop_ptr = find_sequence(buffer, buffered, STOP_PACKET, stop_len);
        if (stop_ptr)
        {
            size_t data_len = (size_t)(stop_ptr - buffer);
            if (data_len > 0 && send(dst_sock, buffer, data_len, 0) < 0)
            {
                return -1;
            }
            return 0;
        }
        if (buffered > stop_len)
        {
            size_t flush_len = buffered - stop_len;
            if (flush_len > 0)
            {
                if (send(dst_sock, buffer, flush_len, 0) < 0)
                {
                    return -1;
                }
                memmove(buffer, buffer + flush_len, buffered - flush_len);
                buffered -= flush_len;
            }
        }
    }
}

// Send a normal response to the client
void send_response(int sock, const char *message)
{
    size_t len = strlen(message);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, message + sent, len - sent, 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
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
        printf("[NM] Detected SS failure at %s:%d. Marking inactive.\n", ss->ip, ss->nm_port);
        ss->active = 0; 
        close(sock);
        ss->active = 0;
        log_message("ERROR", ss->ip, ss->nm_port, "SYSTEM", "Storage server connection failed - marked as inactive");
        return -1;
    }

    if (send(sock, cmd, strlen(cmd), 0) < 0)
    {
        ss->active = 0;
        close(sock);
        ss->active = 0;
        log_message("ERROR", ss->ip, ss->nm_port, "SYSTEM", "Storage server send failed - marked as inactive");
        return -1;
    }

    ssize_t n = recv(sock, response, resp_size - 1, 0);
    if (n < 0)
    {   
        ss->active = 0;
        close(sock);
        ss->active = 0;
        log_message("ERROR", ss->ip, ss->nm_port, "SYSTEM", "Storage server recv failed - marked as inactive");
        return -1;
    }

    response[n] = '\0';
    close(sock);
    return 0;
}

int synchronize_file_between_ss(const char *filename, StorageServerInfo *source_ss, StorageServerInfo *dest_ss)
{
    if (!source_ss || !dest_ss || !source_ss->active || !dest_ss->active)
    {
        return -1;
    }

    int dest_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (dest_sock < 0)
    {
        return -1;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_ss->nm_port);
    dest_addr.sin_addr.s_addr = inet_addr(dest_ss->ip);

    if (connect(dest_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0)
    {
        close(dest_sock);
        return -1;
    }

    char sync_cmd[512];
    snprintf(sync_cmd, sizeof(sync_cmd), "SYNC|%s|%d|%s\n", source_ss->ip, source_ss->client_port, filename);
    send(dest_sock, sync_cmd, strlen(sync_cmd), 0);

    char ack[256];
    ssize_t n = recv(dest_sock, ack, sizeof(ack) - 1, 0);
    close(dest_sock);

    if (n > 0 && strstr(ack, "ACK_SYNC OK"))
    {
        return 0;
    }

    return -1;
}

static const char *access_level_to_string(AccessLevel level)
{
    switch (level)
    {
        case ACCESS_READ: return "READ";
        case ACCESS_WRITE: return "WRITE";
        case ACCESS_OWNER: return "OWNER";
        default: return "NONE";
    }
}

static int access_level_rank(AccessLevel level)
{
    switch (level)
    {
        case ACCESS_OWNER: return 3;
        case ACCESS_WRITE: return 2;
        case ACCESS_READ: return 1;
        default: return 0;
    }
}

static int has_equal_or_higher_access(AccessLevel current, AccessLevel desired)
{
    return access_level_rank(current) >= access_level_rank(desired);
}

static int parse_access_level_token(const char *token, AccessLevel *out_level)
{
    if (!token || !out_level)
    {
        return 0;
    }
    if (strcasecmp(token, "READ") == 0 || strcasecmp(token, "R") == 0)
    {
        *out_level = ACCESS_READ;
        return 1;
    }
    if (strcasecmp(token, "WRITE") == 0 || strcasecmp(token, "W") == 0 ||
        strcasecmp(token, "RW") == 0)
    {
        *out_level = ACCESS_WRITE;
        return 1;
    }
    return 0;
}

static int find_access_request_index(FileMetadata *file, const char *username)
{
    if (!file || !username)
    {
        return -1;
    }
    for (int i = 0; i < file->request_count; i++)
    {
        if (strcmp(file->requests[i].username, username) == 0)
        {
            return i;
        }
    }
    return -1;
}

static void remove_access_request_at(FileMetadata *file, int index)
{
    if (!file || index < 0 || index >= file->request_count)
    {
        return;
    }
    for (int i = index; i < file->request_count - 1; i++)
    {
        file->requests[i] = file->requests[i + 1];
    }
    if (file->request_count > 0)
    {
        file->request_count--;
    }
}

static StorageServerInfo *ensure_primary_server(FileMetadata *file)
{
    if (!file)
    {
        return NULL;
    }

    StorageServerInfo *primary = get_storage_server(file->ss_index);
    if (primary)
    {
        return primary;
    }

    if (file->replica_ss_index >= 0)
    {
        StorageServerInfo *replica = get_storage_server(file->replica_ss_index);
        if (replica)
        {
            char log_buf[BUFFER_SIZE];
            snprintf(log_buf, sizeof(log_buf),
                     "Promoting replica SS %d for file %s", file->replica_ss_index, file->filename);
            log_message("WARN", replica->ip, replica->nm_port, "SYSTEM", log_buf);

            file->ss_index = file->replica_ss_index;
            file->replica_ss_index = -1;
            persist_metadata();
            return replica;
        }
    }
    return NULL;
}

static StorageServerInfo *get_replica_server(FileMetadata *file)
{
    if (!file || file->replica_ss_index < 0)
    {
        return NULL;
    }

    StorageServerInfo *replica = get_storage_server(file->replica_ss_index);
    if (!replica)
    {
        char log_buf[BUFFER_SIZE];
        snprintf(log_buf, sizeof(log_buf),
                 "Replica SS %d unavailable for file %s",
                 file->replica_ss_index, file->filename);
        log_message("WARN", NULL, 0, "SYSTEM", log_buf);
        file->replica_ss_index = -1;
        persist_metadata();
    }
    return replica;
}

static void send_command_to_replica(FileMetadata *file, const char *cmd, const char *ok_token)
{
    StorageServerInfo *replica = get_replica_server(file);
    if (!replica || !cmd)
    {
        return;
    }

    char ack[256];
    if (send_ss_command(replica, cmd, ack, sizeof(ack)) != 0 ||
        (ok_token && strstr(ack, ok_token) == NULL))
    {
        char log_buf[BUFFER_SIZE];
        snprintf(log_buf, sizeof(log_buf),
                 "Replica command failed for file %s on SS %d",
                 file->filename, file->replica_ss_index);
        log_message("WARN", replica->ip, replica->nm_port, "SYSTEM", log_buf);
    }
}

static int build_connect_packet(FileMetadata *file, const char *filename, int is_undo, char *out, size_t out_size)
{
    if (!file || !filename || !out || out_size == 0)
    {
        return 0;
    }

    StorageServerInfo *primary = ensure_primary_server(file);
    if (!primary)
    {
        return 0;
    }

    StorageServerInfo *replica = get_replica_server(file);
    const char *prefix = is_undo ? "CONNECT_SS_UNDO" : "CONNECT_SS";
    if (replica)
    {
        snprintf(out, out_size, "%s %s %d %s REPLICA %s %d\n",
                 prefix, primary->ip, primary->client_port, filename,
                 replica->ip, replica->client_port);
    }
    else
    {
        snprintf(out, out_size, "%s %s %d %s\n",
                 prefix, primary->ip, primary->client_port, filename);
    }
    return 1;
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
        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        if (find_file(&nm_state.file_map, normalized_filename))
        {
            send_error(client_sock, ERR_FILE_ALREADY_EXISTS);
            return;
        }

        char parent_path[MAX_FOLDER_PATH];
        extract_parent_path(normalized_filename, parent_path, sizeof(parent_path));
        if (parent_path[0] != '\0' && !find_folder(&nm_state.folder_map, parent_path))
        {
            send_error(client_sock, ERR_FOLDER_NOT_FOUND);
            return;
        }

        int ss_index = find_least_loaded_ss();
        if (ss_index == -1)
        {
            send_error(client_sock, ERR_NO_STORAGE_SERVERS);
            return;
        }
        
        StorageServerInfo *primary = get_storage_server(ss_index);

        if (!primary)
        {
            send_error(client_sock, ERR_NO_STORAGE_SERVERS);
            return;
        }

        int replica_index = find_replica_ss(ss_index);
        StorageServerInfo *replica = (replica_index >= 0) ? get_storage_server(replica_index) : NULL;
        if (!replica)
        {
            replica_index = -1;
        }

        char ss_cmd[BUFFER_SIZE];
        snprintf(ss_cmd, sizeof(ss_cmd), "CREATE|%s\n", normalized_filename);

        char ack[256];
        if (send_ss_command(primary, ss_cmd, ack, sizeof(ack)) == 0 &&
            strstr(ack, "ACK_CREATE OK"))
        {
            int replica_ok = 0;
            if (replica)
            {
                char replica_ack[256];
                if (send_ss_command(replica, ss_cmd, replica_ack, sizeof(replica_ack)) == 0 &&
                    strstr(replica_ack, "ACK_CREATE OK"))
                {
                    replica_ok = 1;
                }
                else
                {
                    char log_buf[BUFFER_SIZE];
                    snprintf(log_buf, sizeof(log_buf),
                             "Failed to create replica for %s on SS %d",
                             normalized_filename, replica_index);
                    log_message("WARN", replica->ip, replica->nm_port, username, log_buf);
                    replica_index = -1;
                }
            }

            FileMetadata meta = {0};
            strncpy(meta.filename, normalized_filename, MAX_FILENAME - 1);
            strncpy(meta.owner, username, MAX_USERNAME - 1);

            meta.ss_index = ss_index;
            meta.replica_ss_index = replica_index;
            meta.created_time = time(NULL);
            meta.last_modified = time(NULL);
            meta.last_accessed = time(NULL);
            strncpy(meta.last_accessed_by, username, MAX_USERNAME - 1);

            insert_file(&nm_state.file_map, &meta);

            track_file_on_ss(meta.ss_index, normalized_filename);
            if (replica_ok && replica_index >= 0)
            {
                track_file_on_ss(replica_index, normalized_filename);
            }

            char resp[BUFFER_SIZE];
            snprintf(resp, sizeof(resp), "File '%s' created successfully!\n", normalized_filename);
            send_response(client_sock, resp);
            log_response(username, ERR_SUCCESS, normalized_filename);
            persist_metadata();
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

    // ================= CREATEFOLDER =================
    else if (strcasecmp(token, "CREATEFOLDER") == 0)
    {
        char *folder = strtok(NULL, " \t\n");
        if (!folder)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        cmd_create_folder(client_sock, username, folder);
    }

    // ================= VIEWFOLDER =================
    else if (strcasecmp(token, "VIEWFOLDER") == 0)
    {
        char *folder = strtok(NULL, " \t\n");
        cmd_view_folder(client_sock, username, folder);
    }

    // ================= MOVE =================
    else if (strcasecmp(token, "MOVE") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char *folder = strtok(NULL, " \t\n");
        if (!filename || !folder)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        cmd_move_file(client_sock, username, filename, folder);
    }

    // ================= READ =================
    else if (strcasecmp(token, "READ") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        FileMetadata *file = find_file(&nm_state.file_map, normalized_filename);
        if (!file)
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }

        AccessLevel access = check_access(normalized_filename, username);
        if (access == ACCESS_NONE)
        {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }

        char resp[BUFFER_SIZE];
        if (!build_connect_packet(file, normalized_filename, 0, resp, sizeof(resp)))
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        send_response(client_sock, resp);

        file->last_accessed = time(NULL);
        strncpy(file->last_accessed_by, username, MAX_USERNAME - 1);
        log_response(username, ERR_SUCCESS, normalized_filename);
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
        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        cmd_info(client_sock, username, normalized_filename);
    }

    // ================= DELETE =================
    else if (strcasecmp(token, "DELETE") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        FileMetadata *file = find_file(&nm_state.file_map, normalized_filename);
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

        StorageServerInfo *ss = ensure_primary_server(file);
        if (!ss)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        char ss_cmd[BUFFER_SIZE];
        snprintf(ss_cmd, sizeof(ss_cmd), "DELETE|%s\n", normalized_filename);

        char ack[256];
        if (send_ss_command(ss, ss_cmd, ack, sizeof(ack)) == 0 &&
            strstr(ack, "ACK_DELETE OK"))
        {
            send_command_to_replica(file, ss_cmd, "ACK_DELETE OK");
            untrack_file_on_ss(file->ss_index, normalized_filename);
            if (file->replica_ss_index >= 0)
            {
                untrack_file_on_ss(file->replica_ss_index, normalized_filename);
            }

            delete_file(&nm_state.file_map, filename);

            char resp[BUFFER_SIZE];
            snprintf(resp, sizeof(resp), "File '%s' deleted successfully!\n", normalized_filename);
            send_response(client_sock, resp);
            log_response(username, ERR_SUCCESS, normalized_filename);
            persist_metadata();
        }
        else
        {
            send_response(client_sock, "ERROR: Storage Server failed to delete file\n");
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

        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        cmd_add_access(client_sock, username, normalized_filename, target_user, is_write);
    }

    // ================= REQUESTACCESS =================
    else if (strcasecmp(token, "REQUESTACCESS") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char *access_flag = strtok(NULL, " \t\n");

        if (!filename || !access_flag)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }

        AccessLevel desired;
        if (!parse_access_level_token(access_flag, &desired))
        {
            send_error(client_sock, ERR_INVALID_ACCESS_LEVEL);
            return;
        }

        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        cmd_request_access(client_sock, username, normalized_filename, desired);
    }

    // ================= VIEWREQUESTS =================
    else if (strcasecmp(token, "VIEWREQUESTS") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }
        cmd_view_requests(client_sock, username, normalized_filename);
    }

    // ================= APPROVEREQUEST =================
    else if (strcasecmp(token, "APPROVEREQUEST") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char *target_user = strtok(NULL, " \t\n");
        if (!filename || !target_user)
        {
            return;
        }
        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }
        cmd_approve_request(client_sock, username, normalized_filename, target_user);
    }

    // ================= DENYREQUEST =================
    else if (strcasecmp(token, "DENYREQUEST") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char *target_user = strtok(NULL, " \t\n");
        if (!filename || !target_user)
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }
        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }
        cmd_deny_request(client_sock, username, normalized_filename, target_user);
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

        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        cmd_remove_access(client_sock, username, normalized_filename, target_user);
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

        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        FileMetadata *file = find_file(&nm_state.file_map, normalized_filename);
        if (!file)
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }

        AccessLevel access = check_access(normalized_filename, username);
        if (access != ACCESS_OWNER && access != ACCESS_WRITE)
        {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }

        char resp[BUFFER_SIZE];
        if (!build_connect_packet(file, normalized_filename, 0, resp, sizeof(resp)))
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }
        send_response(client_sock, resp);

        // Optimistically mark the file as modified; exact size/word/char
        // counts will be refreshed on the next VIEW -l / INFO.
        file->last_modified = time(NULL);
        file->last_accessed = file->last_modified;
        strncpy(file->last_accessed_by, username, MAX_USERNAME - 1);

        log_response(username, ERR_SUCCESS, normalized_filename);
        persist_metadata();
    }

    // ================= STREAM =================
    else if (strcasecmp(token, "STREAM") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        FileMetadata *file = find_file(&nm_state.file_map, normalized_filename);
        if (!file)
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }

        AccessLevel access = check_access(normalized_filename, username);
        if (access == ACCESS_NONE)
        {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }

        char resp[BUFFER_SIZE];
        if (!build_connect_packet(file, normalized_filename, 0, resp, sizeof(resp)))
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }
        send_response(client_sock, resp);

        file->last_accessed = time(NULL);
        strncpy(file->last_accessed_by, username, MAX_USERNAME - 1);
        log_response(username, ERR_SUCCESS, normalized_filename);
    }

    // ================= CHECKPOINT =================
    else if (strcasecmp(token, "CHECKPOINT") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char *tag = strtok(NULL, " \t\n");
        if (!tag || !is_valid_checkpoint_tag(tag))
        {
            return;
        }

        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        FileMetadata *file = find_file(&nm_state.file_map, normalized_filename);
        if (!file)
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }

        AccessLevel access = check_access(normalized_filename, username);
        if (access != ACCESS_OWNER && access != ACCESS_WRITE)
        {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }

        StorageServerInfo *ss = ensure_primary_server(file);
        if (!ss)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }
        char ss_cmd[BUFFER_SIZE];
        snprintf(ss_cmd, sizeof(ss_cmd), "CHECKPOINT|%s|%s\n", normalized_filename, tag);
        char ack[BUFFER_SIZE];
        if (send_ss_command(ss, ss_cmd, ack, sizeof(ack)) == 0 &&
            strstr(ack, "ACK_CHECKPOINT OK"))
        {
            send_command_to_replica(file, ss_cmd, "ACK_CHECKPOINT OK");
            send_response(client_sock, "Checkpoint created successfully!\n");
            log_response(username, ERR_SUCCESS, normalized_filename);
        }
        else
        {
            send_response(client_sock, "ERROR: Failed to create checkpoint\n");
        }
    }

    // ================= VIEWCHECKPOINT =================
    else if (strcasecmp(token, "VIEWCHECKPOINT") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char *tag = strtok(NULL, " \t\n");
        if (!tag || !is_valid_checkpoint_tag(tag))
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }

        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        FileMetadata *file = find_file(&nm_state.file_map, normalized_filename);
        if (!file)
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }

        AccessLevel access = check_access(normalized_filename, username);
        if (access == ACCESS_NONE)
        {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }

        StorageServerInfo *ss = ensure_primary_server(file);
        int ss_sock = connect_to_ss(ss);
        if (ss_sock < 0)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        char ss_cmd[BUFFER_SIZE];
        snprintf(ss_cmd, sizeof(ss_cmd), "VIEWCHECKPOINT|%s|%s\n", normalized_filename, tag);
        send(ss_sock, ss_cmd, strlen(ss_cmd), 0);

        char ack_line[BUFFER_SIZE];
        if (read_line(ss_sock, ack_line, sizeof(ack_line)) != 0)
        {
            close(ss_sock);
            send_error(client_sock, ERR_INTERNAL_ERROR);
            return;
        }

        if (strstr(ack_line, "ACK_VIEWCHECKPOINT OK") == NULL)
        {
            close(ss_sock);
            send_response(client_sock, "ERROR: Failed to read checkpoint\n");
            return;
        }

        send_response(client_sock, "Checkpoint contents:\n");
        if (forward_until_stop(ss_sock, client_sock) != 0)
        {
            send_response(client_sock, "\nERROR: Stream interrupted\n");
            close(ss_sock);
            return;
        }
        close(ss_sock);
        send_response(client_sock, "\n");
    }

    // ================= REVERT =================
    else if (strcasecmp(token, "REVERT") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char *tag = strtok(NULL, " \t\n");
        if (!tag || !is_valid_checkpoint_tag(tag))
        {
            send_error(client_sock, ERR_INVALID_ARGUMENTS);
            return;
        }

        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        FileMetadata *file = find_file(&nm_state.file_map, normalized_filename);
        if (!file)
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }

        AccessLevel access = check_access(normalized_filename, username);
        if (access != ACCESS_OWNER && access != ACCESS_WRITE)
        {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }

        StorageServerInfo *ss = ensure_primary_server(file);
        if (!ss)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }
        char ss_cmd[BUFFER_SIZE];
        snprintf(ss_cmd, sizeof(ss_cmd), "REVERT|%s|%s\n", normalized_filename, tag);
        char ack[BUFFER_SIZE];
        if (send_ss_command(ss, ss_cmd, ack, sizeof(ack)) == 0 &&
            strstr(ack, "ACK_REVERT OK"))
        {
            send_command_to_replica(file, ss_cmd, "ACK_REVERT OK");
            file->last_modified = time(NULL);
            file->last_accessed = file->last_modified;
            strncpy(file->last_accessed_by, username, MAX_USERNAME - 1);
            send_response(client_sock, "File reverted to checkpoint successfully!\n");
            log_response(username, ERR_SUCCESS, normalized_filename);
            persist_metadata();
        }
        else
        {
            send_response(client_sock, "ERROR: Failed to revert file\n");
        }
    }

    // ================= LISTCHECKPOINTS =================
    else if (strcasecmp(token, "LISTCHECKPOINTS") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        FileMetadata *file = find_file(&nm_state.file_map, normalized_filename);
        if (!file)
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }

        AccessLevel access = check_access(normalized_filename, username);
        if (access == ACCESS_NONE)
        {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }

        StorageServerInfo *ss = ensure_primary_server(file);
        if (!ss)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }
        char ss_cmd[BUFFER_SIZE];
        snprintf(ss_cmd, sizeof(ss_cmd), "LISTCHECKPOINTS|%s\n", normalized_filename);
        char ack[BUFFER_SIZE];
        if (send_ss_command(ss, ss_cmd, ack, sizeof(ack)) != 0)
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        const char *prefix = "ACK_LISTCHECKPOINTS OK ";
        if (strncmp(ack, prefix, strlen(prefix)) != 0)
        {
            send_response(client_sock, "ERROR: Failed to list checkpoints\n");
            return;
        }

        char *list_payload = ack + strlen(prefix);
        size_t payload_len = strlen(list_payload);
        while (payload_len > 0 &&
               (list_payload[payload_len - 1] == '\n' ||
                list_payload[payload_len - 1] == '\r'))
        {
            list_payload[--payload_len] = '\0';
        }

        char response[BUFFER_SIZE * 2];
        if (payload_len == 0)
        {
            snprintf(response, sizeof(response),
                     "Checkpoints for %s:\n  (none)\n",
                     normalized_filename);
        }
        else
        {
            snprintf(response, sizeof(response),
                     "Checkpoints for %s:\n%s\n",
                     normalized_filename, list_payload);
        }
        send_response(client_sock, response);
    }

    // ================= EXEC =================
    else if (strcasecmp(token, "EXEC") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        FileMetadata *file = find_file(&nm_state.file_map, normalized_filename);
        if (!file)
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }

        AccessLevel access = check_access(normalized_filename, username);
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
        snprintf(ss_cmd, sizeof(ss_cmd), "READ|%s\n", normalized_filename);
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

        // Split file content by sentence delimiters (. ! ?) and execute each sentence separately
        int total_exit_status = 0;
        int command_count = 0;
        char current_sentence[BUFFER_SIZE * 4];
        size_t sentence_len = 0;

        for (size_t i = 0; i <= content_len; i++)
        {
            // Check for sentence delimiter or end of content
            int is_delimiter = (file_content[i] == '.' || file_content[i] == '!' || file_content[i] == '?');
            int is_end = (i == content_len);

            if (!is_delimiter && !is_end)
            {
                // Accumulate characters in current sentence
                if (sentence_len < sizeof(current_sentence) - 1)
                {
                    current_sentence[sentence_len++] = file_content[i];
                }
                continue;
            }

            if (sentence_len > 0)
            {
                current_sentence[sentence_len] = '\0';

                // Trim whitespace and delimiters
                char *cmd_start = current_sentence;
                while (*cmd_start == ' ' || *cmd_start == '\t' || *cmd_start == '\r' || *cmd_start == '\n')
                    cmd_start++;

                char *cmd_end = cmd_start + strlen(cmd_start) - 1;
                while (cmd_end >= cmd_start && 
                       (*cmd_end == ' ' || *cmd_end == '\t' || *cmd_end == '\r' || *cmd_end == '\n' ||
                        *cmd_end == '.' || *cmd_end == '!' || *cmd_end == '?'))
                    cmd_end--;
                cmd_end[1] = '\0';

                if (*cmd_start != '\0')
                {
                    command_count++;

                    FILE *exec_pipe = popen(cmd_start, "r");
                    if (!exec_pipe)
                    {
                        total_exit_status = -1;
                    }
                    else
                    {
                        char exec_output[BUFFER_SIZE];
                        while (fgets(exec_output, sizeof(exec_output), exec_pipe) != NULL)
                        {
                            send_response(client_sock, exec_output);
                        }

                        int exit_status = pclose(exec_pipe);
                        if (exit_status != 0)
                        {
                            total_exit_status = exit_status;
                        }
                    }
                }

                sentence_len = 0;
            }
        }

        if (total_exit_status == 0)
        {
            log_response(username, ERR_SUCCESS, filename);
        }

        // Signal end of multi-part response to client
        send_response(client_sock, STOP_PACKET);
    }

    // ================= UNDO =================
    else if (strcasecmp(token, "UNDO") == 0)
    {
        char *filename = strtok(NULL, " \t\n");
        char normalized_filename[MAX_FILENAME];
        if (!normalize_file_path_or_error(client_sock, filename, normalized_filename, sizeof(normalized_filename)))
        {
            return;
        }

        FileMetadata *file = find_file(&nm_state.file_map, normalized_filename);
        if (!file)
        {
            send_error(client_sock, ERR_FILE_NOT_FOUND);
            return;
        }

        AccessLevel access = check_access(normalized_filename, username);
        if (access != ACCESS_OWNER && access != ACCESS_WRITE)
        {
            send_error(client_sock, ERR_PERMISSION_DENIED);
            return;
        }

        char resp[BUFFER_SIZE];
        if (!build_connect_packet(file, normalized_filename, 1, resp, sizeof(resp)))
        {
            send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
            return;
        }

        send_response(client_sock, resp);

        file->last_modified = time(NULL);
        file->last_accessed = file->last_modified;
        strncpy(file->last_accessed_by, username, MAX_USERNAME - 1);

        log_response(username, ERR_SUCCESS, normalized_filename);
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

void cmd_request_access(int client_sock, const char *username,
                        const char *filename, AccessLevel desired)
{
    FileMetadata *file = find_file(&nm_state.file_map, filename);
    if (!file)
    {
        send_error(client_sock, ERR_FILE_NOT_FOUND);
        return;
    }

    if (strcmp(file->owner, username) == 0)
    {
        send_error(client_sock, ERR_ALREADY_HAS_ACCESS);
        return;
    }

    AccessLevel current = check_access(filename, username);
    if (has_equal_or_higher_access(current, desired))
    {
        send_error(client_sock, ERR_ALREADY_HAS_ACCESS);
        return;
    }

    time_t now = time(NULL);
    char resp[BUFFER_SIZE];
    int index = find_access_request_index(file, username);

    if (index >= 0)
    {
        file->requests[index].requested_level = desired;
        file->requests[index].requested_at = now;
        snprintf(resp, sizeof(resp),
                 "Updated existing request to %s access.\n",
                 access_level_to_string(desired));
    }
    else
    {
        if (file->request_count >= MAX_ACCESS_REQUESTS)
        {
            send_error(client_sock, ERR_INTERNAL_ERROR);
            return;
        }
        AccessRequest *req = &file->requests[file->request_count++];
        strncpy(req->username, username, MAX_USERNAME - 1);
        req->username[MAX_USERNAME - 1] = '\0';
        req->requested_level = desired;
        req->requested_at = now;
        snprintf(resp, sizeof(resp),
                 "Access request submitted for %s access.\n",
                 access_level_to_string(desired));
    }

    send_response(client_sock, resp);
    log_response(username, ERR_SUCCESS, filename);
    persist_metadata();
}

void cmd_view_requests(int client_sock, const char *username, const char *filename)
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

    if (file->request_count == 0)
    {
        char resp[BUFFER_SIZE];
        snprintf(resp, sizeof(resp), "No pending access requests for %s.\n", filename);
        send_response(client_sock, resp);
        return;
    }

    char response[BUFFER_SIZE * 2];
    response[0] = '\0';
    size_t used = 0;
    append_response_line(response, sizeof(response), &used,
                         "Pending access requests for %s:\n", filename);
    for (int i = 0; i < file->request_count; i++)
    {
        AccessRequest *req = &file->requests[i];
        char ts[64];
        format_time(req->requested_at, ts, sizeof(ts));
        append_response_line(response, sizeof(response), &used,
                             "  - %s (%s requested at %s)\n",
                             req->username,
                             access_level_to_string(req->requested_level),
                             ts);
    }
    send_response(client_sock, response);
}

void cmd_approve_request(int client_sock, const char *username,
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

    int index = find_access_request_index(file, target_user);
    if (index < 0)
    {
        send_error(client_sock, ERR_REQUEST_NOT_FOUND);
        return;
    }

    AccessRequest req = file->requests[index];
    ErrorCode result = (req.requested_level == ACCESS_WRITE)
                           ? add_write_access(filename, target_user)
                           : add_read_access(filename, target_user);
    if (result != ERR_SUCCESS)
    {
        send_error(client_sock, result);
        return;
    }

    remove_access_request_at(file, index);

    char resp[BUFFER_SIZE];
    snprintf(resp, sizeof(resp),
             "Approved %s access for %s.\n",
             access_level_to_string(req.requested_level),
             target_user);
    send_response(client_sock, resp);
    log_response(username, ERR_SUCCESS, filename);
    persist_metadata();
}

void cmd_deny_request(int client_sock, const char *username,
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

    int index = find_access_request_index(file, target_user);
    if (index < 0)
    {
        send_error(client_sock, ERR_REQUEST_NOT_FOUND);
        return;
    }

    remove_access_request_at(file, index);
    char resp[BUFFER_SIZE];
    snprintf(resp, sizeof(resp), "Denied access request from %s.\n", target_user);
    send_response(client_sock, resp);
    log_response(username, ERR_SUCCESS, filename);
    persist_metadata();
}

// ================= CREATEFOLDER =================
void cmd_create_folder(int client_sock, const char *username, const char *folder_path)
{
    char normalized_path[MAX_FOLDER_PATH];
    if (!normalize_folder_path_or_error(client_sock, folder_path, normalized_path, sizeof(normalized_path), 0))
    {
        return;
    }

    if (normalized_path[0] == '\0')
    {
        send_error(client_sock, ERR_INVALID_PATH);
        return;
    }

    if (find_folder(&nm_state.folder_map, normalized_path))
    {
        send_error(client_sock, ERR_FOLDER_ALREADY_EXISTS);
        return;
    }

    char parent[MAX_FOLDER_PATH];
    extract_parent_path(normalized_path, parent, sizeof(parent));
    if (parent[0] != '\0' && !find_folder(&nm_state.folder_map, parent))
    {
        send_error(client_sock, ERR_FOLDER_NOT_FOUND);
        return;
    }

    FolderMetadata meta = {0};
    strncpy(meta.path, normalized_path, MAX_FOLDER_PATH - 1);
    strncpy(meta.owner, username, MAX_USERNAME - 1);
    meta.created_time = time(NULL);

    if (insert_folder(&nm_state.folder_map, &meta) != 0)
    {
        send_error(client_sock, ERR_INTERNAL_ERROR);
        return;
    }

    int sync_failed = 0;
    pthread_mutex_lock(&nm_state.ss_lock);
    for (int i = 0; i < nm_state.storage_count; i++)
    {
        StorageServerInfo *ss = &nm_state.storage_servers[i];
        if (!ss->active)
        {
            continue;
        }
        char ss_cmd[BUFFER_SIZE];
        snprintf(ss_cmd, sizeof(ss_cmd), "CREATEFOLDER|%s\n", normalized_path);
        char ack[256];
        if (send_ss_command(ss, ss_cmd, ack, sizeof(ack)) != 0 ||
            strstr(ack, "ACK_CREATEFOLDER OK") == NULL)
        {
            char warn_msg[BUFFER_SIZE];
            snprintf(warn_msg, sizeof(warn_msg),
                     "Failed to sync folder %s on SS %s:%d",
                     normalized_path, ss->ip, ss->nm_port);
            log_message("WARN", ss->ip, ss->nm_port, username, warn_msg);
            sync_failed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&nm_state.ss_lock);

    if (sync_failed)
    {
        delete_folder(&nm_state.folder_map, normalized_path);
        send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
        return;
    }

    char resp[BUFFER_SIZE];
    char display[MAX_FOLDER_PATH + 2];
    snprintf(display, sizeof(display), "/%s", normalized_path);
    snprintf(resp, sizeof(resp), "Folder '%s' created successfully!\n", display);
    send_response(client_sock, resp);
    log_response(username, ERR_SUCCESS, normalized_path);
    persist_metadata();
}

// ================= VIEWFOLDER =================
void cmd_view_folder(int client_sock, const char *username, const char *folder_path)
{
    const char *requested = folder_path ? folder_path : "/";
    char normalized[MAX_FOLDER_PATH];
    if (!normalize_folder_path_or_error(client_sock, requested, normalized, sizeof(normalized), 1))
    {
        return;
    }

    if (normalized[0] != '\0' && !find_folder(&nm_state.folder_map, normalized))
    {
        send_error(client_sock, ERR_FOLDER_NOT_FOUND);
        return;
    }

    FolderMetadata **folders = NULL;
    FileMetadata **files = NULL;

    int folder_count = get_all_folders(&nm_state.folder_map, &folders);
    int file_count = get_all_files(&nm_state.file_map, &files);

    char response[BUFFER_SIZE * 4];
    response[0] = '\0';
    size_t used = 0;

    char display_path[MAX_FOLDER_PATH + 2];
    if (normalized[0] == '\0')
    {
        strcpy(display_path, "/");
    }
    else
    {
        snprintf(display_path, sizeof(display_path), "/%s", normalized);
    }
    append_response_line(response, sizeof(response), &used, "Folder: %s\n", display_path);

    append_response_line(response, sizeof(response), &used, "Subfolders:\n");
    int subfolders = 0;
    if (folders && folder_count > 0)
    {
        for (int i = 0; i < folder_count; i++)
        {
            FolderMetadata *folder = folders[i];
            if (folder->path[0] == '\0')
            {
                continue;
            }
            if (has_parent_path(folder->path, normalized))
            {
                append_response_line(response, sizeof(response), &used,
                                     "  - %s (owner %s)\n",
                                     path_basename(folder->path), folder->owner);
                subfolders++;
            }
        }
    }
    if (subfolders == 0)
    {
        append_response_line(response, sizeof(response), &used, "  (none)\n");
    }

    append_response_line(response, sizeof(response), &used, "Files:\n");
    int listed_files = 0;
    if (files && file_count > 0)
    {
        for (int i = 0; i < file_count; i++)
        {
            FileMetadata *file = files[i];
            if (!has_parent_path(file->filename, normalized))
            {
                continue;
            }
            AccessLevel access = check_access(file->filename, username);
            if (access == ACCESS_NONE)
            {
                continue;
            }
            append_response_line(response, sizeof(response), &used,
                                 "  - %s (owner %s)\n",
                                 path_basename(file->filename), file->owner);
            listed_files++;
        }
    }
    if (listed_files == 0)
    {
        append_response_line(response, sizeof(response), &used, "  (none)\n");
    }

    send_response(client_sock, response);

    if (folders)
    {
        free(folders);
    }
    if (files)
    {
        free(files);
    }
}

// ================= MOVE =================
void cmd_move_file(int client_sock, const char *username, const char *file_path, const char *target_folder)
{
    char normalized_file[MAX_FILENAME];
    if (!normalize_file_path_or_error(client_sock, file_path, normalized_file, sizeof(normalized_file)))
    {
        return;
    }

    FileMetadata *file = find_file(&nm_state.file_map, normalized_file);
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

    char normalized_folder[MAX_FOLDER_PATH];
    if (!normalize_folder_path_or_error(client_sock, target_folder, normalized_folder, sizeof(normalized_folder), 1))
    {
        return;
    }

    if (normalized_folder[0] != '\0' && !find_folder(&nm_state.folder_map, normalized_folder))
    {
        send_error(client_sock, ERR_FOLDER_NOT_FOUND);
        return;
    }

    const char *basename = path_basename(normalized_file);
    if (basename[0] == '\0')
    {
        send_error(client_sock, ERR_INVALID_PATH);
        return;
    }

    char new_path[MAX_FILENAME];
    if (normalized_folder[0] == '\0')
    {
        if (snprintf(new_path, sizeof(new_path), "%s", basename) >= (int)sizeof(new_path))
        {
            send_error(client_sock, ERR_INVALID_PATH);
            return;
        }
    }
    else if (snprintf(new_path, sizeof(new_path), "%s/%s", normalized_folder, basename) >= (int)sizeof(new_path))
    {
        send_error(client_sock, ERR_INVALID_PATH);
        return;
    }

    if (strcmp(new_path, normalized_file) == 0)
    {
        send_response(client_sock, "File is already in the specified folder.\n");
        return;
    }

    if (find_file(&nm_state.file_map, new_path))
    {
        send_error(client_sock, ERR_FILE_ALREADY_EXISTS);
        return;
    }

    StorageServerInfo *ss = ensure_primary_server(file);
    if (!ss)
    {
        send_error(client_sock, ERR_STORAGE_SERVER_DOWN);
        return;
    }

    char ss_cmd[BUFFER_SIZE];
    snprintf(ss_cmd, sizeof(ss_cmd), "MOVE|%s|%s\n", normalized_file, new_path);
    char ack[256];
    if (send_ss_command(ss, ss_cmd, ack, sizeof(ack)) != 0 ||
        strstr(ack, "ACK_MOVE OK") == NULL)
    {
        send_response(client_sock, "ERROR: Storage Server failed to move file\n");
        return;
    }
    send_command_to_replica(file, ss_cmd, "ACK_MOVE OK");

    FileMetadata updated = *file;
    FileMetadata backup = *file;
    if (delete_file(&nm_state.file_map, normalized_file) != 0)
    {
        send_error(client_sock, ERR_INTERNAL_ERROR);
        return;
    }
    strncpy(updated.filename, new_path, MAX_FILENAME - 1);
    updated.last_modified = time(NULL);
    if (insert_file(&nm_state.file_map, &updated) != 0)
    {
        insert_file(&nm_state.file_map, &backup);
        send_error(client_sock, ERR_INTERNAL_ERROR);
        return;
    }

    rename_tracked_file_on_ss(updated.ss_index, normalized_file, new_path);
    if (updated.replica_ss_index >= 0)
    {
        rename_tracked_file_on_ss(updated.replica_ss_index, normalized_file, new_path);
    }

    char folder_display[MAX_FOLDER_PATH + 2];
    if (normalized_folder[0] == '\0')
    {
        strcpy(folder_display, "/");
    }
    else
    {
        snprintf(folder_display, sizeof(folder_display), "/%s", normalized_folder);
    }
    char resp[BUFFER_SIZE];
    snprintf(resp, sizeof(resp), "File moved to folder '%s'.\n", folder_display);
    send_response(client_sock, resp);
    log_response(username, ERR_SUCCESS, new_path);
    persist_metadata();
}
