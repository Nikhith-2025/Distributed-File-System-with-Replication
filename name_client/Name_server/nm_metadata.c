#include "nm_metadata.h"
#include <ctype.h>
#include <errno.h>
#include <sys/time.h>

#define NM_METADATA_FILE "nm_files/metadata.db"

NameServerState nm_state;

unsigned int hash_filename(const char *filename)
{
    unsigned int hash = 5381;
    int c;
    while ((c = *filename++))
    {
        hash = ((hash << 5) + hash) + c;
    }
    return hash % HASH_TABLE_SIZE;
}

void init_file_hashmap(FileHashMap *map)
{
    for (int i = 0; i < HASH_TABLE_SIZE; i++)
    {
        map->buckets[i] = NULL;
    }
    pthread_mutex_init(&map->lock, NULL);
    map->total_files = 0;
}

int insert_file(FileHashMap *map, FileMetadata *metadata)
{
    pthread_mutex_lock(&map->lock);

    unsigned int index = hash_filename(metadata->filename);
    FileNode *current = map->buckets[index];

    while (current)
    {
        if (strcmp(current->metadata.filename, metadata->filename) == 0)
        {
            pthread_mutex_unlock(&map->lock);
            return -1;
        }
        current = current->next;
    }

    FileNode *new_node = malloc(sizeof(FileNode));
    if (!new_node)
    {
        pthread_mutex_unlock(&map->lock);
        return -1;
    }

    new_node->metadata = *metadata;
    new_node->next = map->buckets[index];
    map->buckets[index] = new_node;
    map->total_files++;

    pthread_mutex_unlock(&map->lock);
    return 0;
}

