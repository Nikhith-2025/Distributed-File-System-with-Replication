#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>

#define MAX_FILENAME 256
#define MAX_USERNAME 64
#define MAX_PATH 512
#define MAX_COMMAND 1024
#define BUFFER_SIZE 4096
#define MAX_FILES 10000
#define MAX_USERS 1000
#define MAX_STORAGE_SERVERS 10
#define MAX_ACCESS_USERS 100
#define INET_ADDRSTRLEN 16

typedef enum
{
    ERR_SUCCESS = 0,
    ERR_FILE_NOT_FOUND,
    ERR_FILE_ALREADY_EXISTS,
    ERR_PERMISSION_DENIED,
    ERR_INVALID_COMMAND,
    ERR_INVALID_ARGUMENTS,
    ERR_STORAGE_SERVER_DOWN,
    ERR_SENTENCE_LOCKED,
    ERR_SENTENCE_OUT_OF_RANGE,
    ERR_WORD_INDEX_INVALID,
    ERR_USER_NOT_FOUND,
    ERR_NOT_FILE_OWNER,
    ERR_CONNECTION_FAILED,
    ERR_INTERNAL_ERROR,
    ERR_NO_STORAGE_SERVERS,
    ERR_WRITE_MODE_ACTIVE,
    ERR_NOT_IN_WRITE_MODE,
    ERR_FILE_LOCKED,
    ERR_NO_UNDO_HISTORY
} ErrorCode;

typedef enum
{
    MSG_REGISTER_CLIENT,
    MSG_REGISTER_SS,
    MSG_CREATE_FILE,
    MSG_DELETE_FILE,
    MSG_READ_FILE,
    MSG_WRITE_FILE,
    MSG_STREAM_FILE,
    MSG_VIEW_FILES,
    MSG_INFO_FILE,
    MSG_LIST_USERS,
    MSG_ADD_ACCESS,
    MSG_REM_ACCESS,
    MSG_EXEC_FILE,
    MSG_UNDO,
    MSG_RESPONSE,
    MSG_ERROR,
    MSG_CONNECT_SS,
    MSG_ACK,
    MSG_ALIVE_CHECK
} MessageType;

typedef enum
{
    ACCESS_NONE = 0,
    ACCESS_READ,
    ACCESS_WRITE,
    ACCESS_OWNER
} AccessLevel;

typedef struct
{
    char filename[MAX_FILENAME];
    char owner[MAX_USERNAME];
    int ss_index;

    char readers[MAX_ACCESS_USERS][MAX_USERNAME];
    int reader_count;
    char writers[MAX_ACCESS_USERS][MAX_USERNAME];
    int writer_count;

    time_t created_time;
    time_t last_modified;
    time_t last_accessed;
    char last_accessed_by[MAX_USERNAME];
    size_t file_size;
    int word_count;
    int char_count;

    int is_locked;
    int locked_sentence;
    char locked_by[MAX_USERNAME];
} FileMetadata;

typedef struct
{
    int sockfd;
    char ip[INET_ADDRSTRLEN];
    int nm_port;
    int client_port;
    int active;
    char files[MAX_FILES][MAX_FILENAME];
    int file_count;
} StorageServerInfo;

typedef struct
{
    int sockfd;
    char username[MAX_USERNAME];
    char ip[INET_ADDRSTRLEN];
    int active;

    int in_write_mode;
    char current_write_file[MAX_FILENAME];
    int current_write_sentence;
} ClientInfo;

typedef struct
{
    MessageType type;
    ErrorCode error_code;
    char username[MAX_USERNAME];
    char filename[MAX_FILENAME];
    char data[BUFFER_SIZE];
    int sentence_number;
    int word_index;

    char ss_ip[INET_ADDRSTRLEN];
    int ss_port;

    int flag_all;
    int flag_long;
    int flag_read;
    int flag_write;
} Message;

static inline const char *get_error_message(ErrorCode code)
{
    switch (code)
    {
        case ERR_SUCCESS: return "Success";
        case ERR_FILE_NOT_FOUND: return "ERROR: File not found";
        case ERR_FILE_ALREADY_EXISTS: return "ERROR: File already exists";
        case ERR_PERMISSION_DENIED: return "ERROR: Permission denied";
        case ERR_INVALID_COMMAND: return "ERROR: Invalid command";
        case ERR_INVALID_ARGUMENTS: return "ERROR: Invalid arguments";
        case ERR_STORAGE_SERVER_DOWN: return "ERROR: Storage server unavailable";
        case ERR_SENTENCE_LOCKED: return "ERROR: Sentence is locked for editing";
        case ERR_SENTENCE_OUT_OF_RANGE: return "ERROR: Sentence index out of range";
        case ERR_WORD_INDEX_INVALID: return "ERROR: Word index invalid";
        case ERR_USER_NOT_FOUND: return "ERROR: User not found";
        case ERR_NOT_FILE_OWNER: return "ERROR: You are not the file owner";
        case ERR_CONNECTION_FAILED: return "ERROR: Connection failed";
        case ERR_INTERNAL_ERROR: return "ERROR: Internal server error";
        case ERR_NO_STORAGE_SERVERS: return "ERROR: No storage servers available";
        case ERR_WRITE_MODE_ACTIVE: return "ERROR: Already in write mode";
        case ERR_NOT_IN_WRITE_MODE: return "ERROR: Not in write mode";
        case ERR_FILE_LOCKED: return "ERROR: File is locked";
        case ERR_NO_UNDO_HISTORY: return "ERROR: No undo history available";
        default: return "ERROR: Unknown error";
    }
}

static inline void trim_string(char *str)
{
    if (!str) return;

    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\n' || str[len - 1] == '\r' || str[len - 1] == '\t'))
    {
        str[--len] = '\0';
    }

    char *start = str;
    while (*start && (*start == ' ' || *start == '\t'))
    {
        start++;
    }

    if (start != str)
    {
        memmove(str, start, strlen(start) + 1);
    }
}

static inline void get_timestamp(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

static inline void format_time(time_t t, char *buffer, size_t size)
{
    struct tm *tm_info = localtime(&t);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

#endif
