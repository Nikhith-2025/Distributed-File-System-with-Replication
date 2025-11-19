#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/sendfile.h>

int SS_NM_PORT = 9000;      
int SS_CLIENT_PORT = 9001; 
char SS_STORAGE_DIR[128] = "storage/";

#define LOG_FILE "storage_server.log"
#define STOP_PACKET "\n--STOP--\n"
#define NM_RESP_BUFFER 4096

#define NM_IP "127.0.0.1"
#define NM_REGISTER_PORT 8000

static FILE* log_fp;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct Filelock {

    char filename[256];
    int sentence_index;
    pthread_t owner_thread_id; 
    struct Filelock* next;
} Filelock;

typedef struct stringnode{
    char* str;
    struct stringnode* next;
}Stringnode;

typedef struct modification {
    int word_index;
    char* content;
    struct modification* next;
} Modification;



void server_log(const char* level, const char* fmt, ...);
static int ensure_dirs_for_full_path(const char* full_path);
static int ensure_parent_dirs_for_relative(const char* relative_path);
static void send_simple_response(int sock, const char* msg);

Stringnode* get_node_at(Stringnode* head, int index) {

    for (int i = 0; head != NULL && i < index; i++) {
        head = head->next;
    }
    return head;
}

void free_list(Stringnode* head) {

    while (head) {

        Stringnode* temp = head;
        head = head->next;
        free(temp->str); 
        free(temp); 
    }
}

void free_modification_list(Modification* head) {
    while (head) {
        Modification* temp = head;
        head = head->next;
        free(temp->content);
        free(temp); 
    }
}

// Check if character is a sentence delimiter (. ! ?)
int is_sentence_delimiter(char c) 
{
    return (c == '.' || c == '!' || c == '?');
}

// Parser code for splitting file content into sentences
Stringnode* parse_into_sentences(const char* file_path) 
{
    FILE* fp = fopen(file_path, "r");
    if (!fp) 
    {
        return NULL;
    }
    Stringnode* head = NULL;
    Stringnode* tail = NULL;
    char buffer[4096];
    char current_sentence[4096] = "";
    while (fgets(buffer, sizeof(buffer), fp)) 
    {
        for (size_t i = 0; i < strlen(buffer); i++) 
        {
            strncat(current_sentence, &buffer[i], 1);
            
            // Check for the sentence delimiter
            if (is_sentence_delimiter(buffer[i])) 
            {
                // Create new sentence node
                Stringnode* new_node = (Stringnode*)malloc(sizeof(Stringnode));
                new_node->str = strdup(current_sentence);
                new_node->next = NULL;
                
                if (!head) 
                {
                    head = tail = new_node;
                } 
                else 
                {
                    tail->next = new_node;
                    tail = new_node;
                }
                
                current_sentence[0] = '\0';
            }
        }
    }
    
    // if no delimeter at the end
    if (strlen(current_sentence) > 0) 
    {
        Stringnode* new_node = (Stringnode*)malloc(sizeof(Stringnode));
        new_node->str = strdup(current_sentence);
        new_node->next = NULL;
        
        if (!head) 
        {
            head = tail = new_node;
        } 
        else 
        {
            tail->next = new_node;
        }
    }
    
    fclose(fp);
    return head;
}

// Count sentences in file
int count_sentences(Stringnode* head) 
{
    int count = 0;
    while (head) 
    {
        count++;
        head = head->next;
    }
    return count;
}

// Parsing sentence into words 
Stringnode* parse_sentence_into_words(const char* sentence) 
{
    Stringnode* head = NULL;
    Stringnode* tail = NULL;
    
    // make a modifiable copy and ensure null-termination
    char sent_copy[4096];
    strncpy(sent_copy, sentence, sizeof(sent_copy) - 1);
    sent_copy[sizeof(sent_copy) - 1] = '\0';
    
    char* word = strtok(sent_copy, " \t\n\r");
    while (word) 
    {
        Stringnode* new_node = (Stringnode*)malloc(sizeof(Stringnode));
        new_node->str = strdup(word);
        new_node->next = NULL;
        
        if (!head) 
        {
            head = tail = new_node;
        } 
        else 
        {
            tail->next = new_node;
            tail = new_node;
        }
        
        word = strtok(NULL, " \t\n\r");
    }
    
    return head;
}

char* replace_word_in_sentence(const char* sentence, int word_index, const char* new_word) 
{
    // preserve any leading whitespace (spaces/newlines) so that
    // sentence boundaries and formatting remain intact when we
    // write the updated content back to the file.
    char leading_ws[256];
    int prefix_len = 0;
    while (sentence[prefix_len] != '\0' && (sentence[prefix_len] == ' ' || sentence[prefix_len] == '\t' || sentence[prefix_len] == '\n' || sentence[prefix_len] == '\r'))
    {
        if (prefix_len < (int)sizeof(leading_ws) - 1)
        {
            leading_ws[prefix_len] = sentence[prefix_len];
        }
        prefix_len++;
    }

    leading_ws[(prefix_len < (int)sizeof(leading_ws) - 1) ? prefix_len : (int)sizeof(leading_ws) - 1] = '\0';

    const char* core_sentence = sentence + prefix_len;

    Stringnode* words = parse_sentence_into_words(core_sentence);
    
    // Count words
    int word_count = 0;
    Stringnode* temp = words;
    while (temp) 
    {
        word_count++;
        temp = temp->next;
    }

    // Allow insertion at the end (index == word_count)
    if (word_index < 0 || word_index > word_count) 
    {
        free_list(words);
        return NULL;
    }
    
    // Build new sentence
    char* result = (char*)malloc(4096);
    if (!result) 
    {
        free_list(words);
        return NULL;
    }
    result[0] = '\0';

    // start with original leading whitespace (if any)
    strcat(result, leading_ws);
    
    temp = words;
    int idx = 0;
    while (temp) 
    {
        if (idx == word_index) 
        {
            // Insert new word before the current word
            strcat(result, new_word);
            strcat(result, " ");
        }

        strcat(result, temp->str);
        if (temp->next) 
        {
            strcat(result, " ");
        }

        temp = temp->next;
        idx++;
    }

    // Append at end if index == word_count
    if (word_index == word_count)
    {
        if (word_count > 0)
        {
          strcat(result, " ");
        }

       strcat(result, new_word);
    }
    
    free_list(words);
    return result;
}

static Stringnode* create_sentence_node(const char* text)
{
    Stringnode* node = (Stringnode*)malloc(sizeof(Stringnode));
    if (!node)
    {
        return NULL;
    }
    node->str = strdup(text ? text : "");
    if (!node->str)
    {
        free(node);
        return NULL;
    }
    node->next = NULL;
    return node;
}