FileMetadata *find_file(FileHashMap *map, const char *filename)
{
    pthread_mutex_lock(&map->lock);

    unsigned int index = hash_filename(filename);
    FileNode *current = map->buckets[index];

    while (current)
    {
        if (strcmp(current->metadata.filename, filename) == 0)
        {
            pthread_mutex_unlock(&map->lock);
            return &current->metadata;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&map->lock);
    return NULL;
}

int delete_file(FileHashMap *map, const char *filename)
{
    pthread_mutex_lock(&map->lock);

    unsigned int index = hash_filename(filename);
    FileNode *current = map->buckets[index];
    FileNode *prev = NULL;

    while (current)
    {
        if (strcmp(current->metadata.filename, filename) == 0)
        {
            if (prev)
            {
                prev->next = current->next;
            }
            else
            {
                map->buckets[index] = current->next;
            }
            free(current);
            map->total_files--;
            pthread_mutex_unlock(&map->lock);
            return 0;
        }
        prev = current;
        current = current->next;
    }

    pthread_mutex_unlock(&map->lock);
    return -1;
}

int get_all_files(FileHashMap *map, FileMetadata ***files_out)
{
    pthread_mutex_lock(&map->lock);

    if (map->total_files == 0)
    {
        *files_out = NULL;
        pthread_mutex_unlock(&map->lock);
        return 0;
    }

    FileMetadata **files = malloc(sizeof(FileMetadata *) * map->total_files);
    int count = 0;

    for (int i = 0; i < HASH_TABLE_SIZE; i++)
    {
        FileNode *current = map->buckets[i];
        while (current)
        {
            files[count++] = &current->metadata;
            current = current->next;
        }
    }

    *files_out = files;
    pthread_mutex_unlock(&map->lock);
    return count;
}

void free_file_hashmap(FileHashMap *map)
{
    pthread_mutex_lock(&map->lock);

    for (int i = 0; i < HASH_TABLE_SIZE; i++)
    {
        FileNode *current = map->buckets[i];
        while (current)
        {
            FileNode *tmp = current;
            current = current->next;
            free(tmp);
        }
        map->buckets[i] = NULL;
    }
    map->total_files = 0;

    pthread_mutex_unlock(&map->lock);
    pthread_mutex_destroy(&map->lock);
}

// ==================== USER MANAGEMENT ====================

int add_user(const char *username)
{
    pthread_mutex_lock(&nm_state.user_lock);
    int added = 0;

    for (int i = 0; i < nm_state.user_count; i++)
    {
        if (strcmp(nm_state.users[i], username) == 0)
        {
            pthread_mutex_unlock(&nm_state.user_lock);
            return 0;
        }
    }

    if (nm_state.user_count < MAX_USERS)
    {
        strncpy(nm_state.users[nm_state.user_count], username, MAX_USERNAME - 1);
        nm_state.users[nm_state.user_count][MAX_USERNAME - 1] = '\0';
        nm_state.user_count++;
        added = 1;
    }

    pthread_mutex_unlock(&nm_state.user_lock);
    return added;
}

int user_exists(const char *username)
{
    pthread_mutex_lock(&nm_state.user_lock);

    for (int i = 0; i < nm_state.user_count; i++)
    {
        if (strcmp(nm_state.users[i], username) == 0)
        {
            pthread_mutex_unlock(&nm_state.user_lock);
            return 1;
        }
    }

    pthread_mutex_unlock(&nm_state.user_lock);
    return 0;
}

int get_all_users(char users[][MAX_USERNAME], int max_users)
{
    pthread_mutex_lock(&nm_state.user_lock);

    int count = nm_state.user_count < max_users ? nm_state.user_count : max_users;
    for (int i = 0; i < count; i++)
    {
        strncpy(users[i], nm_state.users[i], MAX_USERNAME);
    }

    pthread_mutex_unlock(&nm_state.user_lock);
    return count;
}

// ==================== CLIENT MANAGEMENT ====================

int register_client(int sockfd, const char *username, const char *ip)
{
    pthread_mutex_lock(&nm_state.client_lock);

    if (nm_state.client_count >= MAX_USERS)
    {
        pthread_mutex_unlock(&nm_state.client_lock);
        return -1;
    }

    ClientInfo *client = &nm_state.clients[nm_state.client_count];
    client->sockfd = sockfd;
    strncpy(client->username, username, MAX_USERNAME - 1);
    strncpy(client->ip, ip, INET_ADDRSTRLEN - 1);
    client->active = 1;
    client->in_write_mode = 0;
    client->current_write_file[0] = '\0';
    client->current_write_sentence = -1;

    nm_state.client_count++;

    pthread_mutex_unlock(&nm_state.client_lock);

    if (add_user(username))
    {
        persist_metadata();
    }
    return 0;
}

ClientInfo *find_client_by_socket(int sockfd)
{
    pthread_mutex_lock(&nm_state.client_lock);

    for (int i = 0; i < nm_state.client_count; i++)
    {
        if (nm_state.clients[i].sockfd == sockfd && nm_state.clients[i].active)
        {
            pthread_mutex_unlock(&nm_state.client_lock);
            return &nm_state.clients[i];
        }
    }

    pthread_mutex_unlock(&nm_state.client_lock);
    return NULL;
}

ClientInfo *find_client_by_username(const char *username)
{
    pthread_mutex_lock(&nm_state.client_lock);

    for (int i = 0; i < nm_state.client_count; i++)
    {
        if (strcmp(nm_state.clients[i].username, username) == 0 &&
            nm_state.clients[i].active)
        {
            pthread_mutex_unlock(&nm_state.client_lock);
            return &nm_state.clients[i];
        }
    }

    pthread_mutex_unlock(&nm_state.client_lock);
    return NULL;
}

void remove_client(int sockfd)
{
    pthread_mutex_lock(&nm_state.client_lock);

    for (int i = 0; i < nm_state.client_count; i++)
    {
        if (nm_state.clients[i].sockfd == sockfd)
        {
            nm_state.clients[i].active = 0;
            const char *username = nm_state.clients[i].username;

            // Release any locks held
            pthread_mutex_lock(&nm_state.file_map.lock);
            for (int j = 0; j < HASH_TABLE_SIZE; j++)
            {
                FileNode *current = nm_state.file_map.buckets[j];
                while (current)
                {
                    if (current->metadata.is_locked &&
                        strcmp(current->metadata.locked_by, username) == 0)
                    {
                        current->metadata.is_locked = 0;
                        current->metadata.locked_sentence = -1;
                        current->metadata.locked_by[0] = '\0';
                    }
                    current = current->next;
                }
            }
            pthread_mutex_unlock(&nm_state.file_map.lock);

            break;
        }
    }

    pthread_mutex_unlock(&nm_state.client_lock);
}

// ==================== STORAGE SERVER MANAGEMENT ====================

int register_storage_server(int sockfd, const char *ip, int nm_port, int client_port)
{
    (void)sockfd;

    pthread_mutex_lock(&nm_state.ss_lock);

    if (nm_state.storage_count >= MAX_STORAGE_SERVERS)
    {
        pthread_mutex_unlock(&nm_state.ss_lock);
        return -1;
    }

    StorageServerInfo *ss = &nm_state.storage_servers[nm_state.storage_count];
    ss->sockfd = -1;
    strncpy(ss->ip, ip, INET_ADDRSTRLEN - 1);
    ss->nm_port = nm_port;
    ss->client_port = client_port;
    ss->active = 1;
    ss->file_count = 0;

    int index = nm_state.storage_count;
    nm_state.storage_count++;

    pthread_mutex_unlock(&nm_state.ss_lock);
    return index;
}

void add_files_to_ss(int ss_index, char files[][MAX_FILENAME], int file_count)
{
    pthread_mutex_lock(&nm_state.ss_lock);

    if (ss_index >= 0 && ss_index < nm_state.storage_count)
    {
        StorageServerInfo *ss = &nm_state.storage_servers[ss_index];

        for (int i = 0; i < file_count && ss->file_count < MAX_FILES; i++)
        {
            strncpy(ss->files[ss->file_count], files[i], MAX_FILENAME - 1);
            ss->file_count++;
        }
    }

    pthread_mutex_unlock(&nm_state.ss_lock);
}

int find_least_loaded_ss()
{
    pthread_mutex_lock(&nm_state.ss_lock);

    int best = -1;
    int min_files = MAX_FILES + 1;

    for (int i = 0; i < nm_state.storage_count; i++)
    {
        if (nm_state.storage_servers[i].active &&
            nm_state.storage_servers[i].file_count < min_files)
        {
            min_files = nm_state.storage_servers[i].file_count;
            best = i;
        }
    }

    pthread_mutex_unlock(&nm_state.ss_lock);
    return best;
}

void deactivate_storage_server(int ss_index)
{
    pthread_mutex_lock(&nm_state.ss_lock);

    if (ss_index >= 0 && ss_index < nm_state.storage_count)
    {
        nm_state.storage_servers[ss_index].active = 0;
    }

    pthread_mutex_unlock(&nm_state.ss_lock);
}

StorageServerInfo *get_storage_server(int ss_index)
{
    pthread_mutex_lock(&nm_state.ss_lock);

    if (ss_index >= 0 && ss_index < nm_state.storage_count &&
        nm_state.storage_servers[ss_index].active)
    {
        pthread_mutex_unlock(&nm_state.ss_lock);
        return &nm_state.storage_servers[ss_index];
    }

    pthread_mutex_unlock(&nm_state.ss_lock);
    return NULL;
}

// ==================== FILE METADATA REFRESH ====================

int refresh_file_metadata(FileMetadata *file)
{
    if (!file || file->ss_index < 0)
    {
        return -1;
    }

    StorageServerInfo *ss = get_storage_server(file->ss_index);
    if (!ss)
    {
        return -1;
    }

    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_sock < 0)
    {
        return -1;
    }

    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss->client_port);
    ss_addr.sin_addr.s_addr = inet_addr(ss->ip);

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(ss_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(ss_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0)
    {
        close(ss_sock);
        return -1;
    }

    char cmd[BUFFER_SIZE];
    snprintf(cmd, sizeof(cmd), "READ|%s\n", file->filename);
    if (send(ss_sock, cmd, strlen(cmd), 0) < 0)
    {
        close(ss_sock);
        return -1;
    }

    char recv_buf[BUFFER_SIZE];
    char content[BUFFER_SIZE * 4];
    size_t content_len = 0;
    content[0] = '\0';
    int status = 0;

    while (1)
    {
        ssize_t n = recv(ss_sock, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n <= 0)
        {
            status = -1;
            break;
        }

        recv_buf[n] = '\0';

        char *stop_pos = strstr(recv_buf, "--STOP--");
        if (stop_pos)
        {
            *stop_pos = '\0';
        }

        size_t copy_len = strlen(recv_buf);
        if (content_len + copy_len < sizeof(content) - 1)
        {
            memcpy(content + content_len, recv_buf, copy_len);
            content_len += copy_len;
            content[content_len] = '\0';
        }

        if (stop_pos)
        {
            break;
        }
    }

    close(ss_sock);

    if (status != 0)
    {
        return -1;
    }

    // Update file size in bytes (including spaces, excluding any STOP marker).
    file->file_size = content_len;

    int word_count = 0;
    int char_count = 0;
    int in_word = 0;

    for (size_t i = 0; i < content_len; i++)
    {
        unsigned char c = (unsigned char)content[i];
        if (!isspace(c))
        {
            char_count++;
            if (!in_word)
            {
                word_count++;
                in_word = 1;
            }
        }
        else
        {
            in_word = 0;
        }
    }

    file->word_count = word_count;
    file->char_count = char_count;

    return 0;
}

