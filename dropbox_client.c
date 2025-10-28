#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define BUF_SIZE 8192
#define PROGRESS_BAR_WIDTH 50


#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"
#define COLOR_CYAN    "\033[36m"

void print_banner() {
    printf("\n%s", COLOR_CYAN);
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                      DROPBOX CLIENT v2.0                     ║\n");
    printf("║                  Secure Cloud Storage                        ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("%s\n", COLOR_RESET);
}

void print_menu() {
    printf("\n%s", COLOR_BLUE);
    printf("┌──────────────────────────────────────────────────────────────┐\n");
    printf("│                         MAIN MENU                            │\n");
    printf("├──────────────────────────────────────────────────────────────┤\n");
    printf("│      UPLOAD   - Upload file to cloud storage                 │\n");
    printf("│      DOWNLOAD - Download file from storage                   │\n");
    printf("│      DELETE   - Remove file from storage                     │\n");
    printf("│      LIST     - View all your files                          │\n");
    printf("│      EXIT     - Quit application                             │\n");
    printf("└──────────────────────────────────────────────────────────────┘\n");
    printf("%s", COLOR_RESET);
}

void print_success(const char *message) {
    printf("%s %s%s\n", COLOR_GREEN, message, COLOR_RESET);
}

void print_error(const char *message) {
    printf("%s %s%s\n", COLOR_RED, message, COLOR_RESET);
}

void print_info(const char *message) {
    printf("%s  %s%s\n", COLOR_YELLOW, message, COLOR_RESET);
}

void show_progress(long current, long total, const char *operation) {
    if (total == 0) return;
   
    int percentage = (int)((current * 100) / total);
    int bars = (percentage * PROGRESS_BAR_WIDTH) / 100;
   
    printf("\r%s%s: [", COLOR_MAGENTA, operation);
    for (int i = 0; i < PROGRESS_BAR_WIDTH; i++) {
        if (i < bars) printf("█");
        else printf(" ");
    }
    printf("] %d%% (%ld/%ld bytes)", percentage, current, total);
    fflush(stdout);
   
    if (percentage == 100) {
        printf("%s\n", COLOR_RESET);
    }
}

int send_all(int sock, const void *buf, size_t len) {
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t s = send(sock, p, left, 0);
        if (s <= 0) return -1;
        p += s;
        left -= s;
    }
    return 0;
}

void send_file(int sock, const char *filename) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        print_error("File not found");
        return;
    }
   
    // Get file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
   
    printf("Uploading %s (%ld bytes)...\n", filename, file_size);
   
    char buffer[BUF_SIZE];
    long total_sent = 0;
    int bytes;
   
    while ((bytes = fread(buffer, 1, BUF_SIZE, fp)) > 0) {
        if (send_all(sock, buffer, bytes) < 0) {
            print_error("Upload failed");
            fclose(fp);
            return;
        }
        total_sent += bytes;
        show_progress(total_sent, file_size, "Uploading");
    }
    fclose(fp);
   
    // Send EOF marker (important!)
    send_all(sock, "EOF", 3);
    print_success("File uploaded successfully");
}

void receive_file(int sock, const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        print_error("Cannot create file");
        return;
    }
   
    printf("Downloading %s...\n", filename);
   
    char buffer[BUF_SIZE];
    long total_received = 0;
    int eof_found = 0;
   
    while (!eof_found) {
        ssize_t bytes = recv(sock, buffer, BUF_SIZE, 0);
        if (bytes <= 0) break;
       
        // Check for EOF marker
        if (bytes >= 3) {
            for (int i = 0; i <= bytes - 3; i++) {
                if (memcmp(buffer + i, "EOF", 3) == 0) {
                    // Write data before EOF
                    if (i > 0) {
                        fwrite(buffer, 1, i, fp);
                        total_received += i;
                    }
                    eof_found = 1;
                    break;
                }
            }
        }
       
        if (!eof_found) {
            fwrite(buffer, 1, bytes, fp);
            total_received += bytes;
        }
       
        show_progress(total_received, total_received + BUF_SIZE, "Downloading");
    }
    fclose(fp);
   
    if (eof_found && total_received > 0) {
        print_success("File downloaded successfully");
    } else {
        print_error("Download failed or file not found");
    }
}

int authenticate(int sock) {
    char buf[BUF_SIZE];
    char username[64], password[64];
    int authenticated = 0;
   
    printf("\n%s", COLOR_CYAN);
    printf("┌──────────────────────────────────────────────────────────────┐\n");
    printf("│                       AUTHENTICATION                         │\n");
    printf("├──────────────────────────────────────────────────────────────┤\n");
    printf("│  1. LOGIN  - I have an account                               │\n");
    printf("│  2. SIGNUP - Create new account                              │\n");
    printf("└──────────────────────────────────────────────────────────────┘\n");
    printf("%s", COLOR_RESET);
   
    while (!authenticated) {
        printf("\nChoose option (1 or 2): ");
        if (!fgets(buf, sizeof(buf), stdin)) continue;
       
        if (strcmp(buf, "1\n") == 0) {
            printf("Username: ");
            if (!fgets(username, sizeof(username), stdin)) continue;
            username[strcspn(username, "\n")] = 0;
           
            printf("Password: ");
            if (!fgets(password, sizeof(password), stdin)) continue;
            password[strcspn(password, "\n")] = 0;
           
            snprintf(buf, sizeof(buf), "LOGIN %s %s\n", username, password);
            send_all(sock, buf, strlen(buf));
           
            memset(buf, 0, sizeof(buf));
            recv(sock, buf, sizeof(buf), 0);
           
            if (strncmp(buf, "OK", 2) == 0) {
                print_success("Login successful!");
                authenticated = 1;
            } else {
                print_error("Login failed. Check your credentials.");
            }
        }
        else if (strcmp(buf, "2\n") == 0) {
            printf("Choose username: ");
            if (!fgets(username, sizeof(username), stdin)) continue;
            username[strcspn(username, "\n")] = 0;
           
            printf("Choose password: ");
            if (!fgets(password, sizeof(password), stdin)) continue;
            password[strcspn(password, "\n")] = 0;
           
            snprintf(buf, sizeof(buf), "SIGNUP %s %s\n", username, password);
            send_all(sock, buf, strlen(buf));
           
            memset(buf, 0, sizeof(buf));
            recv(sock, buf, sizeof(buf), 0);
           
            if (strncmp(buf, "OK", 2) == 0) {
                print_success("Account created successfully! You can now login.");
            } else {
                print_error("Username already exists. Please choose another.");
            }
        }
        else {
            print_error("Please choose 1 or 2");
        }
    }
    return authenticated;
}

