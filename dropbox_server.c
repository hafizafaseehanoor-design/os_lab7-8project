#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>

#define PORT 8080
#define BACKLOG 16
#define CLIENT_POOL_SIZE 4
#define WORKER_POOL_SIZE 4
#define CLIENT_Q_CAP 256
#define MAX_FILENAME 256
#define TMP_DIR "tmp_storage"
#define STORAGE_DIR "storage"
#define USERNAME_MAX 64
#define PASS_MAX 64
#define MAX_QUOTA (50 * 1024 * 1024)

static volatile sig_atomic_t running = 1;
static void sigint_handler(int s) { (void)s; running = 0; }

static void perror_exit(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return;
        fprintf(stderr, "Path exists and is not a directory: %s\n", path);
        exit(1);
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) perror_exit("mkdir");
}

typedef struct FileNode {
    char *name;
    size_t size;
    struct FileNode *next;
} FileNode;

typedef struct User {
    char username[USERNAME_MAX];
    char password[PASS_MAX];
    size_t used;
    FileNode *files;
    pthread_mutex_t ulock;
    struct User *next;
} User;

static User *users = NULL;
static pthread_mutex_t users_mutex = PTHREAD_MUTEX_INITIALIZER;

static User *user_find_locked(const char *username) {
    User *u = users;
    while (u) {
        if (strcmp(u->username, username) == 0) return u;
        u = u->next;
    }
    return NULL;
}

int user_create(const char *username, const char *password) {
    pthread_mutex_lock(&users_mutex);
    if (user_find_locked(username) != NULL) {
        pthread_mutex_unlock(&users_mutex);
        return -1;
    }
    User *u = calloc(1, sizeof(User));
    if (!u) { pthread_mutex_unlock(&users_mutex); return -1; }
    strncpy(u->username, username, USERNAME_MAX-1);
    strncpy(u->password, password, PASS_MAX-1);
    u->used = 0; u->files = NULL;
    pthread_mutex_init(&u->ulock, NULL);
    u->next = users; users = u;
    pthread_mutex_unlock(&users_mutex);

    char path[512];
    ensure_dir(STORAGE_DIR);
    snprintf(path, sizeof(path), "%s/%s", STORAGE_DIR, username);
    ensure_dir(path);
    return 0;
}

int user_check_password(const char *username, const char *password) {
    pthread_mutex_lock(&users_mutex);
    User *u = user_find_locked(username);
    if (!u) { pthread_mutex_unlock(&users_mutex); return -1; }
    int ok = (strcmp(u->password, password) == 0);
    pthread_mutex_unlock(&users_mutex);
    return ok ? 0 : -1;
}

void user_add_file(const char *username, const char *filename, size_t size) {
    pthread_mutex_lock(&users_mutex);
    User *u = user_find_locked(username);
    if (!u) { pthread_mutex_unlock(&users_mutex); return; }
    pthread_mutex_lock(&u->ulock);
    FileNode *f = calloc(1, sizeof(FileNode));
    f->name = strdup(filename);
    f->size = size;
    f->next = u->files;
    u->files = f;
    u->used += size;
    pthread_mutex_unlock(&u->ulock);
    pthread_mutex_unlock(&users_mutex);
}

int user_remove_file(const char *username, const char *filename, size_t *out_size) {
    pthread_mutex_lock(&users_mutex);
    User *u = user_find_locked(username);
    if (!u) { pthread_mutex_unlock(&users_mutex); return -1; }
    pthread_mutex_lock(&u->ulock);
    FileNode **pp = &u->files;
    while (*pp) {
        if (strcmp((*pp)->name, filename) == 0) {
            FileNode *tmp = *pp;
            *pp = tmp->next;
            size_t sz = tmp->size;
            free(tmp->name); free(tmp);
            u->used -= sz;
            if (out_size) *out_size = sz;
            pthread_mutex_unlock(&u->ulock);
            pthread_mutex_unlock(&users_mutex);
            return 0;
        }
        pp = &((*pp)->next);
    }
    pthread_mutex_unlock(&u->ulock);
    pthread_mutex_unlock(&users_mutex);
    return -1;
}