// ==================== ACCESS CONTROL ====================

AccessLevel check_access(const char *filename, const char *username)
{
    FileMetadata *file = find_file(&nm_state.file_map, filename);
    if (!file)
    {
        return ACCESS_NONE;
    }

    if (strcmp(file->owner, username) == 0)
    {
        return ACCESS_OWNER;
    }

    for (int i = 0; i < file->writer_count; i++)
    {
        if (strcmp(file->writers[i], username) == 0)
        {
            return ACCESS_WRITE;
        }
    }

    for (int i = 0; i < file->reader_count; i++)
    {
        if (strcmp(file->readers[i], username) == 0)
        {
            return ACCESS_READ;
        }
    }

    return ACCESS_NONE;
}

int add_read_access(const char *filename, const char *username)
{
    FileMetadata *file = find_file(&nm_state.file_map, filename);
    if (!file)
    {
        return ERR_FILE_NOT_FOUND;
    }

    // Already has write?
    for (int i = 0; i < file->writer_count; i++)
    {
        if (strcmp(file->writers[i], username) == 0)
        {
            return ERR_SUCCESS;
        }
    }

    for (int i = 0; i < file->reader_count; i++)
    {
        if (strcmp(file->readers[i], username) == 0)
        {
            return ERR_SUCCESS;
        }
    }

    if (file->reader_count < MAX_ACCESS_USERS)
    {
        strncpy(file->readers[file->reader_count], username, MAX_USERNAME - 1);
        file->reader_count++;
        return ERR_SUCCESS;
    }

    return ERR_INTERNAL_ERROR;
}

