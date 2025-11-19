#include "../common.h"

#define NM_IP "127.0.0.1"
#define NM_PORT 8000

#include <errno.h>

typedef struct
{
    char ip[INET_ADDRSTRLEN];
    int port;
    int valid;
} SsEndpoint;

static int connect_to_endpoint(const SsEndpoint *ep)
{
    if (!ep || !ep->valid)
    {
        return -1;
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        return -1;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ep->port);
    addr.sin_addr.s_addr = inet_addr(ep->ip);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }
    return sock;
}

static int send_all(int sock, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0)
        {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

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
            char exit_cmd[MAX_COMMAND + 2];
            snprintf(exit_cmd, sizeof(exit_cmd), "%s\n", command);
            send(nm_sock, exit_cmd, strlen(exit_cmd), 0);
            break;
        }
        
        // Send command to Name Server (with newline)
        char cmd_with_newline[MAX_COMMAND + 2];
        snprintf(cmd_with_newline, sizeof(cmd_with_newline), "%s\n", command);
        send(nm_sock, cmd_with_newline, strlen(cmd_with_newline), 0);
        
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
            SsEndpoint primary = {.valid = 0};
            SsEndpoint replica = {.valid = 0};
            char ss_filename[MAX_FILENAME];

            char *token = strtok(buffer, " \n");
            token = strtok(NULL, " \n");      // IP
            if (token)
            {
                strncpy(primary.ip, token, sizeof(primary.ip));
                primary.valid = 1;
            }
            token = strtok(NULL, " \n");      // Port
            if (token) primary.port = atoi(token);
            token = strtok(NULL, " \n");    // Filename
            if (token) strncpy(ss_filename, token, sizeof(ss_filename));
            token = strtok(NULL, " \n");
            if (token && strcmp(token, "REPLICA") == 0)
            {
                char *rep_ip = strtok(NULL, " \n");
                char *rep_port = strtok(NULL, " \n");
                if (rep_ip && rep_port)
                {
                    strncpy(replica.ip, rep_ip, sizeof(replica.ip));
                    replica.port = atoi(rep_port);
                    replica.valid = 1;
                }
            }

            if (!primary.valid)
            {
                printf("ERROR: Invalid storage server info\n");
                continue;
            }
            
            printf("Connecting to Storage Server at %s:%d...\n", primary.ip, primary.port);

            int ss_sock = connect_to_endpoint(&primary);
            if (ss_sock < 0) 
            {
                if (replica.valid)
                {
                    printf("Primary unavailable, trying replica %s:%d...\n",
                           replica.ip, replica.port);
                    ss_sock = connect_to_endpoint(&replica);
                    if (ss_sock >= 0)
                    {
                        primary = replica;
                    }
                }
            }
            if (ss_sock < 0)
            {
                perror("Failed to connect to Storage Server");
                continue;
            }

            int replica_sock = -1;
            if (replica.valid &&
                (strcmp(replica.ip, primary.ip) != 0 || replica.port != primary.port))
            {
                replica_sock = connect_to_endpoint(&replica);
                if (replica_sock < 0)
                {
                    replica.valid = 0;
                }
            }

            char cmd_copy[MAX_COMMAND];
            strncpy(cmd_copy, command, sizeof(cmd_copy));
            char *cmd_token = strtok(cmd_copy, " \t");

            if (strcasecmp(cmd_token, "READ") == 0)
            {
                char ss_cmd[512];
                snprintf(ss_cmd, sizeof(ss_cmd), "READ|%s\n", ss_filename);
                send_all(ss_sock, ss_cmd, strlen(ss_cmd));
                
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
                send_all(ss_sock, ss_cmd, strlen(ss_cmd));

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
                send_all(ss_sock, ss_cmd, strlen(ss_cmd));
                if (replica_sock >= 0)
                {
                    send_all(replica_sock, ss_cmd, strlen(ss_cmd));
                }
                
                char undo_resp[256];
                ssize_t n = recv(ss_sock, undo_resp, sizeof(undo_resp) - 1, 0);
                if (n > 0) 
                {
                    undo_resp[n] = '\0';
                    printf("%s\n", undo_resp);
                }
                if (replica_sock >= 0)
                {
                    char replica_resp[256];
                    recv(replica_sock, replica_resp, sizeof(replica_resp) - 1, 0);
                }
            }

            else if (strcasecmp(cmd_token, "WRITE") == 0) 
            {
                int write_replica_sock = replica_sock;
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
                if (send_all(ss_sock, ss_cmd, strlen(ss_cmd)) != 0)
                {
                    printf("ERROR: Storage Server failed to accept WRITE\n");
                    if (write_replica_sock >= 0) close(write_replica_sock);
                    close(ss_sock);
                    continue;
                }
                if (write_replica_sock >= 0)
                {
                    if (send_all(write_replica_sock, ss_cmd, strlen(ss_cmd)) != 0)
                    {
                        printf("WARN: Replica server failed to accept WRITE\n");
                        close(write_replica_sock);
                        write_replica_sock = -1;
                    }
                }
                
                // Wait for ACK_WRITE
                char ack[256];
                ssize_t n = recv(ss_sock, ack, sizeof(ack) - 1, 0);
                if (n <= 0 || strncmp(ack, "ACK_WRITE", 9) != 0) 
                {
                    printf("ERROR: Storage Server failed to accept WRITE\n");
                    if (write_replica_sock >= 0) close(write_replica_sock);
                    close(ss_sock);
                    continue;
                }
                if (write_replica_sock >= 0)
                {
                    char replica_ack[256];
                    n = recv(write_replica_sock, replica_ack, sizeof(replica_ack) - 1, 0);
                    if (n <= 0 || strncmp(replica_ack, "ACK_WRITE", 9) != 0)
                    {
                        printf("WARN: Replica server failed to accept WRITE\n");
                        close(write_replica_sock);
                        write_replica_sock = -1;
                    }
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
                        send_all(ss_sock, "ETIRW\n", 6);
                        if (write_replica_sock >= 0)
                        {
                            send_all(write_replica_sock, "ETIRW\n", 6);
                        }
                        
                        char etirw_resp[256];
                        n = recv(ss_sock, etirw_resp, sizeof(etirw_resp) - 1, 0);
                        if (n > 0) 
                        {
                            etirw_resp[n] = '\0';
                            printf("%s\n", etirw_resp);
                        }
                        if (write_replica_sock >= 0)
                        {
                            recv(write_replica_sock, etirw_resp, sizeof(etirw_resp) - 1, 0);
                        }
                        break;
                    }
                    
                    int word_idx;
                    char content[512];
                    
                    if (sscanf(write_input, "%d %[^\n]", &word_idx, content) == 2) 
                    {
                        char write_cmd[1024];
                        snprintf(write_cmd, sizeof(write_cmd), "%d|%s\n", word_idx, content);
                        if (send_all(ss_sock, write_cmd, strlen(write_cmd)) != 0)
                        {
                            printf("ERROR: Failed to send write command\n");
                            break;
                        }
                        if (write_replica_sock >= 0)
                        {
                            if (send_all(write_replica_sock, write_cmd, strlen(write_cmd)) != 0)
                            {
                                printf("WARN: Failed to send write command to replica\n");
                                close(write_replica_sock);
                                write_replica_sock = -1;
                            }
                        }
                        
                        char write_resp[256];
                        n = recv(ss_sock, write_resp, sizeof(write_resp) - 1, 0);
                        
                        if (n > 0) 
                        {
                            write_resp[n] = '\0';
                            printf("%s", write_resp);
                        }
                        if (write_replica_sock >= 0)
                        {
                            recv(write_replica_sock, write_resp, sizeof(write_resp) - 1, 0);
                        }
                    } 
                    else 
                    {
                        printf("Invalid format. Use: <word_index> <content>\n");
                    }
                }
                if (write_replica_sock >= 0)
                {
                    close(write_replica_sock);
                }
                replica_sock = -1;
            }
            
            if (replica_sock >= 0)
            {
                close(replica_sock);
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