void handle_list(int sock) {
    char buf[BUF_SIZE];
   
    // Send LIST command
    send_all(sock, "LIST\n", 5);
   
    printf("\n%s", COLOR_YELLOW);
    printf("┌──────────────────────────────────────────────────────────────┐\n");
    printf("│                         YOUR FILES                           │\n");
    printf("├──────────────────────────────────────────────────────────────┤\n");
    printf("%s", COLOR_RESET);
   
    // Receive list data until we get END_OF_LIST marker
    while (1) {
        memset(buf, 0, sizeof(buf));
        ssize_t bytes = recv(sock, buf, sizeof(buf) - 1, 0);
       
        if (bytes <= 0) {
            break;
        }
       
        // Check if we received the END_OF_LIST marker
        if (strstr(buf, "END_OF_LIST") != NULL) {
            // Print everything before END_OF_LIST
            char *end_marker = strstr(buf, "END_OF_LIST");
            if (end_marker != buf) { // If there's data before the marker
                *end_marker = '\0'; // Terminate the string at the marker
                printf("%s", buf);
            }
            break;
        }
       
        printf("%s", buf);
    }
   
    printf("%s", COLOR_YELLOW);
    printf("└──────────────────────────────────────────────────────────────┘\n");
    printf("%s", COLOR_RESET);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    int sock;
    struct sockaddr_in serv_addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(atoi(argv[2]));
    inet_pton(AF_INET, argv[1], &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        print_error("Connection to server failed");
        return 1;
    }

    print_banner();
    print_success("Connected to Dropbox server!");

    if (!authenticate(sock)) {
        close(sock);
        return 1;
    }

    char buf[BUF_SIZE];
    while (1) {
        print_menu();
        printf("\nEnter command: ");
       
        if (!fgets(buf, sizeof(buf), stdin)) continue;
        buf[strcspn(buf, "\n")] = 0;

        if (strncasecmp(buf, "UPLOAD", 6) == 0) {
            char *fname = strchr(buf, ' ');
            if (fname) {
                fname++;
                // First send the UPLOAD command
                char cmd[BUF_SIZE];
                snprintf(cmd, sizeof(cmd), "UPLOAD %s\n", fname);
                send_all(sock, cmd, strlen(cmd));
                // Small delay to ensure command is processed
                usleep(100000);
                // Then send the file data
                send_file(sock, fname);
            } else {
                print_error("Usage: UPLOAD <filename>");
            }
        }
        else if (strncasecmp(buf, "DOWNLOAD", 8) == 0) {
            char *fname = strchr(buf, ' ');
            if (fname) {
                fname++;
                // Send the DOWNLOAD command with filename
                char cmd[BUF_SIZE];
                snprintf(cmd, sizeof(cmd), "DOWNLOAD %s\n", fname);
                send_all(sock, cmd, strlen(cmd));
                // Then receive the file
                receive_file(sock, fname);
            } else {
                print_error("Usage: DOWNLOAD <filename>");
            }
        }
        else if (strncasecmp(buf, "DELETE", 6) == 0) {
            char *fname = strchr(buf, ' ');
            if (fname) {
                fname++;
                // Send DELETE command
                char cmd[BUF_SIZE];
                snprintf(cmd, sizeof(cmd), "DELETE %s\n", fname);
                send_all(sock, cmd, strlen(cmd));
               
                // Receive response
                char response[256];
                memset(response, 0, sizeof(response));
                ssize_t bytes = recv(sock, response, sizeof(response) - 1, 0);
               
                if (bytes > 0) {
                    response[bytes] = '\0';
                    // Remove newline characters
                    response[strcspn(response, "\n\r")] = '\0';
                   
                    if (strncmp(response, "OK", 2) == 0) {
                        print_success("File deleted successfully");
                    } else if (strncmp(response, "ERR", 3) == 0) {
                        // Extract error message
                        char *err_msg = response + 4; // Skip "ERR "
                        print_error(err_msg);
                    } else {
                        printf("DEBUG: Received response: '%s'\n", response); // Debug line
                        print_error("Unexpected response from server");
                    }
                } else {
                    print_error("No response from server");
                }
            } else {
                print_error("Usage: DELETE <filename>");
            }
        }
        else if (strncasecmp(buf, "LIST", 4) == 0) {
            handle_list(sock);
        }
        else if (strncasecmp(buf, "EXIT", 4) == 0 || strncasecmp(buf, "QUIT", 4) == 0) {
            send_all(sock, "QUIT\n", 5);
            print_success("Goodbye! ");
            break;
        }
        else if (strlen(buf) > 0) {
            print_error("Unknown command. Available: UPLOAD, DOWNLOAD, DELETE, LIST, EXIT");
        }
    }

    close(sock);
    return 0;
}

