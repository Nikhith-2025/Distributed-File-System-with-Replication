#include "nm_metadata.h"

NameServerState nm_state;

// ==================== HASHMAP IMPLEMENTATION ====================

unsigned int hash_filename(const char *filename) 
{
    unsigned int hash = 5381;
    int c;
    while ((c = *filename++)) 
    {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
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

int insert_file(FileHashMap *map, FileMetadata *metadata) {
    pthread_mutex_lock(&map->lock);
    
    unsigned int index = hash_filename(metadata->filename);
    
    // Check if file already exists
    FileNode *current = map->buckets[index];
    while (current != NULL) {
        if (strcmp(current->metadata.filename, metadata->filename) == 0) 
        {
            pthread_mutex_unlock(&map->lock);
            return -1;  // File already exists
        }
        current = current->next;
    }
    
    // Create new node
    FileNode *new_node = (FileNode*)malloc(sizeof(FileNode));
    if (!new_node) {
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

FileMetadata* find_file(FileHashMap *map, const char *filename) 
{
    pthread_mutex_lock(&map->lock);
    
    unsigned int index = hash_filename(filename);
    FileNode *current = map->buckets[index];
    
    while (current != NULL) {
        if (strcmp(current->metadata.filename, filename) == 0) {
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
    
    while (current != NULL) 
    {
        if (strcmp(current->metadata.filename, filename) == 0) 
        {
            if (prev == NULL) 
            {
                map->buckets[index] = current->next;
            } else {
                prev->next = current->next;
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
    
    FileMetadata **files = (FileMetadata**)malloc(sizeof(FileMetadata*) * map->total_files);
    int count = 0;
    
    for (int i = 0; i < HASH_TABLE_SIZE; i++) 
    {
        FileNode *current = map->buckets[i];
        while (current != NULL) 
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
        while (current != NULL) {
            FileNode *temp = current;
            current = current->next;
            free(temp);
        }
        map->buckets[i] = NULL;
    }
    map->total_files = 0;
    
    pthread_mutex_unlock(&map->lock);
    pthread_mutex_destroy(&map->lock);
    // destroy the lock cause we are not going to use it anymore
}

// ==================== USER MANAGEMENT ====================

void add_user(const char *username) 
{
    pthread_mutex_lock(&nm_state.user_lock);
    
    // Check if user already exists
    for (int i = 0; i < nm_state.user_count; i++) 
    {
        if (strcmp(nm_state.users[i], username) == 0) 
        {
            pthread_mutex_unlock(&nm_state.user_lock);
            return;
        }
    }
    
    // Add new user if the user doesnt exist
    if (nm_state.user_count < MAX_USERS) 
    {
        strncpy(nm_state.users[nm_state.user_count], username, MAX_USERNAME - 1);
        nm_state.users[nm_state.user_count][MAX_USERNAME - 1] = '\0';
        nm_state.user_count++;
    }
    
    pthread_mutex_unlock(&nm_state.user_lock);
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
    int count;
    if (nm_state.user_count < max_users) 
    {
    count = nm_state.user_count;
    } 
    else 
    {
    count = max_users;
    }
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
    
    if (nm_state.client_count >= MAX_USERS) {
        pthread_mutex_unlock(&nm_state.client_lock);
        return -1;
    }
    
    ClientInfo *client = &nm_state.clients[nm_state.client_count];
    client->sockfd = sockfd;
    strncpy(client->username, username, MAX_USERNAME - 1);
    client->username[MAX_USERNAME - 1] = '\0';
    strncpy(client->ip, ip, INET_ADDRSTRLEN - 1);
    client->ip[INET_ADDRSTRLEN - 1] = '\0';
    client->active = 1;
    client->in_write_mode = 0;
    client->current_write_file[0] = '\0';
    client->current_write_sentence = -1;
    
    nm_state.client_count++;
    
    pthread_mutex_unlock(&nm_state.client_lock);
    add_user(username);
    
    return 0;
}

ClientInfo* find_client_by_socket(int sockfd) 
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

ClientInfo* find_client_by_username(const char *username) 
{
    pthread_mutex_lock(&nm_state.client_lock);
    
    for (int i = 0; i < nm_state.client_count; i++) 
    {
        if (strcmp(nm_state.clients[i].username, username) == 0 && 
            nm_state.clients[i].active) {
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
            pthread_mutex_lock(&nm_state.file_map.lock);
            for (int j = 0; j < HASH_TABLE_SIZE; j++) 
            {
                FileNode *current = nm_state.file_map.buckets[j];
                while (current != NULL) 
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

// ==================== STORAGE SERVER ====================

int register_storage_server(int sockfd, const char *ip, int nm_port, int client_port) 
{
    pthread_mutex_lock(&nm_state.ss_lock);
    
    if (nm_state.storage_count >= MAX_STORAGE_SERVERS) 
    {
        pthread_mutex_unlock(&nm_state.ss_lock);
        return -1;
    }
    
    StorageServerInfo *ss = &nm_state.storage_servers[nm_state.storage_count];
    ss->sockfd = sockfd;
    strncpy(ss->ip, ip, INET_ADDRSTRLEN - 1);
    ss->ip[INET_ADDRSTRLEN - 1] = '\0';
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
    
    if (ss_index < 0 || ss_index >= nm_state.storage_count) 
    {
        pthread_mutex_unlock(&nm_state.ss_lock);
        return;
    }
    
    StorageServerInfo *ss = &nm_state.storage_servers[ss_index];
    
    for (int i = 0; i < file_count && ss->file_count < MAX_FILES; i++) 
    {
        strncpy(ss->files[ss->file_count], files[i], MAX_FILENAME - 1);
        ss->files[ss->file_count][MAX_FILENAME - 1] = '\0';
        ss->file_count++;
    }
    
    pthread_mutex_unlock(&nm_state.ss_lock);
}

int find_least_loaded_ss() 
{
    pthread_mutex_lock(&nm_state.ss_lock);
    
    int min_files = MAX_FILES + 1;
    int best_index = -1;
    
    for (int i = 0; i < nm_state.storage_count; i++) 
    {
        if (nm_state.storage_servers[i].active && 
            nm_state.storage_servers[i].file_count < min_files) 
        {
            min_files = nm_state.storage_servers[i].file_count;
            best_index = i;
        }
    }
    
    pthread_mutex_unlock(&nm_state.ss_lock);
    return best_index;
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

StorageServerInfo* get_storage_server(int ss_index) 
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

// ==================== ACCESS DETAILS ========================

AccessLevel check_access(const char *filename, const char *username) 
{
    FileMetadata *file = find_file(&nm_state.file_map, filename);
    if (!file) 
    {
        return ACCESS_NONE;
    }
    
    // owner has every access
    if (strcmp(file->owner, username) == 0) 
    {
        return ACCESS_OWNER;
    }
    
    // Check write access
    for (int i = 0; i < file->writer_count; i++) 
    {
        // check if the username is present in the writers list
        if (strcmp(file->writers[i], username) == 0) 
        {
            return ACCESS_WRITE;
        }
    }
    
    // Check read access
    for (int i = 0; i < file->reader_count; i++) 
    {
        // check if the username is present in the readers list
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
    
    // Check if already has write access
    for (int i = 0; i < file->writer_count; i++) 
    {
        if (strcmp(file->writers[i], username) == 0) 
        {
            return ERR_SUCCESS;  // write access includes read access
        }
    }
    
    // Check if already has read access
    for (int i = 0; i < file->reader_count; i++) 
    {
        if (strcmp(file->readers[i], username) == 0) 
        {
            return ERR_SUCCESS;  // Already has read access
        }
    }
    
    // Add read access
    if (file->reader_count < MAX_ACCESS_USERS) 
    {
        strncpy(file->readers[file->reader_count], username, MAX_USERNAME - 1);
        file->readers[file->reader_count][MAX_USERNAME - 1] = '\0';
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
    
    // Check if already has write access
    for (int i = 0; i < file->writer_count; i++) 
    {
        if (strcmp(file->writers[i], username) == 0) 
        {
            return ERR_SUCCESS;
        }
    }
    
    // Remove from readers if present just to reduce redundancy
    for (int i = 0; i < file->reader_count; i++) 
    {
        if (strcmp(file->readers[i], username) == 0) {
            // Shift elements
            for (int j = i; j < file->reader_count - 1; j++) 
            {
                strcpy(file->readers[j], file->readers[j + 1]);
            }
            file->reader_count--;
            break;
        }
    }
    
    // Add write access
    if (file->writer_count < MAX_ACCESS_USERS) 
    {
        strncpy(file->writers[file->writer_count], username, MAX_USERNAME - 1);
        file->writers[file->writer_count][MAX_USERNAME - 1] = '\0';
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

    // no one can remove owner's access
    if (strcmp(file->owner, username) == 0) 
    {
        return ERR_PERMISSION_DENIED;
    }
    
    int removed = 0;
    
    // Remove from writers
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
        fprintf(stderr, "[ERROR] Failed to open log file: %s\n", log_filename);
        return;
    }
    
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    fprintf(nm_state.log_file, "\n========== Name Server Started at %s ==========\n", timestamp);
    fflush(nm_state.log_file);
}
// here level means the message can be information or error
void log_message(const char *level, const char *source_ip, int source_port, 
                 const char *username, const char *message) 
{
    if (!nm_state.log_file) 
    {
        return;
    }
    
    pthread_mutex_lock(&nm_state.log_lock);
    
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    
    fprintf(nm_state.log_file, "[%s] [%s] ", timestamp, level);
    
    if (source_ip && source_port > 0) 
    {
        fprintf(nm_state.log_file, "[%s:%d] ", source_ip, source_port);
    }
    
    if (username && username[0] != '\0') 
    {
        fprintf(nm_state.log_file, "[User: %s] ", username);
    }
    
    fprintf(nm_state.log_file, "%s\n", message);
    fflush(nm_state.log_file);
    
    pthread_mutex_unlock(&nm_state.log_lock);
}

void log_request(const char *username, const char *command, const char *details) 
{
    char log_buf[BUFFER_SIZE];
    snprintf(log_buf, sizeof(log_buf), "REQUEST: %s %s", command, details ? details : "");
    log_message("INFO", NULL, 0, username, log_buf);
}

void log_response(const char *username, ErrorCode error, const char *details) 
{
    char log_buf[BUFFER_SIZE];
    if (error == ERR_SUCCESS) 
    {
        snprintf(log_buf, sizeof(log_buf), "RESPONSE: SUCCESS %s", details ? details : "");
        log_message("INFO", NULL, 0, username, log_buf);
    } 
    else 
    {
        snprintf(log_buf, sizeof(log_buf), "RESPONSE: ERROR[%d] %s %s", 
                 error, get_error_message(error), details ? details : "");
        log_message("ERROR", NULL, 0, username, log_buf);
    }
}

void close_logging() 
{
    if (nm_state.log_file) {
        char timestamp[64];
        get_timestamp(timestamp, sizeof(timestamp));
        fprintf(nm_state.log_file, "========== Name Server Stopped at %s ==========\n\n", timestamp);
        fclose(nm_state.log_file);
        nm_state.log_file = NULL;
    }
    pthread_mutex_destroy(&nm_state.log_lock);
}

// ==================== NAME SERVER INITIALIZATION ====================

void init_name_server() 
{
    // initialized the locks along with the variables
    // Initialize file hashmap
    init_file_hashmap(&nm_state.file_map);
    nm_state.storage_count = 0;
    pthread_mutex_init(&nm_state.ss_lock, NULL);
    nm_state.client_count = 0;
    pthread_mutex_init(&nm_state.client_lock, NULL);
    nm_state.user_count = 0;
    pthread_mutex_init(&nm_state.user_lock, NULL);
    
    init_logging("nm_server.log");
    
    log_message("INFO", NULL, 0, NULL, "Name Server initialized successfully");
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
        if (nm_state.storage_servers[i].active) 
        {
            close(nm_state.storage_servers[i].sockfd);
        }
    }
    pthread_mutex_unlock(&nm_state.ss_lock);
    
    close_logging();
    pthread_mutex_destroy(&nm_state.ss_lock);
    pthread_mutex_destroy(&nm_state.client_lock);
    pthread_mutex_destroy(&nm_state.user_lock);
}
