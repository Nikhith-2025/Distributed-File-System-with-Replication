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

#define NM_PORT 9000
#define CLIENT_PORT 9001
#define STORAGE_DIR "storage/"
#define LOG_FILE "storage_server.log"
#define STOP_PACKET "\n--STOP--\n"

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

int scan_storage_directory(char* file_list_buffer, int buffer_size){

    memset(file_list_buffer, 0, buffer_size); // clear the buffer
    DIR *d;
    struct dirent *dir;

    d = opendir(STORAGE_DIR);
    if (!d) {
        LOG_ERROR("Could not open storage directory: %s: %s", STORAGE_DIR, strerror(errno));
        return -1;
    }

    int first_file = 1;
    int buffer_pos = 0;

    while((dir = readdir(d)) != NULL){

        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
            continue;
        }

        if (strstr(dir->d_name, ".undo") != NULL) {
            continue;
        }

        // add comma before file name
        if (!first_file) {
            if (buffer_pos + 1 >= buffer_size) {
                LOG_WARN("File list buffer too small during scan.");
                break;
            }
            file_list_buffer[buffer_pos++] = ',';
        }

        // add filename
        int name_len = strlen(dir->d_name);
        if (buffer_pos + name_len >= buffer_size) {
            LOG_WARN("File list buffer too small during scan.");
            break;
        }
        strncpy(file_list_buffer + buffer_pos, dir->d_name, name_len);
        buffer_pos += name_len;

        first_file = 0;
    }
    closedir(d);

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
    snprintf(register_msg, sizeof(register_msg), "REGISTER|127.0.0.1|%d|%d|%s", NM_PORT, CLIENT_PORT, file_list_buffer);

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

    if (strstr(filename, "..") != NULL) {
        return 0;
    }

    if (strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL) {
        return 0;
    }
    return 1; 
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

    Filelock* current = global_lock_list;
    while(current){
        if(pthread_equal(current->owner_thread_id, owner_id)){

            LOG_WARN("Thread %lu already holds a lock, cannot acquire another.", owner_id);
            pthread_mutex_unlock(&lock_list_mutex);
            return 0;
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
        char* filename;
        
        char buffer_copy[1024];
        strncpy(buffer_copy, buffer, sizeof(buffer_copy));

        command = strtok(buffer_copy, "|");
        filename = strtok(NULL, "|");

        if (!command || !filename) {

            LOG_WARN("Invalid command format from NM: %s", buffer);
            send(conn_args->client_sock, "ACK_ERROR Invalid format\n", 24, 0);
            continue; 
        }

        if (!is_filename_valid(filename)) {
            LOG_WARN("SECURITY: Invalid filename received: %s", filename);
            send(conn_args->client_sock, "ACK_ERROR Invalid filename\n", 26, 0);
            continue;
        }

        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", STORAGE_DIR, filename);

        if (strcmp(command, "CREATE") == 0) {

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
        buffer[strcspn(buffer, "r\n")] = 0;

        LOG_INFO("Received Client command: %s", buffer);

        char* command;
        char* filename;
        char buffer_copy[1024];
        strncpy(buffer_copy, buffer, sizeof(buffer_copy));

        command = strtok(buffer_copy, "|");
        filename = strtok(NULL, "|");

        if (!command || !filename) {
            
            LOG_WARN("Invalid command format from Client: %s", buffer);
            send(conn_args->client_sock, "ERROR Invalid format\n", 20, 0);
            continue;
        }

        if (strcmp(command, "ETIRW") == 0) {

            LOG_WARN("Client sent ETIRW outside of a write session.");
            send(conn_args->client_sock, "ERROR Not in write mode\n", 23, 0);
            continue;
        }

        if (!is_filename_valid(filename)) {
            LOG_WARN("SECURITY: Invalid filename from Client: %s", filename);
            send(conn_args->client_sock, "ERROR Invalid filename\n", 23, 0);
            continue;
        }

        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s%s", STORAGE_DIR, filename);

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
            snprintf(undo_path, sizeof(undo_path), "%s%s.undo", STORAGE_DIR, filename);

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

        else if (strcmp(command, "WRITE") == 0) {
           
            send(conn_args->client_sock, "ACK_WRITE OK\n", 13, 0);
            LOG_INFO("WRITE command received (not yet implemented)");
            
            // This is where we will enter the "write state"
            // And then read the <word_index> <content> lines
            // And finally ETIRW
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

int main() {

    setup_logging();

    if (mkdir(STORAGE_DIR, 0777) == -1) {

        if (errno != EEXIST) { 
            LOG_ERROR("Failed to create storage directory: %s", strerror(errno));
        }
    } 
    
    else {
        LOG_INFO("Created storage directory: %s", STORAGE_DIR);
    }

    if (register_with_nm() != 0) {
        LOG_ERROR("Failed to register with Name Server. Shutting down.");
        return 1;
    }

    pthread_t nm_listener_tid, client_listener_tid;

    listener_args_t* nm_args = malloc(sizeof(listener_args_t));
    nm_args->port = NM_PORT;
    nm_args->handler_function = handle_nm_connection;
    
    if (pthread_create(&nm_listener_tid, NULL, start_server_listener, (void*)nm_args) != 0) {
        LOG_ERROR("Failed to create NM listener thread: %s", strerror(errno));
        return 1;
    }

    listener_args_t* client_args = malloc(sizeof(listener_args_t));
    client_args->port = CLIENT_PORT;
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