char *user_list_files(const char *username) {
    pthread_mutex_lock(&users_mutex);
    User *u = user_find_locked(username);
    if (!u) { pthread_mutex_unlock(&users_mutex); return NULL; }
    pthread_mutex_lock(&u->ulock);
    size_t cap = 1024;
    char *buf = malloc(cap);
    if (!buf) { pthread_mutex_unlock(&u->ulock); pthread_mutex_unlock(&users_mutex); return NULL; }
    size_t len = 0;
    len += snprintf(buf+len, cap-len, "Storage used: %zu bytes\n", u->used);
    FileNode *f = u->files;
    while (f) {
        size_t needed = strlen(f->name) + 50;
        if (len + needed + 1 > cap) { cap *= 2; buf = realloc(buf, cap); }
        len += snprintf(buf+len, cap-len, "%s (%zu bytes)\n", f->name, f->size);
        f = f->next;
    }
    pthread_mutex_unlock(&u->ulock);
    pthread_mutex_unlock(&users_mutex);
    return buf;
}

enum TaskType { TASK_UPLOAD=1, TASK_DOWNLOAD=2, TASK_DELETE=3, TASK_LIST=4 };

typedef struct Task {
    enum TaskType type;
    char username[USERNAME_MAX];
    char filename[MAX_FILENAME];
    char tmp_path[512];
    size_t filesize;
    char *result_buf;
    size_t result_size;
    int status;
    char errmsg[256];

    pthread_mutex_t mutex;
    pthread_cond_t cond;

    struct Task *next;
} Task;

static Task *task_head = NULL;
static Task *task_tail = NULL;
static pthread_mutex_t taskq_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t taskq_cond = PTHREAD_COND_INITIALIZER;

void push_task(Task *t) {
    t->next = NULL;
    pthread_mutex_lock(&taskq_mutex);
    if (!task_tail) { task_head = task_tail = t; }
    else { task_tail->next = t; task_tail = t; }
    pthread_cond_signal(&taskq_cond);
    pthread_mutex_unlock(&taskq_mutex);
}

Task *pop_task() {
    pthread_mutex_lock(&taskq_mutex);
    while (!task_head) pthread_cond_wait(&taskq_cond, &taskq_mutex);
    Task *t = task_head;
    task_head = t->next;
    if (!task_head) task_tail = NULL;
    pthread_mutex_unlock(&taskq_mutex);
    return t;
}

typedef struct ClientQ {
    int fds[CLIENT_Q_CAP];
    int head, tail, count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} ClientQ;

static ClientQ clientq = { .head=0, .tail=0, .count=0, .mutex=PTHREAD_MUTEX_INITIALIZER, .cond=PTHREAD_COND_INITIALIZER };

void push_client_fd(int fd) {
    pthread_mutex_lock(&clientq.mutex);
    while (clientq.count == CLIENT_Q_CAP) pthread_cond_wait(&clientq.cond, &clientq.mutex);
    clientq.fds[clientq.tail] = fd;
    clientq.tail = (clientq.tail + 1) % CLIENT_Q_CAP;
    clientq.count++;
    pthread_cond_signal(&clientq.cond);
    pthread_mutex_unlock(&clientq.mutex);
}

int pop_client_fd() {
    pthread_mutex_lock(&clientq.mutex);
    while (clientq.count == 0) pthread_cond_wait(&clientq.cond, &clientq.mutex);
    int fd = clientq.fds[clientq.head];
    clientq.head = (clientq.head + 1) % CLIENT_Q_CAP;
    clientq.count--;
    pthread_cond_signal(&clientq.cond);
    pthread_mutex_unlock(&clientq.mutex);
    return fd;
}

