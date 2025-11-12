#ifndef NM_METADATA_H
#define NM_METADATA_H

#include "../common.h"

// ==================== HASHMAP FOR FILES ====================
#define HASH_TABLE_SIZE 10007 

typedef struct FileNode 
{
    FileMetadata metadata;
    struct FileNode *next;
} FileNode;

typedef struct 
{
    FileNode *buckets[HASH_TABLE_SIZE];
    pthread_mutex_t lock;
    int total_files;
} FileHashMap;

// ==================== NAME SERVER STATE ====================
typedef struct 
{
    // this manages all the files in the system
    FileHashMap file_map;
    // Connected storage servers
    StorageServerInfo storage_servers[MAX_STORAGE_SERVERS];
    int storage_count;
    pthread_mutex_t ss_lock;
    // Connected clients
    ClientInfo clients[MAX_USERS];
    int client_count;
    pthread_mutex_t client_lock;
    // User list (all users who have ever connected)
    char users[MAX_USERS][MAX_USERNAME];
    int user_count;
    pthread_mutex_t user_lock;
    FILE *log_file;
    pthread_mutex_t log_lock;
    
} NameServerState;

extern NameServerState nm_state;

// ==================== HASHMAP RELATED FUNCTIONS ====================

// this function takes the name of the file and returns an integer which is later used to get the info
unsigned int hash_filename(const char *filename);
// Initialize the hashmap
void init_file_hashmap(FileHashMap *map);
// Insert file metadata
int insert_file(FileHashMap *map, FileMetadata *metadata);
// Find file metadata
FileMetadata* find_file(FileHashMap *map, const char *filename);
// Delete file from hashmap
int delete_file(FileHashMap *map, const char *filename);
// we get all the files that are present in the hashmap basically it stores everyfile so that we can retieve it easily
int get_all_files(FileHashMap *map, FileMetadata ***files_out);

void free_file_hashmap(FileHashMap *map);

// ==================== USER ====================

// Add user to system
void add_user(const char *username);

// Checks if the user exists
int user_exists(const char *username);

// Get all the user names in the system
int get_all_users(char users[][MAX_USERNAME], int max_users);

// ==================== CLIENT ====================

// Register a new client
int register_client(int sockfd, const char *username, const char *ip);

// Find client by socket
ClientInfo* find_client_by_socket(int sockfd);

// Find client by username
ClientInfo* find_client_by_username(const char *username);

// Remove client
void remove_client(int sockfd);

// ==================== STORAGE SERVER MANAGEMENT ====================

// Register storage server
int register_storage_server(int sockfd, const char *ip, int nm_port, int client_port);

// Add files to storage server
void add_files_to_ss(int ss_index, char files[][MAX_FILENAME], int file_count);

// Find storage server with least files so that a single server is not overloaded
int find_least_loaded_ss();

// Mark storage server as inactive
void deactivate_storage_server(int ss_index);

// Get storage server info
StorageServerInfo* get_storage_server(int ss_index);

// ==================== ACCESS CONTROL ====================

// Check user access level for a file
AccessLevel check_access(const char *filename, const char *username);

// Add read access
int add_read_access(const char *filename, const char *username);

// Add write access this also includes read access
int add_write_access(const char *filename, const char *username);

// Remove access
int remove_access(const char *filename, const char *username);

// ==================== LOGGING ====================

// Initialize logging system
void init_logging(const char *log_filename);

// Log a message
void log_message(const char *level, const char *source_ip, int source_port, 
                 const char *username, const char *message);

// Log request
void log_request(const char *username, const char *command, const char *details);

// Log response
void log_response(const char *username, ErrorCode error, const char *details);

void close_logging();

// ==================== NAME SERVER ====================

// Initialize the entire name server state
void init_name_server();

void cleanup_name_server();

#endif 
