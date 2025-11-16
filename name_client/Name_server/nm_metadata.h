#ifndef NM_METADATA_H
#define NM_METADATA_H

#include "../common.h"

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

typedef struct
{
    FileHashMap file_map;

    StorageServerInfo storage_servers[MAX_STORAGE_SERVERS];
    int storage_count;
    pthread_mutex_t ss_lock;

    ClientInfo clients[MAX_USERS];
    int client_count;
    pthread_mutex_t client_lock;

    char users[MAX_USERS][MAX_USERNAME];
    int user_count;
    pthread_mutex_t user_lock;

    FILE *log_file;
    pthread_mutex_t log_lock;

} NameServerState;

extern NameServerState nm_state;

unsigned int hash_filename(const char *filename);
void init_file_hashmap(FileHashMap *map);
int insert_file(FileHashMap *map, FileMetadata *metadata);
FileMetadata *find_file(FileHashMap *map, const char *filename);
int delete_file(FileHashMap *map, const char *filename);
int get_all_files(FileHashMap *map, FileMetadata ***files_out);
void free_file_hashmap(FileHashMap *map);

void add_user(const char *username);
int user_exists(const char *username);
int get_all_users(char users[][MAX_USERNAME], int max_users);

int register_client(int sockfd, const char *username, const char *ip);
ClientInfo *find_client_by_socket(int sockfd);
ClientInfo *find_client_by_username(const char *username);
void remove_client(int sockfd);

int register_storage_server(int sockfd, const char *ip, int nm_port, int client_port);
StorageServerInfo *get_available_storage_server();
void remove_storage_server(int index);
void add_files_to_ss(int ss_index, char files[][MAX_FILENAME], int file_count);
int find_least_loaded_ss();
void deactivate_storage_server(int ss_index);
StorageServerInfo *get_storage_server(int ss_index);

AccessLevel check_access(const char *filename, const char *username);
int add_read_access(const char *filename, const char *username);
int add_write_access(const char *filename, const char *username);
int remove_access(const char *filename, const char *username);

void init_logging(const char *log_filename);
void log_message(const char *level, const char *source_ip, int source_port,
                 const char *username, const char *message);
void log_request(const char *username, const char *command, const char *details);
void log_response(const char *username, ErrorCode error, const char *details);
void close_logging();

void init_name_server();
void cleanup_name_server();

#endif