ssize_t recv_line(int fd, char *buf, size_t maxlen) {
    size_t idx = 0;
    while (idx + 1 < maxlen) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0) return 0;
        if (r < 0) return -1;
        buf[idx++] = c;
        if (c == '\n') break;
    }
    buf[idx] = '\0';
    return (ssize_t)idx;
}

int recv_exact(int sock, char *buf, size_t n) {
    size_t left = n;
    while (left > 0) {
        ssize_t r = recv(sock, buf + (n - left), left, 0);
        if (r <= 0) return -1;
        left -= r;
    }
    return 0;
}

int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf; size_t left = len;
    while (left > 0) {
        ssize_t s = send(fd, p, left, 0);
        if (s <= 0) return -1;
        p += s; left -= s;
    }
    return 0;
}

static void safe_copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return; }
    char buf[8192];
    ssize_t r;
    while ((r = read(in, buf, sizeof(buf))) > 0) write(out, buf, r);
    close(in); close(out);
}

void handle_upload(Task *t) {
    pthread_mutex_lock(&users_mutex);
    User *u = user_find_locked(t->username);
    if (!u) { pthread_mutex_unlock(&users_mutex); t->status = -1; snprintf(t->errmsg, sizeof(t->errmsg), "User not found"); unlink(t->tmp_path); return; }
    pthread_mutex_lock(&u->ulock);
    if (u->used + t->filesize > MAX_QUOTA) {
        pthread_mutex_unlock(&u->ulock);
        pthread_mutex_unlock(&users_mutex);
        t->status = -1; snprintf(t->errmsg, sizeof(t->errmsg), "Quota exceeded"); unlink(t->tmp_path); return;
    }
    pthread_mutex_unlock(&u->ulock);
    pthread_mutex_unlock(&users_mutex);

    char dest[1024];
    snprintf(dest, sizeof(dest), "%s/%s/%s", STORAGE_DIR, t->username, t->filename);
    if (rename(t->tmp_path, dest) != 0) {
        safe_copy_file(t->tmp_path, dest);
        unlink(t->tmp_path);
    }
    user_add_file(t->username, t->filename, t->filesize);
    t->status = 0;
    t->result_buf = strdup("OK\n"); t->result_size = strlen(t->result_buf);
}

void handle_download(Task *t) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/%s", STORAGE_DIR, t->username, t->filename);
    int in = open(path, O_RDONLY);
    if (in < 0) { t->status = -1; snprintf(t->errmsg, sizeof(t->errmsg), "File not found"); return; }
    struct stat st; if (fstat(in, &st) != 0) { close(in); t->status = -1; snprintf(t->errmsg, sizeof(t->errmsg), "fstat failed"); return; }
    size_t sz = st.st_size;
    char *buf = malloc(sz);
    if (!buf) { close(in); t->status = -1; snprintf(t->errmsg, sizeof(t->errmsg), "OOM"); return; }
    size_t off = 0;
    while (off < sz) {
        ssize_t r = read(in, buf+off, sz-off);
        if (r <= 0) break;
        off += r;
    }
    close(in);
    if (off != sz) { free(buf); t->status = -1; snprintf(t->errmsg, sizeof(t->errmsg), "Partial read"); return; }
    t->status = 0;
    t->result_buf = buf; t->result_size = sz;
}