static void append_sentence_nodes(Stringnode* target, Stringnode* extras)
{
    if (!target || !extras)
    {
        return;
    }
    Stringnode* tail = extras;
    while (tail->next)
    {
        tail = tail->next;
    }
    tail->next = target->next;
    target->next = extras;
}

static void append_sentence_to_tail(Stringnode** head_ref, Stringnode* new_node)
{
    if (!head_ref || !new_node)
    {
        return;
    }
    if (!*head_ref)
    {
        *head_ref = new_node;
        return;
    }
    Stringnode* tail = *head_ref;
    while (tail->next)
    {
        tail = tail->next;
    }
    tail->next = new_node;
}

static int sentence_has_terminal_delimiter(const char* sentence_text)
{
    if (!sentence_text)
    {
        return 0;
    }
    int len = (int)strlen(sentence_text);
    while (len > 0 && (sentence_text[len - 1] == ' ' ||
                       sentence_text[len - 1] == '\n' ||
                       sentence_text[len - 1] == '\r' ||
                       sentence_text[len - 1] == '\t'))
    {
        len--;
    }
    if (len <= 0)
    {
        return 0;
    }
    return is_sentence_delimiter(sentence_text[len - 1]);
}

static int split_sentence_string(const char* sentence_text,
                                 char** first_sentence_out,
                                 Stringnode** extra_sentences_out)
{
    if (!sentence_text || !first_sentence_out || !extra_sentences_out)
    {
        return -1;
    }

    *first_sentence_out = NULL;
    *extra_sentences_out = NULL;

    char buffer[4096];
    buffer[0] = '\0';
    size_t buf_len = 0;
    int sentences_found = 0;

    for (size_t i = 0; sentence_text[i] != '\0'; i++)
    {
        if (buf_len < sizeof(buffer) - 1)
        {
            buffer[buf_len++] = sentence_text[i];
            buffer[buf_len] = '\0';
        }

        if (is_sentence_delimiter(sentence_text[i]))
        {
            if (buf_len == 0)
            {
                continue;
            }

            if (sentences_found == 0)
            {
                *first_sentence_out = strdup(buffer);
                if (!*first_sentence_out)
                {
                    return -1;
                }
            }
            else
            {
                Stringnode* node = create_sentence_node(buffer);
                if (!node)
                {
                    return -1;
                }

                node->next = NULL;
                if (!*extra_sentences_out)
                {
                    *extra_sentences_out = node;
                }
                else
                {
                    Stringnode* tail = *extra_sentences_out;
                    while (tail->next)
                    {
                        tail = tail->next;
                    }
                    tail->next = node;
                }
            }

            buffer[0] = '\0';
            buf_len = 0;
            sentences_found++;
        }
    }

    if (buf_len > 0 || sentences_found == 0)
    {
        if (sentences_found == 0)
        {
            *first_sentence_out = strdup(buffer);
            if (!*first_sentence_out)
            {
                return -1;
            }
        }
        else
        {
            Stringnode* node = create_sentence_node(buffer);
            if (!node)
            {
                return -1;
            }

            node->next = NULL;
            if (!*extra_sentences_out)
            {
                *extra_sentences_out = node;
            }
            else
            {
                Stringnode* tail = *extra_sentences_out;
                while (tail->next)
                {
                    tail = tail->next;
                }
                tail->next = node;
            }
        }
    }

    if (!*first_sentence_out)
    {
        *first_sentence_out = strdup("");
        if (!*first_sentence_out)
        {
            return -1;
        }
    }

    return 0;
}

// Write sentences back to file
int write_sentences_to_file(const char* file_path, Stringnode* sentences) 
{
    if (ensure_dirs_for_full_path(file_path) != 0)
    {
        server_log("WARN", "Failed to ensure directories for %s", file_path);
    }

    FILE* fp = fopen(file_path, "w");
    if (!fp) 
    {
        return -1;
    }
    
    while (sentences) 
    {
        fprintf(fp, "%s", sentences->str);
        sentences = sentences->next;
    }
    
    fclose(fp);
    return 0;
}

static Filelock* global_lock_list = NULL;

static pthread_mutex_t lock_list_mutex = PTHREAD_MUTEX_INITIALIZER;

// logging function
void server_log(const char* level, const char* fmt, ...){
    pthread_mutex_lock(&log_mutex);
    
    time_t now = time(NULL);
    char time_buf[100];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M%S", localtime(&now));

    char log_msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(log_msg, sizeof(log_msg), fmt, args);
    va_end(args);

    unsigned long thread_id = (unsigned long)pthread_self();

    //console
    printf("[%s] [%s] (Thread %lu) %s\n", time_buf, level, thread_id, log_msg);

    //file
    if (log_fp) {

        fprintf(log_fp, "[%s] [%s] (Thread %lu) %s\n", time_buf, level, thread_id, log_msg);
        fflush(log_fp);
    }

    pthread_mutex_unlock(&log_mutex);
}

#define LOG_INFO(...) server_log("INFO", __VA_ARGS__)
#define LOG_WARN(...) server_log("WARN", __VA_ARGS__)
#define LOG_ERROR(...) server_log("ERROR", __VA_ARGS__)

static void append_file_entry(char* buffer, int buffer_size, int* buffer_pos, int* first_file, const char* entry) {
    if (*first_file) {
        *first_file = 0;
    } else {
        if (*buffer_pos + 1 >= buffer_size) {
            LOG_WARN("File list buffer too small during scan.");
            return;
        }
        buffer[(*buffer_pos)++] = ',';
    }

    int len = strlen(entry);
    if (*buffer_pos + len >= buffer_size) {
        LOG_WARN("File list buffer too small during scan.");
        return;
    }
    strncpy(buffer + *buffer_pos, entry, len);
    *buffer_pos += len;
    buffer[*buffer_pos] = '\0';
}

static void scan_directory_recursive(const char* base_dir, const char* relative_path,
                                     char* buffer, int buffer_size, int* buffer_pos, int* first_file) {
    char actual_path[512];
    if (relative_path && relative_path[0] != '\0') {
        snprintf(actual_path, sizeof(actual_path), "%s%s", base_dir, relative_path);
    } else {
        snprintf(actual_path, sizeof(actual_path), "%s", base_dir);
    }

    DIR* d = opendir(actual_path);
    if (!d) {
        return;
    }

    struct dirent* dir;
    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
            continue;
        }

        char child_relative[512];
        if (relative_path && relative_path[0] != '\0') {
            snprintf(child_relative, sizeof(child_relative), "%s/%s", relative_path, dir->d_name);
        } else {
            snprintf(child_relative, sizeof(child_relative), "%s", dir->d_name);
        }

        char child_full[512];
        snprintf(child_full, sizeof(child_full), "%s%s", base_dir, child_relative);

        struct stat st;
        if (stat(child_full, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            scan_directory_recursive(base_dir, child_relative, buffer, buffer_size, buffer_pos, first_file);
        } else {
        if (strstr(dir->d_name, ".undo") != NULL) {
            continue;
        }
            append_file_entry(buffer, buffer_size, buffer_pos, first_file, child_relative);
        }
    }

    closedir(d);
}

