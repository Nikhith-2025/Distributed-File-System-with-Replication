#include "../common.h"

#define NM_IP "127.0.0.1"
#define NM_PORT 8000

int main() {
    printf("==========================================\n");
    printf("       Welcome to Docs++ Client Interface         \n");
    printf("==========================================\n\n");
    
// BY THIS WE WOULD GET THE USERNAME OF THE CLIENT
    char username[MAX_USERNAME];
    printf("Enter your username: ");
    fgets(username, sizeof(username), stdin);
    trim_string(username);
    
    if (strlen(username) == 0) 
    {
        printf("ERROR: Username cannot be empty\n");
        return 1;
    }
    
    // Connect to Name Server
    printf("Connecting to Name Server at %s:%d...\n", NM_IP, NM_PORT);
    
    int nm_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (nm_sock < 0) 
    {
        perror("Socket creation failed");
        return 1;
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(NM_PORT);
    server_addr.sin_addr.s_addr = inet_addr(NM_IP);
    
    if (connect(nm_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to Name Server failed");
        close(nm_sock);
        return 1;
    }
    
    printf("Connected!\n\n");
    
    // Register with Name Server
    char reg_msg[128];
    snprintf(reg_msg, sizeof(reg_msg), "REGISTER %s", username);
    send(nm_sock, reg_msg, strlen(reg_msg), 0);
    
    // Get welcome response
    char buffer[BUFFER_SIZE];
    ssize_t bytes = recv(nm_sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        printf("%s\n", buffer);
    }
    while (1) {
        // this is the prompt that is displayed for ever when the client runs
        printf("docs++(%s)> ", username);
        fflush(stdout);
        // Get command from user
        char command[MAX_COMMAND];
        if (!fgets(command, sizeof(command), stdin)) 
        {
            break;
        }
        trim_string(command);
        // if the command is empty then continue
        if (strlen(command) == 0) 
        {
            continue;
        }
        if (strcasecmp(command, "EXIT") == 0 || strcasecmp(command, "QUIT") == 0) 
        {
            send(nm_sock, command, strlen(command), 0);
            break;
        }
        // Send command to Name Server
        send(nm_sock, command, strlen(command), 0);
        
        // Get response
        bytes = recv(nm_sock, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) 
        {
            printf("ERROR: Lost connection to Name Server\n");
            break;
        }
        
        buffer[bytes] = '\0';
        printf("%s", buffer);
        if (buffer[bytes - 1] != '\n') 
        {
            printf("\n");
        }
    }
    
    printf("\nRun the client again if u want to continue!\n");
    close(nm_sock);
    return 0;
}