void handle_delete(Task *t) {
    printf("DEBUG: Handling delete for user '%s', file '%s'\n", t->username, t->filename);
   
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/%s", STORAGE_DIR, t->username, t->filename);
   
    printf("DEBUG: Attempting to delete file: %s\n", path);
   
    // Check if file exists first
    if (access(path, F_OK) != 0) {
        // Safe string copying
        strncpy(t->errmsg, "File not found", sizeof(t->errmsg) - 1);
        t->errmsg[sizeof(t->errmsg) - 1] = '\0';
        t->status = -1;
        printf("DEBUG: File does not exist\n");
        return;
    }
   
    if (unlink(path) != 0) {
        // Safe string copying
        strncpy(t->errmsg, strerror(errno), sizeof(t->errmsg) - 1);
        t->errmsg[sizeof(t->errmsg) - 1] = '\0';
        t->status = -1;
        printf("DEBUG: unlink failed: %s\n", strerror(errno));
        return;
    }
   
    size_t removed = 0;
    int remove_result = user_remove_file(t->username, t->filename, &removed);
    printf("DEBUG: user_remove_file returned: %d, removed size: %zu\n", remove_result, removed);
   
    t->status = 0;
    t->result_buf = strdup("OK\n");
    t->result_size = strlen(t->result_buf);
    printf("DEBUG: Delete completed successfully\n");
}

void handle_list(Task *t) {
    char *list = user_list_files(t->username);
    if (!list) { t->status = -1; snprintf(t->errmsg, sizeof(t->errmsg), "user not found"); return; }
    t->status = 0; t->result_buf = list; t->result_size = strlen(list);
}

void *worker_thread(void *arg) {
    (void)arg;
    while (running) {
        Task *t = pop_task();
        if (!t) continue;
        if (t->type == TASK_UPLOAD) handle_upload(t);
        else if (t->type == TASK_DOWNLOAD) handle_download(t);
        else if (t->type == TASK_DELETE) handle_delete(t);
        else if (t->type == TASK_LIST) handle_list(t);

        pthread_mutex_lock(&t->mutex);
        pthread_cond_signal(&t->cond);
        pthread_mutex_unlock(&t->mutex);
    }
    return NULL;
}

void send_error(int client_fd, const char *msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "ERR %s\n", msg);
    send_all(client_fd, buf, strlen(buf));
}

void send_ok(int client_fd) {
    send_all(client_fd, "OK\n", 3);
}