int add_write_access(const char *filename, const char *username)
{
    FileMetadata *file = find_file(&nm_state.file_map, filename);
    if (!file)
    {
        return ERR_FILE_NOT_FOUND;
    }

    for (int i = 0; i < file->writer_count; i++)
    {
        if (strcmp(file->writers[i], username) == 0)
        {
            return ERR_SUCCESS;
        }
    }

    // Remove from readers
    for (int i = 0; i < file->reader_count; i++)
    {
        if (strcmp(file->readers[i], username) == 0)
        {
            for (int j = i; j < file->reader_count - 1; j++)
            {
                strcpy(file->readers[j], file->readers[j + 1]);
            }
            file->reader_count--;
            break;
        }
    }

    // Add write
    if (file->writer_count < MAX_ACCESS_USERS)
    {
        strncpy(file->writers[file->writer_count], username, MAX_USERNAME - 1);
        file->writer_count++;
        return ERR_SUCCESS;
    }

    return ERR_INTERNAL_ERROR;
}

int remove_access(const char *filename, const char *username)
{
    FileMetadata *file = find_file(&nm_state.file_map, filename);
    if (!file)
    {
        return ERR_FILE_NOT_FOUND;
    }

    if (strcmp(file->owner, username) == 0)
    {
        return ERR_PERMISSION_DENIED;
    }

    int removed = 0;

    // Writers
    for (int i = 0; i < file->writer_count; i++)
    {
        if (strcmp(file->writers[i], username) == 0)
        {
            for (int j = i; j < file->writer_count - 1; j++)
            {
                strcpy(file->writers[j], file->writers[j + 1]);
            }
            file->writer_count--;
            removed = 1;
            break;
        }
    }

    // Readers
    for (int i = 0; i < file->reader_count; i++)
    {
        if (strcmp(file->readers[i], username) == 0)
        {
            for (int j = i; j < file->reader_count - 1; j++)
            {
                strcpy(file->readers[j], file->readers[j + 1]);
            }
            file->reader_count--;
            removed = 1;
            break;
        }
    }

    if (removed)
    {
      return ERR_SUCCESS;
    }
    else
    {
      return ERR_USER_NOT_FOUND;
    }

}