int scan_storage_directory(char* file_list_buffer, int buffer_size){

    memset(file_list_buffer, 0, buffer_size); // clear the buffer

    DIR *d = opendir(SS_STORAGE_DIR);
    if (!d) {
        LOG_ERROR("Could not open storage directory: %s: %s", SS_STORAGE_DIR, strerror(errno));
        return -1;
    }
    closedir(d);

    int first_file = 1;
    int buffer_pos = 0;
    scan_directory_recursive(SS_STORAGE_DIR, "", file_list_buffer, buffer_size, &buffer_pos, &first_file);

    if (first_file) {
        LOG_INFO("No existing files found in storage directory.");
    } 
    
    else {
        LOG_INFO("Found existing files: %s", file_list_buffer);
    }

    return 0;
}

int register_with_nm(){

    LOG_INFO("Attempting to register with Name Server at %s:%d...", NM_IP, NM_REGISTER_PORT);

    char file_list_buffer[4096]; 
    if (scan_storage_directory(file_list_buffer, sizeof(file_list_buffer)) != 0) {
        return -1;
    }

    char register_msg[5000];
    snprintf(register_msg, sizeof(register_msg), "REGISTER|127.0.0.1|%d|%d|%s", SS_NM_PORT, SS_CLIENT_PORT, file_list_buffer);

    int nm_sock_fd;
    struct sockaddr_in nm_addr;

    nm_sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_sock_fd < 0) {
        LOG_ERROR("Failed to create socket for NM registration: %s", strerror(errno));
        return -1;
    }

    memset(&nm_addr, 0, sizeof(nm_addr));
    nm_addr.sin_family = AF_INET;
    nm_addr.sin_port = htons(NM_REGISTER_PORT);

    // convert IP to binary from text
    if (inet_pton(AF_INET, NM_IP, &nm_addr.sin_addr) <= 0) {

        LOG_ERROR("Invalid Name Server IP address: %s", strerror(errno));
        close(nm_sock_fd);
        return -1;
    }

    if (connect(nm_sock_fd, (struct sockaddr*)&nm_addr, sizeof(nm_addr)) < 0) {

        LOG_ERROR("Failed to connect to Name Server: %s", strerror(errno));
        close(nm_sock_fd);
        return -1;
    }

    if (send(nm_sock_fd, register_msg, strlen(register_msg), 0) < 0) {
        LOG_ERROR("Failed to send registration message: %s", strerror(errno));
        close(nm_sock_fd);
        return -1;
    }
    LOG_INFO("Sent registration message: %s", register_msg);

    // wait for ack
    char buffer[1024] = {0};
    int valread = read(nm_sock_fd, buffer, sizeof(buffer) - 1);
    if (valread <= 0) {

        LOG_ERROR("Failed to read ACK from Name Server: %s", strerror(errno));
        close(nm_sock_fd);
        return -1;
    }

    if (strncmp(buffer, "ACK_REGISTER OK", 15) == 0) {

        LOG_INFO("Registration successful!");
        close(nm_sock_fd);
        return 0; 
    } 
    
    else {

        LOG_WARN("Registration failed. NM responded: %s", buffer);
        close(nm_sock_fd);
        return -1; 
    }
}

typedef struct {
    int client_sock;
    struct sockaddr_in client_addr;
} connection_args_t;

int is_filename_valid(const char* filename) {

    if (!filename || filename[0] == '\0') {
        return 0;
    }

    if (strstr(filename, "..") != NULL) {
        return 0;
    }

    if (filename[0] == '/' || strchr(filename, '\\') != NULL) {
        return 0;
    }

    return 1; 
}

static int is_tag_valid(const char* tag)
{
    if (!tag || tag[0] == '\0')
    {
        return 0;
    }
    for (const char* p = tag; *p; ++p)
    {
        if (*p == '/' || *p == '\\' || *p == '|' || *p == '\n' || *p == '\r')
        {
            return 0;
        }
    }
    return strstr(tag, "..") == NULL;
}

static int ensure_dirs_for_full_path(const char* full_path) {
    if (!full_path || full_path[0] == '\0') {
        return -1;
    }

    char temp[512];
    strncpy(temp, full_path, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';

    for (char* p = temp + 1; *p; ++p) {
        if (*p == '/') {
            char saved = *p;
            *p = '\0';
            if (mkdir(temp, 0777) == -1 && errno != EEXIST) {
                *p = saved;
                return -1;
            }
            *p = saved;
        }
    }

    return 0;
}

static int ensure_parent_dirs_for_relative(const char* relative_path) {
    if (!relative_path) {
        return -1;
    }

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s%s", SS_STORAGE_DIR, relative_path);
    return ensure_dirs_for_full_path(full_path);
}

static void send_simple_response(int sock, const char* msg)
{
    if (!msg)
    {
        return;
    }
    send(sock, msg, strlen(msg), 0);
}

static int copy_file_contents(const char* src_path, const char* dst_path)
{
    FILE* src_fp = fopen(src_path, "r");
    if (!src_fp)
    {
        return -1;
    }

    if (ensure_dirs_for_full_path(dst_path) != 0)
    {
        server_log("WARN", "Failed to prepare destination directories for %s", dst_path);
    }

    FILE* dst_fp = fopen(dst_path, "w");
    if (!dst_fp)
    {
        fclose(src_fp);
        return -1;
    }

    char buffer[4096];
    size_t bytes_read, bytes_written;
    int success = 0;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), src_fp)) > 0)
    {
        bytes_written = fwrite(buffer, 1, bytes_read, dst_fp);
        if (bytes_written != bytes_read)
        {
            success = -1;
            break;
        }
    }

    if (ferror(src_fp) || ferror(dst_fp))
    {
        success = -1;
    }

    fclose(src_fp);
    fclose(dst_fp);

    return success;
}

static void build_checkpoint_path(const char* filename, const char* tag, char* out, size_t out_size) {
    if (!filename || !tag || !out || out_size == 0) {
        if (out && out_size > 0) {
            out[0] = '\0';
        }
        return;
    }

    snprintf(out, out_size, "%s.checkpoints/%s/%s", SS_STORAGE_DIR, filename, tag);
}

static void build_checkpoint_dir(const char* filename, char* out, size_t out_size) {
    if (!filename || !out || out_size == 0) {
        if (out && out_size > 0) {
            out[0] = '\0';
        }
        return;
    }
    snprintf(out, out_size, "%s.checkpoints/%s", SS_STORAGE_DIR, filename);
}