void client_service(int client_fd) {
    char buf[2048];
    char current_user[USERNAME_MAX] = "";
    int logged_in = 0;

    while (1) {
        ssize_t r = recv_line(client_fd, buf, sizeof(buf));
        if (r <= 0) { close(client_fd); return; }
        while (r>0 && (buf[r-1]=='\n' || buf[r-1]=='\r')) { buf[r-1]=0; r--; }
        if (r==0) continue;

        if (!logged_in) {
            if (strncmp(buf, "SIGNUP ", 7) == 0) {
                char user[USERNAME_MAX], pass[PASS_MAX];
                if (sscanf(buf+7, "%63s %63s", user, pass) != 2) { send_error(client_fd, "Usage: SIGNUP <user> <pass>"); continue; }
                if (user_create(user, pass) == 0) send_ok(client_fd); else send_error(client_fd, "User exists");
                continue;
            } else if (strncmp(buf, "LOGIN ", 6) == 0) {
                char user[USERNAME_MAX], pass[PASS_MAX];
                if (sscanf(buf+6, "%63s %63s", user, pass) != 2) { send_error(client_fd, "Usage: LOGIN <user> <pass>"); continue; }
                if (user_check_password(user, pass) == 0) {
                    strncpy(current_user, user, sizeof(current_user)-1);
                    logged_in = 1;
                    send_ok(client_fd);
                } else {
                    send_error(client_fd, "Invalid credentials");
                }
                continue;
            } else {
                send_error(client_fd, "Authenticate first with SIGNUP or LOGIN");
                continue;
            }
        }

        // Handle commands after login
        if (strncmp(buf, "UPLOAD ", 7) == 0) {
            char fname[MAX_FILENAME];
            if (sscanf(buf+7, "%255s", fname) != 1) {
                send_error(client_fd, "Usage: UPLOAD <filename>");
                continue;
            }
           
            // Create temp file for upload
            ensure_dir(TMP_DIR);
            char tmpfn[512];
            snprintf(tmpfn, sizeof(tmpfn), "%s/%s_%ld.tmp", TMP_DIR, fname, (long)time(NULL));
            int out = open(tmpfn, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out < 0) {
                send_error(client_fd, "Temp create failed");
                continue;
            }
           
            // Receive file data until EOF marker
            char file_buf[8192];
            size_t total_received = 0;
            int eof_found = 0;
           
            while (!eof_found) {
                ssize_t bytes = recv(client_fd, file_buf, sizeof(file_buf), 0);
                if (bytes <= 0) break;
               
                // Check for EOF marker in the received data
                if (bytes >= 3) {
                    for (int i = 0; i <= bytes - 3; i++) {
                        if (memcmp(file_buf + i, "EOF", 3) == 0) {
                            // Write data before EOF marker
                            if (i > 0) {
                                write(out, file_buf, i);
                                total_received += i;
                            }
                            eof_found = 1;
                            break;
                        }
                    }
                }
               
                if (!eof_found) {
                    write(out, file_buf, bytes);
                    total_received += bytes;
                }
            }
            close(out);
           
            if (total_received == 0) {
                send_error(client_fd, "No data received");
                unlink(tmpfn);
                continue;
            }
           
            // Create and process the upload task
            Task *t = calloc(1, sizeof(Task));
            pthread_mutex_init(&t->mutex, NULL);
            pthread_cond_init(&t->cond, NULL);
            t->type = TASK_UPLOAD;
            strncpy(t->username, current_user, sizeof(t->username)-1);
            strncpy(t->filename, fname, sizeof(t->filename)-1);
            strncpy(t->tmp_path, tmpfn, sizeof(t->tmp_path)-1);
            t->filesize = total_received;
            t->status = -1;

            push_task(t);

            pthread_mutex_lock(&t->mutex);
            pthread_cond_wait(&t->cond, &t->mutex);
            pthread_mutex_unlock(&t->mutex);

            if (t->status == 0) send_ok(client_fd);
            else send_error(client_fd, t->errmsg[0] ? t->errmsg : "UPLOAD failed");

            if (t->result_buf) free(t->result_buf);
            pthread_mutex_destroy(&t->mutex);
            pthread_cond_destroy(&t->cond);
            free(t);
            continue;
        }
        else if (strncmp(buf, "DOWNLOAD ", 9) == 0) {
            char fname[MAX_FILENAME];
            if (sscanf(buf+9, "%255s", fname) != 1) {
                send_error(client_fd, "Usage: DOWNLOAD <filename>");
                continue;
            }
           
            Task *t = calloc(1, sizeof(Task));
            pthread_mutex_init(&t->mutex, NULL);
            pthread_cond_init(&t->cond, NULL);
            t->type = TASK_DOWNLOAD;
            strncpy(t->username, current_user, sizeof(t->username)-1);
            strncpy(t->filename, fname, sizeof(t->filename)-1);
            t->status = -1;
           
            push_task(t);

            pthread_mutex_lock(&t->mutex);
            pthread_cond_wait(&t->cond, &t->mutex);
            pthread_mutex_unlock(&t->mutex);

            if (t->status != 0) {
                send_error(client_fd, t->errmsg);
                if (t->result_buf) free(t->result_buf);
                pthread_mutex_destroy(&t->mutex);
                pthread_cond_destroy(&t->cond);
                free(t);
                continue;
            }

            // Send file data
            if (t->result_size > 0) {
                send_all(client_fd, t->result_buf, t->result_size);
            }
            // Send EOF marker
            send_all(client_fd, "EOF", 3);

            if (t->result_buf) free(t->result_buf);
            pthread_mutex_destroy(&t->mutex);
            pthread_cond_destroy(&t->cond);
            free(t);
            continue;
        }
        else if (strncmp(buf, "DELETE ", 7) == 0) {
            char fname[MAX_FILENAME];
            if (sscanf(buf+7, "%255s", fname) != 1) {
                send_error(client_fd, "Usage: DELETE <filename>");
                continue;
            }
           
            printf("DEBUG: User '%s' deleting file '%s'\n", current_user, fname); // Debug line
           
            Task *t = calloc(1, sizeof(Task));
            pthread_mutex_init(&t->mutex, NULL);
            pthread_cond_init(&t->cond, NULL);
            t->type = TASK_DELETE;
            strncpy(t->username, current_user, sizeof(t->username)-1);
            strncpy(t->filename, fname, sizeof(t->filename)-1);
            t->status = -1;
           
            push_task(t);
            pthread_mutex_lock(&t->mutex);
            pthread_cond_wait(&t->cond, &t->mutex);
            pthread_mutex_unlock(&t->mutex);
           
            if (t->status == 0) {
                printf("DEBUG: Delete task completed successfully\n"); // Debug line
                send_ok(client_fd);
            } else {
                printf("DEBUG: Delete task failed: %s\n", t->errmsg); // Debug line
                send_error(client_fd, t->errmsg);
            }
           
            if (t->result_buf) free(t->result_buf);
            pthread_mutex_destroy(&t->mutex);
            pthread_cond_destroy(&t->cond);
            free(t);
            continue;
        }
        else if (strcmp(buf, "LIST") == 0) {
            Task *t = calloc(1, sizeof(Task));
            pthread_mutex_init(&t->mutex, NULL);
            pthread_cond_init(&t->cond, NULL);
            t->type = TASK_LIST;
            strncpy(t->username, current_user, sizeof(t->username)-1);
            t->status = -1;
           
            push_task(t);
            pthread_mutex_lock(&t->mutex);
            pthread_cond_wait(&t->cond, &t->mutex);
            pthread_mutex_unlock(&t->mutex);
           
            if (t->status == 0) {
                // Send the list data
                send_all(client_fd, t->result_buf, t->result_size);
                // Send END_OF_LIST marker on a new line
                send_all(client_fd, "END_OF_LIST\n", 12);
            } else {
                send_error(client_fd, t->errmsg);
            }
           
            if (t->result_buf) free(t->result_buf);
            pthread_mutex_destroy(&t->mutex);
            pthread_cond_destroy(&t->cond);
            free(t);
            continue;
        }
        else if (strcmp(buf, "QUIT") == 0 || strcmp(buf, "EXIT") == 0) {
            close(client_fd);
            return;
        }
        else {
            send_error(client_fd, "Unknown command");
        }
    }
}