// ==================== LOGGING ====================

void init_logging(const char *log_filename)
{
    pthread_mutex_init(&nm_state.log_lock, NULL);

    nm_state.log_file = fopen(log_filename, "a");
    if (!nm_state.log_file)
    {
        return;
    }

    char ts[64];
    get_timestamp(ts, sizeof(ts));
    fprintf(nm_state.log_file, "\n===== Name Server Started at %s =====\n", ts);
    fflush(nm_state.log_file);
}

void log_message(const char *level, const char *source_ip, int source_port,
                 const char *username, const char *message)
{
    if (!nm_state.log_file)
    {
        return;
    }

    pthread_mutex_lock(&nm_state.log_lock);

    char ts[64];
    get_timestamp(ts, sizeof(ts));

    fprintf(nm_state.log_file, "[%s] [%s] ", ts, level);

    if (source_ip && source_port > 0)
    {
        fprintf(nm_state.log_file, "[%s:%d] ", source_ip, source_port);
    }

    if (username && username[0])
    {
        fprintf(nm_state.log_file, "[User: %s] ", username);
    }

    fprintf(nm_state.log_file, "%s\n", message);
    fflush(nm_state.log_file);

    pthread_mutex_unlock(&nm_state.log_lock);
}

void log_request(const char *username, const char *command, const char *details)
{
    char buf[BUFFER_SIZE];
    snprintf(buf, sizeof(buf), "REQUEST: %s %s", command, details ? details : "");
    log_message("INFO", NULL, 0, username, buf);
}

void log_response(const char *username, ErrorCode error, const char *details)
{
    char buf[BUFFER_SIZE];

    if (error == ERR_SUCCESS)
    {
        snprintf(buf, sizeof(buf), "RESPONSE: SUCCESS %s", details ? details : "");
        log_message("INFO", NULL, 0, username, buf);
    }
    else
    {
        snprintf(buf, sizeof(buf), "RESPONSE: ERROR[%d] %s %s",
                 error, get_error_message(error), details ? details : "");
        log_message("ERROR", NULL, 0, username, buf);
    }
}

void close_logging()
{
    if (nm_state.log_file)
    {
        char ts[64];
        get_timestamp(ts, sizeof(ts));
        fprintf(nm_state.log_file, "===== Name Server Stopped at %s =====\n\n", ts);
        fclose(nm_state.log_file);
    }

    pthread_mutex_destroy(&nm_state.log_lock);
}