int create_undo_backup(const char* file_path, const char* undo_path){

    int source_fd = open(file_path, O_RDONLY);
    if(source_fd < 0){
        if (errno == ENOENT) return 0; 
        LOG_ERROR("Failed to open source file for undo: %s", strerror(errno));
        return -1;
    }

    // get file stats for calcualting file size
    struct stat stat_source;
    if (fstat(source_fd, &stat_source) < 0) {
        LOG_ERROR("Failed to get file stats for undo: %s", strerror(errno));
        close(source_fd);
        return -1;
    }

    if (ensure_dirs_for_full_path(undo_path) != 0) {
        LOG_WARN("Failed to prepare directories for undo path: %s", undo_path);
    }

    int dest_fd = open(undo_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (dest_fd < 0) {
        LOG_ERROR("Failed to open undo file for writing: %s", strerror(errno));
        close(source_fd);
        return -1;
    }

    off_t offset = 0;
    ssize_t bytes_sent = sendfile(dest_fd, source_fd, &offset, stat_source.st_size);

    close(source_fd);
    close(dest_fd);

    if (bytes_sent != stat_source.st_size) {
        LOG_ERROR("Incomplete copy for undo file.");
        return -1;
    }

    LOG_INFO("Created undo backup: %s", undo_path);
    return 0;
}

int acquire_lock(const char* filename, int sentence_index, pthread_t owner_id){

    pthread_mutex_lock(&lock_list_mutex);

    // Firstly, check if this thread already holds a lock
    Filelock* current = global_lock_list;
    while(current){
        if(pthread_equal(current->owner_thread_id, owner_id)){
            if (strcmp(current->filename, filename) == 0 &&
                current->sentence_index == sentence_index) {
                // Same thread re-acquiring the same sentence: allow it.
                LOG_INFO("Thread %lu already holds lock for %s[%d], reusing.",
                         owner_id, filename, sentence_index);
                pthread_mutex_unlock(&lock_list_mutex);
                return 1;
            } else {
                // Thread is trying to lock a different sentence/file.
                LOG_WARN("Thread %lu already holds a lock, cannot acquire another.", owner_id);
                pthread_mutex_unlock(&lock_list_mutex);
                return 0;
            }
        }
        current = current->next;
    }

    current = global_lock_list;
    while(current){

        if (strcmp(current->filename, filename) == 0 && current->sentence_index == sentence_index) {

            LOG_WARN("Failed to acquire lock: %s[%d] already locked by thread %lu", filename, sentence_index, current->owner_thread_id);
            pthread_mutex_unlock(&lock_list_mutex);
            return 0; 
        }
        current = current->next;
    }

    Filelock* new_lock = (Filelock*)malloc(sizeof(Filelock));
    strncpy(new_lock->filename, filename, sizeof(new_lock->filename) - 1);
    new_lock->sentence_index = sentence_index;
    new_lock->owner_thread_id = owner_id;
    
    new_lock->next = global_lock_list;
    global_lock_list = new_lock;

    LOG_INFO("Lock acquired by thread %lu for %s[%d]", owner_id, filename, sentence_index);
    pthread_mutex_unlock(&lock_list_mutex);
    return 1;
}

void release_lock(pthread_t owner_id) {

    pthread_mutex_lock(&lock_list_mutex);

    Filelock* current = global_lock_list;
    Filelock* prev = NULL;

    while (current) {

        if (pthread_equal(current->owner_thread_id, owner_id)) {
            
            if (prev) {
                prev->next = current->next;
            } 
            
            else {
                global_lock_list = current->next;
            }
            
            LOG_INFO("Lock released by thread %lu for %s[%d]", owner_id, current->filename, current->sentence_index);
            
            free(current);
            pthread_mutex_unlock(&lock_list_mutex);
            return;
        }
        prev = current;
        current = current->next;
    }

    LOG_WARN("Thread %lu tried to release a lock, but held none.", owner_id);
    pthread_mutex_unlock(&lock_list_mutex);
}

Filelock* find_lock_by_thread(pthread_t owner_id) {

    Filelock* current = global_lock_list;
    while (current) {

        if (pthread_equal(current->owner_thread_id, owner_id)) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

void* handle_nm_connection(void* args){

    connection_args_t* conn_args = (connection_args_t*)args;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(conn_args->client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    
    LOG_INFO("Name Server connected from: %s:%d", client_ip, ntohs(conn_args->client_addr.sin_port));

    char buffer[1024];
    int bytes_read;

    while ((bytes_read = read(conn_args->client_sock, buffer, sizeof(buffer) - 1)) > 0) {
       
        buffer[bytes_read] = '\0';

        buffer[strcspn(buffer, "\r\n")] = 0;

        LOG_INFO("Received NM command: %s", buffer);

        char* command;
        char* arg1;
        char* arg2;
        
        char buffer_copy[1024];
        strncpy(buffer_copy, buffer, sizeof(buffer_copy));

        command = strtok(buffer_copy, "|");
        arg1 = strtok(NULL, "|");
        arg2 = strtok(NULL, "|");

        if (!command) {

            LOG_WARN("Invalid command format from NM: %s", buffer);
            send(conn_args->client_sock, "ACK_ERROR Invalid format\n", 24, 0);
            continue; 
        }

        if ((strcmp(command, "CREATE") == 0 ||
             strcmp(command, "DELETE") == 0 ||
             strcmp(command, "CREATEFOLDER") == 0) && !arg1) {
            LOG_WARN("Invalid command format from NM: %s", buffer);
            send(conn_args->client_sock, "ACK_ERROR Invalid format\n", 24, 0);
            continue;
        }

        if (strcmp(command, "MOVE") == 0 && (!arg1 || !arg2)) {
            LOG_WARN("Invalid MOVE command format from NM: %s", buffer);
            send(conn_args->client_sock, "ACK_ERROR Invalid format\n", 24, 0);
            continue;
        }

        if ((strcmp(command, "CREATE") == 0 ||
             strcmp(command, "DELETE") == 0 ||
             strcmp(command, "CREATEFOLDER") == 0 ||
             strcmp(command, "MOVE") == 0) && arg1 && !is_filename_valid(arg1)) {
            LOG_WARN("SECURITY: Invalid filename received: %s", arg1);
            send(conn_args->client_sock, "ACK_ERROR Invalid filename\n", 26, 0);
            continue;
        }

        if (strcmp(command, "MOVE") == 0 && arg2 && !is_filename_valid(arg2)) {
            LOG_WARN("SECURITY: Invalid MOVE target received: %s", arg2);
            send(conn_args->client_sock, "ACK_ERROR Invalid filename\n", 26, 0);
            continue;
        }

        char file_path[512] = {0};
        if (arg1) {
            snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, arg1);
        }

        if (strcmp(command, "CREATE") == 0) {

            ensure_parent_dirs_for_relative(arg1);
            FILE* fp = fopen(file_path, "w"); 
            if (fp) {

                fclose(fp);
                LOG_INFO("File created: %s", file_path);
                send(conn_args->client_sock, "ACK_CREATE OK\n", 13, 0); 
            } 
            
            else {
                LOG_ERROR("Failed to create file: %s (%s)", file_path, strerror(errno));
                send(conn_args->client_sock, "ACK_CREATE ERROR\n", 16, 0);
            }
        } 
        
        else if (strcmp(command, "DELETE") == 0) {

            if (remove(file_path) == 0) { 
                LOG_INFO("File deleted: %s", file_path);
                send(conn_args->client_sock, "ACK_DELETE OK\n", 13, 0);
            } 
            
            else {
                LOG_ERROR("Failed to delete file: %s (%s)", file_path, strerror(errno));
                send(conn_args->client_sock, "ACK_DELETE ERROR\n", 16, 0); 
            }
        }
        
        else if (strcmp(command, "CREATEFOLDER") == 0) {

            ensure_parent_dirs_for_relative(arg1);
            char dir_path[512];
            snprintf(dir_path, sizeof(dir_path), "%s%s", SS_STORAGE_DIR, arg1);
            if (ensure_dirs_for_full_path(dir_path) != 0) {
                LOG_WARN("Failed to ensure directories for folder: %s", dir_path);
            }
            if (mkdir(dir_path, 0777) == -1 && errno != EEXIST) {
                LOG_ERROR("Failed to create folder %s (%s)", dir_path, strerror(errno));
                send_simple_response(conn_args->client_sock, "ACK_CREATEFOLDER ERROR\n");
            } else {
                LOG_INFO("Folder created: %s", dir_path);
                send_simple_response(conn_args->client_sock, "ACK_CREATEFOLDER OK\n");
            }
        }

        else if (strcmp(command, "MOVE") == 0) {

            char new_path[512];
            snprintf(new_path, sizeof(new_path), "%s%s", SS_STORAGE_DIR, arg2);
            if (ensure_dirs_for_full_path(new_path) != 0) {
                LOG_WARN("Failed to prepare directories for %s", new_path);
            }

            if (rename(file_path, new_path) == 0) {
                char old_undo[512];
                char new_undo[512];
                snprintf(old_undo, sizeof(old_undo), "%s%s.undo", SS_STORAGE_DIR, arg1);
                snprintf(new_undo, sizeof(new_undo), "%s%s.undo", SS_STORAGE_DIR, arg2);
                ensure_dirs_for_full_path(new_undo);
                if (access(old_undo, F_OK) == 0) {
                    rename(old_undo, new_undo);
                }
                LOG_INFO("File moved %s -> %s", file_path, new_path);
                send_simple_response(conn_args->client_sock, "ACK_MOVE OK\n");
            } else {
                LOG_ERROR("Failed to move file %s -> %s (%s)", file_path, new_path, strerror(errno));
                send_simple_response(conn_args->client_sock, "ACK_MOVE ERROR\n");
            }
        }

        else if (strcmp(command, "CHECKPOINT") == 0) {

            if (!arg2 || !is_tag_valid(arg2)) {
                send_simple_response(conn_args->client_sock, "ACK_CHECKPOINT ERROR InvalidTag\n");
                continue;
            }

            if (access(file_path, F_OK) != 0) {
                send_simple_response(conn_args->client_sock, "ACK_CHECKPOINT ERROR NoFile\n");
                continue;
            }

            char checkpoint_path[512];
            build_checkpoint_path(arg1, arg2, checkpoint_path, sizeof(checkpoint_path));
            if (copy_file_contents(file_path, checkpoint_path) == 0) {
                LOG_INFO("Checkpoint %s created at %s", arg2, checkpoint_path);
                send_simple_response(conn_args->client_sock, "ACK_CHECKPOINT OK\n");
            } else {
                LOG_ERROR("Failed to create checkpoint %s for %s", arg2, file_path);
                send_simple_response(conn_args->client_sock, "ACK_CHECKPOINT ERROR\n");
            }
        }

        else if (strcmp(command, "VIEWCHECKPOINT") == 0) {

            if (!arg2 || !is_tag_valid(arg2)) {
                send_simple_response(conn_args->client_sock, "ACK_VIEWCHECKPOINT ERROR InvalidTag\n");
                continue;
            }
            char checkpoint_path[512];
            build_checkpoint_path(arg1, arg2, checkpoint_path, sizeof(checkpoint_path));
            FILE* fp = fopen(checkpoint_path, "r");
            if (!fp) {
                send_simple_response(conn_args->client_sock, "ACK_VIEWCHECKPOINT ERROR NotFound\n");
                continue;
            }
            send_simple_response(conn_args->client_sock, "ACK_VIEWCHECKPOINT OK\n");
            char chunk[512];
            size_t nread;
            while ((nread = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
                if (send(conn_args->client_sock, chunk, nread, 0) < 0) {
                    break;
                }
            }
            fclose(fp);
            send(conn_args->client_sock, STOP_PACKET, strlen(STOP_PACKET), 0);
        }

        else if (strcmp(command, "REVERT") == 0) {

            if (!arg2 || !is_tag_valid(arg2)) {
                send_simple_response(conn_args->client_sock, "ACK_REVERT ERROR InvalidTag\n");
                continue;
            }

            char checkpoint_path[512];
            build_checkpoint_path(arg1, arg2, checkpoint_path, sizeof(checkpoint_path));
            if (access(checkpoint_path, F_OK) != 0) {
                send_simple_response(conn_args->client_sock, "ACK_REVERT ERROR NotFound\n");
                continue;
            }

            if (copy_file_contents(checkpoint_path, file_path) == 0) {
                LOG_INFO("Reverted %s to checkpoint %s", file_path, arg2);
                send_simple_response(conn_args->client_sock, "ACK_REVERT OK\n");
            } else {
                LOG_ERROR("Failed to revert %s to checkpoint %s", file_path, arg2);
                send_simple_response(conn_args->client_sock, "ACK_REVERT ERROR\n");
            }
        }

        else if (strcmp(command, "LISTCHECKPOINTS") == 0) {

            char checkpoint_dir[512];
            build_checkpoint_dir(arg1, checkpoint_dir, sizeof(checkpoint_dir));
            DIR* dir = opendir(checkpoint_dir);
            char response[NM_RESP_BUFFER];
            size_t used = 0;
            if (!dir) {
                used = snprintf(response, sizeof(response), "ACK_LISTCHECKPOINTS OK \n");
            }
            else {
                used = snprintf(response, sizeof(response), "ACK_LISTCHECKPOINTS OK ");
                struct dirent* entry;
                int first = 1;
                while ((entry = readdir(dir)) != NULL) {
                    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                        continue;
                    }
                    if (!first) {
                        used += snprintf(response + used, sizeof(response) - used, ",");
                    }
                    first = 0;
                    used += snprintf(response + used, sizeof(response) - used, "%s", entry->d_name);
                    if (used >= sizeof(response) - 2) {
                        break;
                    }
                }
                closedir(dir);
                used += snprintf(response + used, sizeof(response) - used, "\n");
            }
            response[sizeof(response) - 1] = '\0';
            send_simple_response(conn_args->client_sock, response);
        }
        
        else {
            LOG_WARN("Unknown command from NM: %s", command);
            send(conn_args->client_sock, "ACK_ERROR Unknown command\n", 25, 0);
        }
    }

    if (bytes_read == 0) {
        LOG_INFO("Name Server disconnected: %s:%d", client_ip, ntohs(conn_args->client_addr.sin_port));
    } 
    
    else if (bytes_read < 0) {
        LOG_ERROR("Read error from Name Server: %s", strerror(errno));
    }

    close(conn_args->client_sock);
    free(conn_args);
    pthread_exit(NULL);
}

void* handle_client_connection(void* args) {
    
    connection_args_t* conn_args = (connection_args_t*)args;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(conn_args->client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    
    LOG_INFO("Client connected from: %s:%d", client_ip, ntohs(conn_args->client_addr.sin_port));
    
    char buffer[1024];
    int bytes_read;

    while((bytes_read = read(conn_args->client_sock, buffer, sizeof(buffer) - 1))){

        buffer[bytes_read] = '\0';
        buffer[strcspn(buffer, "\r\n")] = 0;

        LOG_INFO("Received Client command: %s", buffer);

        char* command;
        char* filename;
        char buffer_copy[1024];
        strncpy(buffer_copy, buffer, sizeof(buffer_copy));

        command = strtok(buffer_copy, "|");
        filename = strtok(NULL, "|");

        if (!command || !filename) {
            
            LOG_WARN("Invalid command format from Client: %s", buffer);
            send(conn_args->client_sock, "ERROR Invalid format\n", 21, 0);
            continue;
        }

        if (strcmp(command, "ETIRW") == 0) {

            LOG_WARN("Client sent ETIRW outside of a write session.");
            send(conn_args->client_sock, "ERROR Not in write mode\n", 24, 0);
            continue;
        }

        if (!is_filename_valid(filename)) {
            LOG_WARN("SECURITY: Invalid filename from Client: %s", filename);
            send(conn_args->client_sock, "ERROR Invalid filename\n", 23, 0);
            continue;
        }

        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", SS_STORAGE_DIR, filename);

        FILE* fp = fopen(file_path, "r");
        if(fp == NULL){
            LOG_WARN("Client requested file not found: %s", file_path);
            send(conn_args->client_sock, "ERROR File not found\n", 21, 0);
            continue;
        }

        if (strcmp(command, "READ") == 0) {

            FILE* fp = fopen(file_path, "r");
            if (fp == NULL) {

                LOG_WARN("Client requested file not found: %s", file_path);
                send(conn_args->client_sock, "ERROR File not found\n", 21, 0);
                continue;
            }

            LOG_INFO("Handling READ for: %s", file_path);
            char read_buffer[4096];
            size_t bytes_from_file;

            while ((bytes_from_file = fread(read_buffer, 1, sizeof(read_buffer), fp)) > 0) {

                if (send(conn_args->client_sock, read_buffer, bytes_from_file, 0) < 0) {
                    LOG_WARN("Send error during READ to %s: %s", client_ip, strerror(errno));
                    break; 
                }
            }

            send(conn_args->client_sock, STOP_PACKET, strlen(STOP_PACKET), 0);
            LOG_INFO("READ complete for: %s", file_path);
            fclose(fp);
        }

        else if (strcmp(command, "STREAM") == 0) {

            FILE* fp = fopen(file_path, "r");
            if (fp == NULL) {

                LOG_WARN("Client requested file not found: %s", file_path);
                send(conn_args->client_sock, "ERROR File not found\n", 21, 0);
                continue;
            }

            LOG_INFO("Handling STREAM for: %s", file_path);
            char word_buffer[256];
            char word_with_space[260]; 

            while (fscanf(fp, "%255s", word_buffer) == 1) {

                snprintf(word_with_space, sizeof(word_with_space), "%s ", word_buffer);

                if (send(conn_args->client_sock, word_with_space, strlen(word_with_space), 0) < 0) {
                    LOG_WARN("Send error during STREAM to %s: %s", client_ip, strerror(errno));
                    break;
                }
                usleep(100000);
            }

            send(conn_args->client_sock, STOP_PACKET, strlen(STOP_PACKET), 0);
            LOG_INFO("STREAM complete for: %s", file_path);
        }

        else if(strcmp(command, "UNDO") == 0){

            LOG_INFO("Handling UNDO for: %s", filename);
            char undo_path[512];
            snprintf(undo_path, sizeof(undo_path), "%s%s.undo", SS_STORAGE_DIR, filename);

            if (access(undo_path, F_OK) == -1) {
                LOG_WARN("No undo file found: %s", undo_path);
                send(conn_args->client_sock, "ERROR No undo history\n", 22, 0);
                continue;
            }

            // rename() moves the file, overwriting the original
            if (rename(undo_path, file_path) == 0) {
                LOG_INFO("Undo successful. Restored %s from %s", file_path, undo_path);
                send(conn_args->client_sock, "UNDO_SUCCESS\n", 13, 0);
            } 
            
            else {
                LOG_ERROR("Failed to restore from undo file: %s", strerror(errno));
                send(conn_args->client_sock, "ERROR Undo failed\n", 16, 0);
            }
        }

        else if (strcmp(command, "WRITE") == 0) 
        {
            LOG_INFO("WRITE command received for: %s", filename);

            char *sentence_str = strtok(NULL, "|");
            if (!sentence_str) 
            {
                LOG_WARN("WRITE command missing sentence index: %s", buffer);
                send(conn_args->client_sock, "ERROR Invalid format\n", 20, 0);
                continue;
            }
            int sentence_index = atoi(sentence_str);

            // Create undo backup before any writes
            char undo_path[512];
            snprintf(undo_path, sizeof(undo_path), "%s%s.undo", SS_STORAGE_DIR, filename);
            if (create_undo_backup(file_path, undo_path) != 0) 
            {
                LOG_WARN("Failed to create undo backup for %s", filename);
            }
            
            Stringnode* sentences = parse_into_sentences(file_path);
            if (!sentences) 
            {
                sentences = create_sentence_node("");
                if (!sentences) 
                {
                    send(conn_args->client_sock, "ERROR Failed to allocate memory\n", 29, 0);
                    continue;
                }
            }
            
            int sentence_count = count_sentences(sentences);
            LOG_INFO("File has %d sentences", sentence_count);

            // Validate sentence index according to spec
            if (sentence_index < 0 || sentence_index > sentence_count) 
            {
                char err[128];
                snprintf(err, sizeof(err), "ERROR Sentence %d out of range (0-%d)\\n", sentence_index, sentence_count);
                send(conn_args->client_sock, err, strlen(err), 0);
                free_list(sentences);
                continue;
            }

            if (sentence_index == sentence_count)
            {
                if (sentence_count > 0)
                {
                    Stringnode* last_sentence = get_node_at(sentences, sentence_count - 1);
                    if (last_sentence && !sentence_has_terminal_delimiter(last_sentence->str))
                    {
                        LOG_WARN("Cannot append sentence %d: previous sentence lacks delimiter", sentence_index);
                        send(conn_args->client_sock,
                             "ERROR Cannot append sentence without delimiter\n", 49, 0);
                        free_list(sentences);
                        continue;
                    }
                }

                Stringnode* new_sentence_node = create_sentence_node("");
                if (!new_sentence_node)
                {
                    send(conn_args->client_sock, "ERROR Failed to allocate memory\n", 29, 0);
                    free_list(sentences);
                    continue;
                }
                append_sentence_to_tail(&sentences, new_sentence_node);
                sentence_count++;
                LOG_INFO("Appended new sentence slot. Total sentences: %d", sentence_count);
            }

            // Acquire lock for this sentence once for the whole WRITE session
            pthread_t my_thread = pthread_self();
            if (!acquire_lock(filename, sentence_index, my_thread)) 
            {
                send(conn_args->client_sock,
                     "ERROR Sentence locked by another user\n", 39, 0);
                free_list(sentences);
                continue;
            }
            
            send(conn_args->client_sock, "ACK_WRITE OK\n", 13, 0);
            
            Modification* modifications = NULL;
            Modification* modifications_tail = NULL;
            int modifications_made = 0;
            
            while (1) 
            {
                char write_buffer[1024];
                int write_bytes = read(conn_args->client_sock, write_buffer, sizeof(write_buffer) - 1);
                if (write_bytes <= 0) 
                {
                    LOG_WARN("Client disconnected during WRITE session");
                    free_modification_list(modifications);
                    free_list(sentences);
                    break;
                }
                
                write_buffer[write_bytes] = '\0';
                write_buffer[strcspn(write_buffer, "\r\n")] = 0;
                
                LOG_INFO("WRITE mode input: %s", write_buffer);
                
                // Check for ETIRW (end write)
                if (strcmp(write_buffer, "ETIRW") == 0) 
                {
                    // Release any locks held by this thread
                    release_lock(my_thread);
                    
                    // Write modified sentences back to file
                    if (modifications_made > 0) 
                    {
                        // Re-read the file to get the latest state (including other users' changes)
                        Stringnode* latest_sentences = parse_into_sentences(file_path);
                        if (!latest_sentences) 
                        {
                            latest_sentences = create_sentence_node("");
                            if (!latest_sentences) 
                            {
                                LOG_ERROR("Failed to allocate memory for latest sentences");
                                send(conn_args->client_sock, "ERROR Failed to allocate memory\n", 29, 0);
                                free_modification_list(modifications);
                                break;
                            }
                        }
                        
                        // Validate sentence index against latest file state
                        int latest_sentence_count = count_sentences(latest_sentences);
                        if (sentence_index < 0 || sentence_index >= latest_sentence_count) 
                        {
                            // If sentence_index was valid at start but now out of range,
                            // it means the file structure changed. Try to handle it.
                            if (sentence_index == latest_sentence_count) 
                            {
                                // Append new sentence if needed
                                if (latest_sentence_count > 0)
                                {
                                    Stringnode* last_sentence = get_node_at(latest_sentences, latest_sentence_count - 1);
                                    if (last_sentence && !sentence_has_terminal_delimiter(last_sentence->str))
                                    {
                                        LOG_WARN("Cannot append sentence %d: previous sentence lacks delimiter", sentence_index);
                                        send(conn_args->client_sock, "ERROR Cannot append sentence without delimiter\n", 49, 0);
                                        free_list(latest_sentences);
                                        free_modification_list(modifications);
                                        break;
                                    }
                                }
                                Stringnode* new_sentence_node = create_sentence_node("");
                                if (new_sentence_node) 
                                {
                                    append_sentence_to_tail(&latest_sentences, new_sentence_node);
                                    latest_sentence_count++;
                                }
                            }
                            else 
                            {
                                LOG_WARN("Sentence index %d out of range (0-%d) after re-read", sentence_index, latest_sentence_count - 1);
                                send(conn_args->client_sock, "ERROR Sentence index out of range\n", 33, 0);
                                free_list(latest_sentences);
                                free_modification_list(modifications);
                                break;
                            }
                        }
                        
                        Stringnode* target_sentence_node = get_node_at(latest_sentences, sentence_index);
                        if (!target_sentence_node) 
                        {
                            LOG_ERROR("Failed to get target sentence node");
                            send(conn_args->client_sock, "ERROR Failed to get sentence\n", 29, 0);
                            free_list(latest_sentences);
                            free_modification_list(modifications);
                            break;
                        }
                        
                        // Each modification is applied to the result of the previous modification
                        Modification* mod = modifications;
                        while (mod) 
                        {
                            // Get current sentence text
                            char* current_sentence_text = target_sentence_node->str;
                            
                            char* new_sentence = replace_word_in_sentence(current_sentence_text, mod->word_index, mod->content);
                            if (!new_sentence) 
                            {
                                LOG_WARN("Failed to apply modification: word index %d", mod->word_index);
                                mod = mod->next;
                                continue;
                            }

                            char* first_sentence_part = NULL;
                            Stringnode* extra_sentences_split = NULL;
                            if (split_sentence_string(new_sentence, &first_sentence_part, &extra_sentences_split) != 0)
                            {
                                free(new_sentence);
                                LOG_WARN("Failed to split sentence after modification");
                                mod = mod->next;
                                continue;
                            }
                            free(new_sentence);
                            
                            // Update the sentence with the result of this modification
                            free(target_sentence_node->str);
                            target_sentence_node->str = first_sentence_part;
                            if (extra_sentences_split)
                            {
                                append_sentence_nodes(target_sentence_node, extra_sentences_split);
                                LOG_INFO("Sentence %d split into additional sentences", sentence_index);
                            }
                            
                            mod = mod->next;
                        }
                        
                        // Write the updated sentences back to file
                        if (write_sentences_to_file(file_path, latest_sentences) == 0) 
                        {
                            LOG_INFO("Write successful: %d modifications applied to latest file state", modifications_made);
                            send(conn_args->client_sock, "Write Successful!\n", 18, 0);
                        } 
                        else 
                        {
                            LOG_ERROR("Failed to write file back");
                            send(conn_args->client_sock, "ERROR Failed to write file\n", 27, 0);
                        }
                        
                        free_list(latest_sentences);
                    } 
                    else 
                    {
                        send(conn_args->client_sock, "No modifications made\n", 22, 0);
                    }
                    
                    free_modification_list(modifications);
                    free_list(sentences);
                    break;
                }
        
                char* word_idx_str = strtok(write_buffer, "|");
                char* new_content = strtok(NULL, "\n");
                
                if (!word_idx_str || !new_content) 
                {
                    send(conn_args->client_sock, "ERROR Invalid format\n", 20, 0);
                    continue;
                }
                
                int user_word_index = atoi(word_idx_str);

                Stringnode* target_node = get_node_at(sentences, sentence_index);
                if (!target_node) {
                     send(conn_args->client_sock, "ERROR Internal state error\n", 27, 0);
                     continue;
                }

                char* test_sentence = replace_word_in_sentence(target_node->str, user_word_index, new_content);
                
                if (!test_sentence) 
                {
                    char err[128];
                    snprintf(err, sizeof(err), "ERROR Word index %d out of bounds\n", user_word_index);
                    send(conn_args->client_sock, err, strlen(err), 0);
                    continue; 
                }

                free(target_node->str);
                target_node->str = test_sentence;
                
                // Store the modification instead of applying it immediately
                Modification* new_mod = (Modification*)malloc(sizeof(Modification));
                if (!new_mod) 
                {
                    send(conn_args->client_sock, "ERROR Failed to allocate memory\n", 29, 0);
                    continue;
                }
                new_mod->word_index = user_word_index;
                new_mod->content = strdup(new_content);
                new_mod->next = NULL;
                
                if (!modifications) 
                {
                    modifications = modifications_tail = new_mod;
                } 
                else 
                {
                    modifications_tail->next = new_mod;
                    modifications_tail = new_mod;
                }
                
                modifications_made++;
                LOG_INFO("Stored modification: sentence %d, word %d", sentence_index, user_word_index);
                send(conn_args->client_sock, "OK\n", 3, 0);
            }
        }
        
        else {
            LOG_WARN("Unknown command from Client: %s", command);
            send(conn_args->client_sock, "ERROR Unknown command\n", 22, 0);
        }
    }

    if (bytes_read == 0) {
        LOG_INFO("Client disconnected: %s:%d", client_ip, ntohs(conn_args->client_addr.sin_port));
    } 
    
    else if (bytes_read < 0) {
        LOG_ERROR("Read error from Client: %s", strerror(errno));
    }

    close(conn_args->client_sock);    
    free(conn_args);
    pthread_exit(NULL);
}

typedef struct {
    int port;
    void* (*handler_function)(void*); 
} listener_args_t;

void* start_server_listener(void* args){
    
    listener_args_t* listener = (listener_args_t*)args;
    int port = listener->port;
    void* (*handler)(void*) = listener->handler_function;

    free(listener);

    struct sockaddr_in server_addr;
    int server_sock_fd;
    
    server_sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock_fd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        pthread_exit(NULL);
    }

    int opt = 1;
    if(setsockopt(server_sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))){
        LOG_WARN("Failed to set SO_REUSEADDR: %s", strerror(errno));
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // listen on 0.0.0.0
    server_addr.sin_port = htons(port);       // host to network short byte order
    
    if (bind(server_sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {

        LOG_ERROR("Failed to bind to port %d: %s", port, strerror(errno));
        close(server_sock_fd);
        pthread_exit(NULL);
    }

    if (listen(server_sock_fd, 10) < 0) {
        LOG_ERROR("Failed to listen on port %d: %s", port, strerror(errno));
        close(server_sock_fd);
        pthread_exit(NULL);
    }

    LOG_INFO("Server listener started on 0.0.0.0:%d", port);
    
    while(1){

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock_fd = accept(server_sock_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_sock_fd < 0) {
            LOG_WARN("Failed to accept connection: %s", strerror(errno));
            continue; 
        }

        connection_args_t* conn_args = malloc(sizeof(connection_args_t));
        if (!conn_args) {
            LOG_ERROR("Failed to allocate memory for connection args");
            close(client_sock_fd);
            continue;
        }
        conn_args->client_sock = client_sock_fd;
        conn_args->client_addr = client_addr;

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handler, (void*)conn_args) != 0) {
            LOG_ERROR("Failed to create handler thread: %s", strerror(errno));
            free(conn_args);
            close(client_sock_fd);
        }
        
        pthread_detach(thread_id);
        
    }
    
    close(server_sock_fd);
    pthread_exit(NULL);
}

void setup_logging(){

    log_fp = fopen(LOG_FILE, "a");
    if(log_fp == NULL) perror("Failed to open file");

    LOG_INFO("Storage Server logger initialized.");
}

int main(int argc, char*argv[]) {

    setup_logging();

    if (argc == 3) {

        SS_NM_PORT = atoi(argv[1]);
        SS_CLIENT_PORT = atoi(argv[2]);

        snprintf(SS_STORAGE_DIR, sizeof(SS_STORAGE_DIR), "storage_%d/", SS_NM_PORT);

        LOG_INFO("Using custom ports: NM=%d, Client=%d", SS_NM_PORT, SS_CLIENT_PORT);
        LOG_INFO("Using custom storage directory: %s", SS_STORAGE_DIR);
    } 
    
    else {
        LOG_INFO("Using default ports: NM=%d, Client=%d", SS_NM_PORT, SS_CLIENT_PORT);
    }

    if (mkdir(SS_STORAGE_DIR, 0777) == -1) {

        if (errno != EEXIST) { 
            LOG_ERROR("Failed to create storage directory: %s", strerror(errno));
        }
    } 
    
    else {
        LOG_INFO("Created storage directory: %s", SS_STORAGE_DIR);
    }

    if (register_with_nm() != 0) {
        LOG_ERROR("Failed to register with Name Server. Shutting down.");
        return 1;
    }

    pthread_t nm_listener_tid, client_listener_tid;

    listener_args_t* nm_args = malloc(sizeof(listener_args_t));
    nm_args->port = SS_NM_PORT;
    nm_args->handler_function = handle_nm_connection;
    
    if (pthread_create(&nm_listener_tid, NULL, start_server_listener, (void*)nm_args) != 0) {
        LOG_ERROR("Failed to create NM listener thread: %s", strerror(errno));
        return 1;
    }

    listener_args_t* client_args = malloc(sizeof(listener_args_t));
    client_args->port = SS_CLIENT_PORT;
    client_args->handler_function = handle_client_connection;

    if (pthread_create(&client_listener_tid, NULL, start_server_listener, (void*)client_args) != 0) {
        LOG_ERROR("Failed to create Client listener thread: %s", strerror(errno));
        return 1;
    }
    
    LOG_INFO("Storage Server is running. Press Ctrl+C to stop.");

    pause();

    fclose(log_fp);
    pthread_mutex_destroy(&log_mutex);
    return 0;
}