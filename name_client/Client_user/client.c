#include "../common.h"

#define NM_IP "127.0.0.1"
#define NM_PORT 8000

int main() 
{
    printf("==========================================\n");
    printf("          Welcome to HUSTLERS++           \n");
    printf("==========================================\n\n");

    // Get username from user
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
    
    if (connect(nm_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) 
    {
        perror("Connection to Name Server failed");
        close(nm_sock);
        return 1;
    }
    
    printf("Connected!\n\n");
    
    // Register user with NS
    char reg_msg[128];
    snprintf(reg_msg, sizeof(reg_msg), "REGISTER %s", username);
    send(nm_sock, reg_msg, strlen(reg_msg), 0);
   
    char buffer[BUFFER_SIZE];
    ssize_t bytes = recv(nm_sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes > 0) 
    {
        buffer[bytes] = '\0';
        printf("%s\n", buffer);
    }

    while (1) 
    {
        printf("hustlers++(%s)> ", username);
        fflush(stdout);
        
        // Read user command
        char command[MAX_COMMAND];
        if (!fgets(command, sizeof(command), stdin)) 
        {
            break;
        }
        trim_string(command);
        
        if (strlen(command) == 0) 
        {
            continue;
        }
        
        // Exit command
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

        if (strncmp(buffer, "CONNECT_SS", 10) == 0) 
        {
            int is_undo = (strncmp(buffer, "CONNECT_SS_UNDO", 15) == 0);
            char ss_ip[INET_ADDRSTRLEN];
            int ss_port;
            char ss_filename[MAX_FILENAME];

            // Parse CONNECT_SS packet
            char *token = strtok(buffer, " \n");
            token = strtok(NULL, " ");      // IP
            if (token) strncpy(ss_ip, token, sizeof(ss_ip));
            token = strtok(NULL, " ");      // Port
            if (token) ss_port = atoi(token);
            token = strtok(NULL, " \n");    // Filename
            if (token) strncpy(ss_filename, token, sizeof(ss_filename));
            
            printf("Connecting to Storage Server at %s:%d...\n", ss_ip, ss_port);

            // Connect to Storage Server
            int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
            if (ss_sock < 0) 
            {
                perror("Failed to create SS socket");
                continue;
            }
            
            struct sockaddr_in ss_addr;
            ss_addr.sin_family = AF_INET;
            ss_addr.sin_port = htons(ss_port);
            ss_addr.sin_addr.s_addr = inet_addr(ss_ip);
            
            if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) 
            {
                perror("Failed to connect to Storage Server");
                close(ss_sock);
                continue;
            }

            char cmd_copy[MAX_COMMAND];
            strncpy(cmd_copy, command, sizeof(cmd_copy));
            char *cmd_token = strtok(cmd_copy, " \t");

            if (strcasecmp(cmd_token, "READ") == 0)
            {
                char ss_cmd[512];
                snprintf(ss_cmd, sizeof(ss_cmd), "READ|%s\n", ss_filename);
                send(ss_sock, ss_cmd, strlen(ss_cmd), 0);
                
                printf("\n");
                char recv_buf[BUFFER_SIZE];
                
                while (1) 
                {
                    ssize_t n = recv(ss_sock, recv_buf, sizeof(recv_buf) - 1, 0);
                    if (n <= 0) break;
                    
                    recv_buf[n] = '\0';
                    
                    if (strstr(recv_buf, "--STOP--")) 
                    {
                        char *stop_pos = strstr(recv_buf, "\n--STOP--");
                        if (stop_pos) *stop_pos = '\0';
                        printf("%s", recv_buf);
                        break;
                    }
                    
                    printf("%s", recv_buf);
                }
                printf("\n\n");
            }

            else if (strcasecmp(cmd_token, "STREAM") == 0)
            {
                char ss_cmd[512];
                snprintf(ss_cmd, sizeof(ss_cmd), "STREAM|%s\n", ss_filename);
                send(ss_sock, ss_cmd, strlen(ss_cmd), 0);

                char recv_buf[BUFFER_SIZE];
                char partial[BUFFER_SIZE];
                partial[0] = '\0';

                printf("\n");

                while (1)
                {
                    ssize_t n = recv(ss_sock, recv_buf, sizeof(recv_buf) - 1, 0);
                    if (n <= 0) break;

                    recv_buf[n] = '\0';

                    char *stop_pos = strstr(recv_buf, "--STOP--");
                    if (stop_pos) *stop_pos = '\0';

                    // Combine any partial word from previous chunk with new data
                    char combined[BUFFER_SIZE * 2];
                    snprintf(combined, sizeof(combined), "%s%s", partial, recv_buf);

                    char *saveptr;
                    char *token = strtok_r(combined, " \t\r\n", &saveptr);
                    partial[0] = '\0';

                    while (token)
                    {
                        char *next = strtok_r(NULL, " \t\r\n", &saveptr);
                        if (next == NULL && !stop_pos)
                        {
                            // Last token of this chunk and stream not finished yet:
                            // store it as partial in case it is split across packets.
                            strncpy(partial, token, sizeof(partial) - 1);
                            partial[sizeof(partial) - 1] = '\0';
                            break;
                        }

                        printf("%s ", token);
                        fflush(stdout);
                        usleep(100000); // 0.1 seconds between words
                        token = next;
                    }

                    if (stop_pos)
                    {
                        // Flush any remaining partial word when stream ends
                        if (partial[0] != '\0')
                        {
                            printf("%s ", partial);
                            fflush(stdout);
                        }
                        break;
                    }
                }

                printf("\n\n");
            }

            else if (strcasecmp(cmd_token, "UNDO") == 0 || is_undo) 
            {
                char ss_cmd[512];
                snprintf(ss_cmd, sizeof(ss_cmd), "UNDO|%s\n", ss_filename);
                send(ss_sock, ss_cmd, strlen(ss_cmd), 0);
                
                char undo_resp[256];
                ssize_t n = recv(ss_sock, undo_resp, sizeof(undo_resp) - 1, 0);
                if (n > 0) 
                {
                    undo_resp[n] = '\0';
                    printf("%s\n", undo_resp);
                }
            }

            else if (strcasecmp(cmd_token, "WRITE") == 0) 
            {
                char write_cmd_copy[MAX_COMMAND];
                strncpy(write_cmd_copy, command, sizeof(write_cmd_copy));
                write_cmd_copy[sizeof(write_cmd_copy) - 1] = '\0';

                char *token = strtok(write_cmd_copy, " \t"); // "WRITE"
                (void)token;
                char *ignored_filename = strtok(NULL, " \t");
                (void)ignored_filename;
                
                char *sentence_token = strtok(NULL, " \t");
                int sentence_num = sentence_token ? atoi(sentence_token) : 0;

                char ss_cmd[512];
                snprintf(ss_cmd, sizeof(ss_cmd), "WRITE|%s|%d\n", ss_filename, sentence_num);
                send(ss_sock, ss_cmd, strlen(ss_cmd), 0);
                
                // Wait for ACK_WRITE
                char ack[256];
                ssize_t n = recv(ss_sock, ack, sizeof(ack) - 1, 0);
                if (n <= 0 || strncmp(ack, "ACK_WRITE", 9) != 0) 
                {
                    printf("ERROR: Storage Server failed to accept WRITE\n");
                    close(ss_sock);
                    continue;
                }

                // Write mode loop
                while (1) 
                {
                    printf("write> ");
                    fflush(stdout);
                    
                    char write_input[MAX_COMMAND];
                    if (!fgets(write_input, sizeof(write_input), stdin)) break;
                    trim_string(write_input);
                    
                    if (strcasecmp(write_input, "ETIRW") == 0) 
                    {
                        send(ss_sock, "ETIRW\n", 6, 0);
                        
                        char etirw_resp[256];
                        n = recv(ss_sock, etirw_resp, sizeof(etirw_resp) - 1, 0);
                        if (n > 0) 
                        {
                            etirw_resp[n] = '\0';
                            printf("%s\n", etirw_resp);
                        }
                        break;
                    }
                    
                    int word_idx;
                    char content[512];
                    
                    if (sscanf(write_input, "%d %[^\n]", &word_idx, content) == 2) 
                    {
                        char write_cmd[1024];
                        snprintf(write_cmd, sizeof(write_cmd), "%d|%s\n", word_idx, content);
                        send(ss_sock, write_cmd, strlen(write_cmd), 0);
                        
                        char write_resp[256];
                        n = recv(ss_sock, write_resp, sizeof(write_resp) - 1, 0);
                        
                        if (n > 0) 
                        {
                            write_resp[n] = '\0';
                            printf("%s", write_resp);
                        }
                    } 
                    else 
                    {
                        printf("Invalid format. Use: <word_index> <content>\n");
                    }
                }
            }
            
            close(ss_sock);
        }
        else 
        {
            printf("%s", buffer);
            if (buffer[bytes - 1] != '\n') 
            {
                printf("\n");
            }
        }
    }
    
    printf("\nGoodbye!\n");
    close(nm_sock);
    return 0;
}