void *client_worker_thread(void *arg) {
    (void)arg;
    while (running) {
        int fd = pop_client_fd();
        if (fd >= 0) client_service(fd);
    }
    return NULL;
}

int main(void) {
    signal(SIGINT, sigint_handler);
    ensure_dir(STORAGE_DIR); ensure_dir(TMP_DIR);

    // Create some test users
    user_create("hello", "hello1234");
    user_create("test", "test123");

    pthread_t workers[WORKER_POOL_SIZE];
    for (int i=0;i<WORKER_POOL_SIZE;i++) pthread_create(&workers[i], NULL, worker_thread, NULL);

    pthread_t clients[CLIENT_POOL_SIZE];
    for (int i=0;i<CLIENT_POOL_SIZE;i++) pthread_create(&clients[i], NULL, client_worker_thread, NULL);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) perror_exit("socket");
    int opt = 1; setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET; addr.sin_port = htons(PORT); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) perror_exit("bind");
    if (listen(listenfd, BACKLOG) < 0) perror_exit("listen");
    printf("Server listening on port %d\n", PORT);

    while (running) {
        struct sockaddr_in cli; socklen_t clilen = sizeof(cli);
        int conn = accept(listenfd, (struct sockaddr*)&cli, &clilen);
        if (conn < 0) { if (errno==EINTR) break; perror("accept"); continue; }
        push_client_fd(conn);
    }

    close(listenfd);
    printf("Server shutting down\n");
    return 0;
}