void persist_metadata()
{
    FILE *fp = fopen(NM_METADATA_FILE, "w");
    if (!fp)
    {
        perror("persist_metadata");
        return;
    }

    pthread_mutex_lock(&nm_state.file_map.lock);
    fprintf(fp, "FILES %d\n", nm_state.file_map.total_files);
    for (int i = 0; i < HASH_TABLE_SIZE; i++)
    {
        FileNode *current = nm_state.file_map.buckets[i];
        while (current)
        {
            FileMetadata *meta = &current->metadata;
            fprintf(fp, "FILE %s\n", meta->filename);
            fprintf(fp, "OWNER %s\n", meta->owner);
            fprintf(fp, "SS_INDEX %d\n", meta->ss_index);
            fprintf(fp, "CREATED %ld\n", (long)meta->created_time);
            fprintf(fp, "LAST_MOD %ld\n", (long)meta->last_modified);
            fprintf(fp, "LAST_ACC %ld\n", (long)meta->last_accessed);
            fprintf(fp, "LAST_ACC_BY %s\n", meta->last_accessed_by);
            fprintf(fp, "FILE_SIZE %zu\n", meta->file_size);
            fprintf(fp, "WORD_COUNT %d\n", meta->word_count);
            fprintf(fp, "CHAR_COUNT %d\n", meta->char_count);
            for (int w = 0; w < meta->writer_count; w++)
            {
                fprintf(fp, "WRITER %s\n", meta->writers[w]);
            }
            for (int r = 0; r < meta->reader_count; r++)
            {
                fprintf(fp, "READER %s\n", meta->readers[r]);
            }
            fprintf(fp, "FILE_END\n");
            current = current->next;
        }
    }
    pthread_mutex_unlock(&nm_state.file_map.lock);

    pthread_mutex_lock(&nm_state.user_lock);
    fprintf(fp, "USERS %d\n", nm_state.user_count);
    for (int i = 0; i < nm_state.user_count; i++)
    {
        fprintf(fp, "USER %s\n", nm_state.users[i]);
    }
    pthread_mutex_unlock(&nm_state.user_lock);

    fclose(fp);
}

static void finalize_loaded_file(FileMetadata *meta)
{
    if (!meta || meta->filename[0] == '\0')
    {
        return;
    }
    meta->locked_sentence = -1;
    meta->locked_by[0] = '\0';
    meta->is_locked = 0;
    insert_file(&nm_state.file_map, meta);
}

void load_metadata_from_disk()
{
    FILE *fp = fopen(NM_METADATA_FILE, "r");
    if (!fp)
    {
        return;
    }

    char line[BUFFER_SIZE];
    FileMetadata meta = {0};
    int in_file_block = 0;

    while (fgets(line, sizeof(line), fp))
    {
        trim_string(line);
        if (line[0] == '\0')
        {
            continue;
        }

        if (strncmp(line, "FILE ", 5) == 0)
        {
            if (in_file_block)
            {
                finalize_loaded_file(&meta);
            }
            memset(&meta, 0, sizeof(meta));
            strncpy(meta.filename, line + 5, MAX_FILENAME - 1);
            meta.ss_index = -1;
            in_file_block = 1;
            continue;
        }

        if (in_file_block)
        {
            if (strcmp(line, "FILE_END") == 0)
            {
                finalize_loaded_file(&meta);
                in_file_block = 0;
                continue;
            }
            else if (strncmp(line, "OWNER ", 6) == 0)
            {
                strncpy(meta.owner, line + 6, MAX_USERNAME - 1);
            }
            else if (strncmp(line, "SS_INDEX ", 9) == 0)
            {
                meta.ss_index = atoi(line + 9);
            }
            else if (strncmp(line, "CREATED ", 8) == 0)
            {
                meta.created_time = (time_t)atoll(line + 8);
            }
            else if (strncmp(line, "LAST_MOD ", 9) == 0)
            {
                meta.last_modified = (time_t)atoll(line + 9);
            }
            else if (strncmp(line, "LAST_ACC ", 9) == 0)
            {
                meta.last_accessed = (time_t)atoll(line + 9);
            }
            else if (strncmp(line, "LAST_ACC_BY ", 12) == 0)
            {
                strncpy(meta.last_accessed_by, line + 12, MAX_USERNAME - 1);
            }
            else if (strncmp(line, "FILE_SIZE ", 10) == 0)
            {
                meta.file_size = (size_t)strtoull(line + 10, NULL, 10);
            }
            else if (strncmp(line, "WORD_COUNT ", 11) == 0)
            {
                meta.word_count = atoi(line + 11);
            }
            else if (strncmp(line, "CHAR_COUNT ", 11) == 0)
            {
                meta.char_count = atoi(line + 11);
            }
            else if (strncmp(line, "WRITER ", 7) == 0)
            {
                if (meta.writer_count < MAX_ACCESS_USERS)
                {
                    strncpy(meta.writers[meta.writer_count], line + 7, MAX_USERNAME - 1);
                    meta.writers[meta.writer_count][MAX_USERNAME - 1] = '\0';
                    meta.writer_count++;
                }
            }
            else if (strncmp(line, "READER ", 7) == 0)
            {
                if (meta.reader_count < MAX_ACCESS_USERS)
                {
                    strncpy(meta.readers[meta.reader_count], line + 7, MAX_USERNAME - 1);
                    meta.readers[meta.reader_count][MAX_USERNAME - 1] = '\0';
                    meta.reader_count++;
                }
            }
            continue;
        }

        if (strncmp(line, "USERS ", 6) == 0)
        {
            int total = atoi(line + 6);
            for (int i = 0; i < total; i++)
            {
                if (!fgets(line, sizeof(line), fp))
                {
                    break;
                }
                trim_string(line);
                if (strncmp(line, "USER ", 5) == 0)
                {
                    add_user(line + 5);
                }
                else if (line[0] != '\0')
                {
                    add_user(line);
                }
            }
        }
    }

    if (in_file_block)
    {
        finalize_loaded_file(&meta);
    }

    fclose(fp);
}

