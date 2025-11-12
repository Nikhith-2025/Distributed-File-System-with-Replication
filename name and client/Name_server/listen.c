#include "../common.h"
#include "nm_metadata.h"
#include "commands.h"
#include <sys/stat.h>

#define NM_PORT 8000

// this handles one client at a time so that multiple clients can be handled simultaneously
// each thread handles one client
void *handle_client_thread(void *arg);

int main() 
{
    printf("==========================================\n");
    printf("     Name Server Starting...             \n");
    printf("==========================================\n\n");
    
    init_name_server();
    
    // Create directory for storing files temporarily
    mkdir("nm_files", 0777);
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) 
    {
        perror("Socket creation failed");
        return 1;
    }
    
    // Allow reusing address
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // when we use a port then that port goes into a TIME_WAIT state so to avoid that we use this option
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(NM_PORT);
   
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) 
    {
        perror("Bind failed");
        close(server_sock);
        return 1;
    }
    
    if (listen(server_sock, 10) < 0) 
    {
        perror("Listen failed");
        close(server_sock);
        return 1;
    }
    
    printf("[NM] Name Server listening on port %d\n", NM_PORT);
    log_message("INFO", NULL, 0, NULL, "Name Server started and listening");
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
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
        // when we use detach the thread we don't have to join it later
    }
    
    cleanup_name_server();
    close(server_sock);
    return 0;
}

// This function runs in a separate thread for each client
void *handle_client_thread(void *arg) 
{
    int client_sock = *(int*)arg;
    free(arg);
    
    char buffer[BUFFER_SIZE];
    char username[MAX_USERNAME] = {0};
    // the first message is for the registration of the client
    ssize_t bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) 
    {
        close(client_sock);
        return NULL;
    }
    buffer[bytes] = '\0';
    trim_string(buffer);
    
    if (strncmp(buffer, "REGISTER ", 9) == 0) 
    {
        strncpy(username, buffer + 9, MAX_USERNAME - 1);
        trim_string(username);
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        getpeername(client_sock, (struct sockaddr*)&addr, &addr_len);
        
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
        send(client_sock, "ERROR: First message must be REGISTER username\n", 48, 0);
        close(client_sock);
        return NULL;
    }
    
    while (1) 
    {
        bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
       
        // didnt get any message from the client
        if (bytes <= 0) 
        {
            break;
        }
        
        buffer[bytes] = '\0';
        trim_string(buffer);
        
        if (strcasecmp(buffer, "EXIT") == 0 || strcasecmp(buffer, "QUIT") == 0) 
        {
            send(client_sock, "BYE!\n", 9, 0);
            break;
        }
        
        printf("[NM] %s: %s\n", username, buffer);
        
        handle_command(client_sock, username, buffer);
    }
    
    printf("[NM] Client %s disconnected\n", username);
    log_message("INFO", NULL, 0, username, "Client disconnected");
    
    remove_client(client_sock);
    close(client_sock);
    
    return NULL;
}
