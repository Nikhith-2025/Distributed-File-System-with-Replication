#include "../common.h"
#include "nm_metadata.h"
#include "commands.h"
#include <sys/stat.h>

#define NM_PORT 8000

// Thread entry function for handling one client
void *handle_client_thread(void *arg);

int main()
{
    // Initialize Name Server structures and metadata
    init_name_server();
    mkdir("nm_files", 0777);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0)
    {
        perror("Socket creation failed");
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Configure server address
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(NM_PORT);

    // Bind server socket
    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed");
        close(server_sock);
        return 1;
    }

    // Start listening
    if (listen(server_sock, 10) < 0)
    {
        perror("Listen failed");
        close(server_sock);
        return 1;
    }

    printf("[NM] Name Server listening on port %d\n", NM_PORT);
    log_message("INFO", NULL, 0, NULL, "Name Server started and listening");

    // Main accept loop
    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0)
        {
            perror("Accept failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

        printf("[NM] New connection from %s\n", client_ip);

        pthread_t thread;
        int *sock_ptr = malloc(sizeof(int));
        *sock_ptr = client_sock;

        if (pthread_create(&thread, NULL, handle_client_thread, sock_ptr) != 0)
        {
            perror("Thread creation failed");
            close(client_sock);
            free(sock_ptr);
            continue;
        }

        pthread_detach(thread);
    }

    cleanup_name_server();
    close(server_sock);
    return 0;
}

// Handle one client's session
void *handle_client_thread(void *arg)
{
    int client_sock = *(int *)arg;
    free(arg);

    char buffer[BUFFER_SIZE];
    char username[MAX_USERNAME] = {0};

    // First message: must be registration
    ssize_t bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0)
    {
        close(client_sock);
        return NULL;
    }
    buffer[bytes] = '\0';
    trim_string(buffer);

    // ================= Storage Server Registration =================
    if (strncmp(buffer, "REGISTER|", 9) == 0)
    {
        char *saveptr;
        char *token = strtok_r(buffer, "|", &saveptr);

        // IP
        token = strtok_r(NULL, "|", &saveptr);
        char ss_ip[INET_ADDRSTRLEN];
        if (token) strncpy(ss_ip, token, INET_ADDRSTRLEN - 1);

        // NM control port
        token = strtok_r(NULL, "|", &saveptr);
        int nm_port = token ? atoi(token) : 0;

        // Client port
        token = strtok_r(NULL, "|", &saveptr);
        int client_port = token ? atoi(token) : 0;

        // File list
        token = strtok_r(NULL, "|", &saveptr);
        char *file_list = token ? token : "";

        // Register Storage Server
        int ss_index = register_storage_server(client_sock, ss_ip, nm_port, client_port);

        // add files from SS to NM metadata
        if (ss_index >= 0 && strlen(file_list) > 0)
        {
            char *file_saveptr;
            char *filename = strtok_r(file_list, ",", &file_saveptr);
            int metadata_changed = 0;

            while (filename)
            {
                trim_string(filename);
                if (strlen(filename) == 0)
                {
                    filename = strtok_r(NULL, ",", &file_saveptr);
                    continue;
                }

                FileMetadata meta = {0};
                strncpy(meta.filename, filename, MAX_FILENAME - 1);
                strcpy(meta.owner, "system");
                meta.ss_index = ss_index;
                meta.created_time = time(NULL);
                meta.last_modified = time(NULL);
                meta.last_accessed = time(NULL);

                if (insert_file(&nm_state.file_map, &meta) == 0)
                {
                    metadata_changed = 1;
                }
                else
                {
                    FileMetadata *existing = find_file(&nm_state.file_map, filename);
                    if (existing)
                    {
                       if (existing->ss_index >= 0 && nm_state.storage_servers[existing->ss_index].active) {
                            existing->replica_ss_index = ss_index;
                        } 
                        else {
                            existing->ss_index = ss_index;
                        }
                        metadata_changed = 1;
                    }
                }

                filename = strtok_r(NULL, ",", &file_saveptr);
            }

            if (metadata_changed)
            {
                persist_metadata();
            }
        }

        if (ss_index >= 0)
        {
            printf("[NM] Storage Server registered: %s (NM port: %d, Client port: %d, SS index: %d)\n",
                   ss_ip, nm_port, client_port, ss_index);

            log_message("INFO", ss_ip, client_port, "STORAGE_SERVER", "Storage Server registered");

            send(client_sock, "ACK_REGISTER OK\n", 17, 0);
        }
        else
        {
            send(client_sock, "ACK_REGISTER ERROR\n", 20, 0);
        }

        close(client_sock);
        return NULL;
    }

    // ================= Client Registration =================
    else if (strncmp(buffer, "REGISTER ", 9) == 0)
    {
        strncpy(username, buffer + 9, MAX_USERNAME - 1);
        trim_string(username);

        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        getpeername(client_sock, (struct sockaddr *)&addr, &addr_len);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);

        register_client(client_sock, username, ip);

        printf("[NM] Client registered: %s from %s\n", username, ip);
        log_message("INFO", ip, 0, username, "Client connected");

        char welcome[256];
        snprintf(welcome, sizeof(welcome), "Welcome %s! Connected to Name Server.\n", username);
        send(client_sock, welcome, strlen(welcome), 0);
    }
    else
    {
        send(client_sock, "ERROR: First message must be REGISTER\n", 39, 0);
        close(client_sock);
        return NULL;
    }

    // ================= Command Loop =================
    while (1)
    {
        bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);

        if (bytes <= 0)
        {
            break;
        }

        buffer[bytes] = '\0';
        
        // Split buffer by newlines and process each command separately
        char *line = buffer;
        char *next_line;
        while (line && *line)
        {
            // Find the next newline
            next_line = strchr(line, '\n');
            if (next_line)
            {
                *next_line = '\0';
                next_line++;
            }
            
            // Trim current line
            char *cmd_start = line;
            while (*cmd_start == ' ' || *cmd_start == '\t' || *cmd_start == '\r')
                cmd_start++;
            
            char *cmd_end = cmd_start + strlen(cmd_start) - 1;
            while (cmd_end >= cmd_start && (*cmd_end == ' ' || *cmd_end == '\t' || *cmd_end == '\r'))
                cmd_end--;
            cmd_end[1] = '\0';
            
            if (*cmd_start == '\0')
            {
                line = next_line;
                continue;
            }
            
            if (strcasecmp(cmd_start, "EXIT") == 0 || strcasecmp(cmd_start, "QUIT") == 0)
            {
                send(client_sock, "Goodbye!\n", 9, 0);
                goto client_exit;
            }

            printf("[NM] %s: %s\n", username, cmd_start);

            handle_command(client_sock, username, cmd_start);
            
            line = next_line;
        }
    }
    
client_exit:

    // Client exits
    printf("[NM] Client %s disconnected\n", username);
    log_message("INFO", NULL, 0, username, "Client disconnected");

    remove_client(client_sock);
    close(client_sock);

    return NULL;
}