// ==================== NAME SERVER INIT / CLEANUP ====================

StorageServerInfo *get_available_storage_server()
{
    // choose the least-loaded active storage server so that newly
    // created files are distributed across all available servers.
    int index = find_least_loaded_ss();
    if (index < 0)
    {
        return NULL;
    }
    return get_storage_server(index);
}

void remove_storage_server(int index)
{
    pthread_mutex_lock(&nm_state.ss_lock);

    if (index >= 0 && index < nm_state.storage_count)
    {
        nm_state.storage_servers[index].active = 0;
        if (nm_state.storage_servers[index].sockfd >= 0)
        {
            close(nm_state.storage_servers[index].sockfd);
            nm_state.storage_servers[index].sockfd = -1;
        }
    }

    pthread_mutex_unlock(&nm_state.ss_lock);
}

void init_name_server()
{
    init_file_hashmap(&nm_state.file_map);

    nm_state.storage_count = 0;
    pthread_mutex_init(&nm_state.ss_lock, NULL);

    nm_state.client_count = 0;
    pthread_mutex_init(&nm_state.client_lock, NULL);

    nm_state.user_count = 0;
    pthread_mutex_init(&nm_state.user_lock, NULL);

    init_logging("nm_server.log");
    log_message("INFO", NULL, 0, NULL, "Name Server initialized");

    load_metadata_from_disk();
}

void cleanup_name_server()
{
    log_message("INFO", NULL, 0, NULL, "Name Server shutting down");

    free_file_hashmap(&nm_state.file_map);

    pthread_mutex_lock(&nm_state.client_lock);
    for (int i = 0; i < nm_state.client_count; i++)
    {
        if (nm_state.clients[i].active)
        {
            close(nm_state.clients[i].sockfd);
        }
    }
    pthread_mutex_unlock(&nm_state.client_lock);

    pthread_mutex_lock(&nm_state.ss_lock);
    for (int i = 0; i < nm_state.storage_count; i++)
    {
        if (nm_state.storage_servers[i].active &&
            nm_state.storage_servers[i].sockfd >= 0)
        {
            close(nm_state.storage_servers[i].sockfd);
            nm_state.storage_servers[i].sockfd = -1;
        }
    }
    pthread_mutex_unlock(&nm_state.ss_lock);

    close_logging();

    pthread_mutex_destroy(&nm_state.ss_lock);
    pthread_mutex_destroy(&nm_state.client_lock);
    pthread_mutex_destroy(&nm_state.user_lock);
}

void increment_ss_file_count(int ss_index)
{
    pthread_mutex_lock(&nm_state.ss_lock);

    if (ss_index >= 0 && ss_index < nm_state.storage_count)
    {
        nm_state.storage_servers[ss_index].file_count++;
        
        char log_buf[128];
        StorageServerInfo *ss = &nm_state.storage_servers[ss_index];

        snprintf(log_buf, sizeof(log_buf), "Load updated: SS %d now has %d files.", ss_index, ss->file_count);
        
        log_message("INFO", ss->ip, ss->nm_port, "SYSTEM", log_buf); 
    }

    pthread_mutex_unlock(&nm_state.ss_lock);
}
