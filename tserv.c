#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "netfuncs.h"

#define LISTEN_PORT "5000"
#define RESPONSE_LINE_MAXSIZE 2048

typedef struct clientctx {
    int sock;                   // client socket
    sockbuf_t *sb;              // input buffer for reading lines
    httpreq_t *req;             // http request being parsed

    char *partial_line;         // partial line from previous read
    int nlinesread;             // number of lines read so far
    int empty_line_parsed;      // flag indicating whether empty line received

    httpresp_t *resp;

    // buffer containing response head including blank line
    // with number of response head bytes sent so far
    buf_t *resphead;
    size_t resphead_bytes_sent;

    // number of resp body bytes sent so far
    size_t respbody_bytes_sent;

    struct clientctx *next;     // linked list to next client
} clientctx_t;

clientctx_t *clientctx_new(int sock);
void clientctx_free(clientctx_t *ctx);
void add_clientctx(clientctx_t **pphead, clientctx_t *ctx);
void remove_clientctx(clientctx_t **pphead, int sock);

void print_err(char *s);
void exit_err(char *s);
int is_empty_line(char *s);
int ends_with_newline(char *s);
char *append_string(char *dst, char *s);
void *addrinfo_sin_addr(struct addrinfo *addr);
void handle_sigint(int sig);
void handle_sigchld(int sig);

void read_request_from_client(int clientfd);
void process_line(clientctx_t *ctx, char *line);
void process_client_request(clientctx_t *ctx);
int is_valid_http_method(char *method);

void send_response_to_client(int clientfd);

clientctx_t *ctxhead = NULL;
fd_set readfds;
fd_set writefds;

int main(int argc, char *argv[]) {
    int z;

    signal(SIGINT, handle_sigint);
    signal(SIGCHLD, handle_sigchld);

    // Get this server's address.
    struct addrinfo hints, *servaddr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    z = getaddrinfo("localhost", LISTEN_PORT, &hints, &servaddr);
    if (z != 0) {
        exit_err("getaddrinfo()");
    }

    // Get human readable IP address string in servipstr.
    char servipstr[INET6_ADDRSTRLEN];
    const char *pz = inet_ntop(servaddr->ai_family, addrinfo_sin_addr(servaddr), servipstr, sizeof(servipstr));
    if (pz ==NULL) {
        exit_err("inet_ntop()");
    }

    int s0 = socket(servaddr->ai_family, servaddr->ai_socktype, servaddr->ai_protocol);
    if (s0 == -1) {
        exit_err("socket()");
    }

    int yes=1;
    z = setsockopt(s0, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (s0 == -1) {
        exit_err("setsockopt(SO_REUSEADDR)");
    }

    z = bind(s0, servaddr->ai_addr, servaddr->ai_addrlen);
    if (z != 0) {
        exit_err("bind()");
    }

    freeaddrinfo(servaddr);
    servaddr = NULL;

    z = listen(s0, 5);
    if (z != 0) {
        exit_err("listen()");
    }
    printf("Listening on %s:%s...\n", servipstr, LISTEN_PORT);

    FD_ZERO(&readfds);
    FD_ZERO(&writefds);

    FD_SET(s0, &readfds);
    int maxfd = s0;

    while (1) {
        // readfds contain the master list of read sockets
        fd_set cur_readfds = readfds;
        fd_set cur_writefds = writefds;
        z = select(maxfd+1, &cur_readfds, &cur_writefds, NULL, NULL);
        if (z == -1) {
            exit_err("select()");
        }
        if (z == 0) {
            // timeout returned
            continue;
        }

        // fds now contain list of clients with data available to be read.
        for (int i=0; i <= maxfd; i++) {
            if (FD_ISSET(i, &cur_readfds)) {
                // New client connection
                if (i == s0) {
                    struct sockaddr_in a;
                    socklen_t a_len = sizeof(a);
                    int newclientsock = accept(s0, (struct sockaddr*)&a, &a_len);
                    if (newclientsock == -1) {
                        print_err("accept()");
                        continue;
                    }

                    // Add new client socket to list of read sockets.
                    FD_SET(newclientsock, &readfds);
                    if (newclientsock > maxfd) {
                        maxfd = newclientsock;
                    }

                    printf("read fd: %d\n", newclientsock);
                    clientctx_t *ctx = clientctx_new(newclientsock);
                    add_clientctx(&ctxhead, ctx);
                    continue;
                } else {
                    // i contains client socket with data available to be read.
                    int clientfd = i;
                    read_request_from_client(clientfd);
                }
            } else if (FD_ISSET(i, &cur_writefds)) {
                printf("write fd %d\n", i);
                // i contains client socket ready to be written to.
                int clientfd = i;
                send_response_to_client(clientfd);
            }
        }
    } // while (1)

    // Code doesn't reach here.
    close(s0);
    return 0;
}

clientctx_t *clientctx_new(int sock) {
    clientctx_t *ctx = malloc(sizeof(clientctx_t));
    ctx->sock = sock;
    ctx->sb = sockbuf_new(sock, 0);
    ctx->req = httpreq_new();
    ctx->partial_line = NULL;
    ctx->nlinesread = 0;
    ctx->empty_line_parsed = 0;

    ctx->resp = httpresp_new();
    ctx->resphead = buf_new(0);

    ctx->resphead_bytes_sent = 0;
    ctx->respbody_bytes_sent = 0;
    ctx->next = NULL;
    return ctx;
}

void clientctx_free(clientctx_t *ctx) {
    sockbuf_free(ctx->sb);
    httpreq_free(ctx->req);
    httpresp_free(ctx->resp);

    if (ctx->partial_line) {
        free(ctx->partial_line);
    }

    ctx->sb = NULL;
    ctx->req = NULL;
    ctx->resp = NULL;
    ctx->partial_line = NULL;
    free(ctx);
}

// Add ctx to end of clientctx linked list. Skip if ctx sock already in list.
void add_clientctx(clientctx_t **pphead, clientctx_t *ctx) {
    assert(pphead != NULL);

    if (*pphead == NULL) {
        // first client
        *pphead = ctx;
    } else {
        // add client to end of clients list
        clientctx_t *p = *pphead;
        while (p->next != NULL) {
            // ctx sock already exists
            if (p->sock == ctx->sock) {
                return;
            }
            p = p->next;
        }
        p->next = ctx;
    }
}

// Remove ctx sock from clientctx linked list.
void remove_clientctx(clientctx_t **pphead, int sock) {
    assert(pphead != NULL);

    if (*pphead == NULL) {
        return;
    }
    // remove head ctx
    if ((*pphead)->sock == sock) {
        clientctx_t *tmp = *pphead;
        *pphead = (*pphead)->next;
        clientctx_free(tmp);
        return;
    }

    clientctx_t *p = *pphead;
    clientctx_t *prev = NULL;
    while (p != NULL) {
        if (p->sock == sock) {
            assert(prev != NULL);
            prev->next = p->next;
            clientctx_free(p);
            return;
        }

        prev = p;
        p = p->next;
    }
}

// Print the last error message corresponding to errno.
void print_err(char *s) {
    fprintf(stderr, "%s: %s\n", s, strerror(errno));
}
void exit_err(char *s) {
    print_err(s);
    exit(1);
}

// Return whether line is empty, ignoring whitespace chars ' ', \r, \n
int is_empty_line(char *s) {
    int slen = strlen(s);
    for (int i=0; i < slen; i++) {
        // Not an empty line if non-whitespace char is present.
        if (s[i] != ' ' && s[i] != '\n' && s[i] != '\r') {
            return 0;
        }
    }
    return 1;
}

// Return whether string ends with \n char.
int ends_with_newline(char *s) {
    int slen = strlen(s);
    if (slen == 0) {
        return 0;
    }
    if (s[slen-1] == '\n') {
        return 1;
    }
    return 0;
}

// Append s to dst, returning new ptr to dst.
char *append_string(char *dst, char *s) {
    int slen = strlen(s);
    if (slen == 0) {
        return dst;
    }

    int dstlen = strlen(dst);
    dst = realloc(dst, dstlen + slen + 1);
    strncpy(dst + dstlen, s, slen+1);
    assert(dst[dstlen+slen] == '\0');

    return dst;
}

// Return sin_addr or sin6_addr depending on address family.
void *addrinfo_sin_addr(struct addrinfo *addr) {
    // addr->ai_addr is either struct sockaddr_in* or sockaddr_in6* depending on ai_family
    if (addr->ai_family == AF_INET) {
        struct sockaddr_in *p = (struct sockaddr_in*) addr->ai_addr;
        return &(p->sin_addr);
    } else {
        struct sockaddr_in6 *p = (struct sockaddr_in6*) addr->ai_addr;
        return &(p->sin6_addr);
    }
}

void handle_sigint(int sig) {
    printf("SIGINT received\n");
    fflush(stdout);
    exit(0);
}

void handle_sigchld(int sig) {
    int tmp_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    errno = tmp_errno;
}

void read_request_from_client(int clientfd) {
    clientctx_t *ctx = ctxhead;
    while (ctx != NULL && ctx->sock != clientfd) {
        ctx = ctx->next;
    }
    if (ctx == NULL) {
        printf("read_request_from_client() clientfd %d not in clients list\n", clientfd);
        return;
    }
    assert(ctx->sock == clientfd);
    assert(ctx->sb->sock == clientfd);

    while (1) {
        char buf[1000];
        int z = sockbuf_readline(ctx->sb, buf, sizeof buf);
        if (z == 0) {
            break;
        }
        if (z == -1) {
            print_err("sockbuf_readline()");
            break;
        }
        assert(buf[z] == '\0');

        // If there's a previous partial line, combine it with current line.
        if (ctx->partial_line != NULL) {
            ctx->partial_line = append_string(ctx->partial_line, buf);
            if (ends_with_newline(ctx->partial_line)) {
                process_line(ctx, ctx->partial_line);

                free(ctx->partial_line);
                ctx->partial_line = NULL;
            }
            continue;
        }

        // If current line is only partial line (not newline terminated), remember it for
        // next read.
        if (!ends_with_newline(buf)) {
            ctx->partial_line = strdup(buf);
            continue;
        }

        // Current line is complete.
        process_line(ctx, buf);
    }
}

void process_line(clientctx_t *ctx, char *line) {
    if (ctx->nlinesread == 0) {
        httpreq_parse_request_line(ctx->req, line);
        ctx->nlinesread++;
        return;
    }

    if (is_empty_line(line)) {
        ctx->empty_line_parsed = 1;
        ctx->nlinesread++;

        httpreq_t *req = ctx->req;
        printf("127.0.0.1 [11/Mar/2023 14:05:46] \"%s %s HTTP/1.1\" %d\n", 
            req->method, req->uri, 200);

        if (ctx->resphead->bytes_len == 0) {
            process_client_request(ctx);
        }
        return;
    }

    if (!ctx->empty_line_parsed) {
        httpreq_parse_header_line(ctx->req, line);
        ctx->nlinesread++;
        return;
    }

    httpreq_append_body(ctx->req, line, strlen(line));
    ctx->nlinesread++;
}

void process_client_request(clientctx_t *ctx) {
    static char *html_error_start = 
       "<!DOCTYPE html>\n"
       "<html>\n"
       "<head><title>Error response</title></head>\n"
       "<body><h1>Error response</h1>\n";
    static char *html_error_end =
       "</body></html>\n";

    static char *html_sample =
       "<!DOCTYPE html>\n"
       "<html>\n"
       "<head><title>Little Kitten</title></head>\n"
       "<body><h1>Little Kitten webserver</h1>\n"
       "<p>Hello Little Kitten!</p>\n"
       "</body></html>\n";

    char line[RESPONSE_LINE_MAXSIZE];
    //int line_len;
    char *method = ctx->req->method ? ctx->req->method : "";

    // Invalid request method: 501 Unknown method ('GET2')
    if (!is_valid_http_method(method)) {
        ctx->resp->status = 501;
        ctx->resp->statustext = strdup("Unsupported method ('method here')");
        ctx->resp->version = strdup("HTTP/1.0");

        // Compose respbody buffer
        buf_append(ctx->resp->body, html_error_start, strlen(html_error_start));
        snprintf(line, RESPONSE_LINE_MAXSIZE, "<p>Error code: %d</p>\n", ctx->resp->status);
        buf_append(ctx->resp->body, line, strlen(line));
        snprintf(line, RESPONSE_LINE_MAXSIZE, "<p>Message: Unsupported method ('%s').</p>\n", method);
        buf_append(ctx->resp->body, line, strlen(line));
        buf_append(ctx->resp->body, html_error_end, strlen(html_error_end));

        httpresp_head_to_buf(ctx->resp, ctx->resphead);

        FD_SET(ctx->sock, &writefds);
        return;
    }

    if (!strcmp(method, "GET")) {
        ctx->resp->status = 200;
        ctx->resp->statustext = strdup("OK");
        ctx->resp->version = strdup("HTTP/1.0");
        buf_append(ctx->resp->body, html_sample, strlen(html_sample));

        httpresp_head_to_buf(ctx->resp, ctx->resphead);

        FD_SET(ctx->sock, &writefds);
    }
}

int is_valid_http_method(char *method) {
    if (method == NULL) {
        return 0;
    }

    if (!strcasecmp(method, "GET")      ||
        !strcasecmp(method, "POST")     || 
        !strcasecmp(method, "PUT")      || 
        !strcasecmp(method, "DELETE")   ||
        !strcasecmp(method, "HEAD"))  {
        return 1;
    }

    return 0;
}

void close_client(clientctx_t *ctx) {
    // Remove client from client list.
    clientctx_t *prev = NULL;
    clientctx_t *p = ctxhead;
    while (p != NULL) {
        if (p == ctx) {
            if (prev == NULL) {
                ctxhead = p->next;
            } else {
                prev->next = p->next;
            }
            break;
        }
        prev = p;
        p = p->next;
    }

    // Free ctx resources
    close(ctx->sock);
    sockbuf_free(ctx->sb);
    httpreq_free(ctx->req);
    if (ctx->partial_line) {
        free(ctx->partial_line);
    }
    free(ctx);
}

int is_supported_http_method(char *method) {
    if (method == NULL) {
        return 0;
    }

    if (!strcasecmp(method, "GET")      ||
        !strcasecmp(method, "HEAD"))  {
        return 1;
    }

    return 0;
}

void send_response_to_client(int clientfd) {
    clientctx_t *ctx = ctxhead;
    while (ctx != NULL && ctx->sock != clientfd) {
        ctx = ctx->next;
    }
    if (ctx == NULL) {
        printf("send_response_to_client() clientfd %d not in clients list\n", clientfd);
        return;
    }
    assert(ctx->sock == clientfd);
    assert(ctx->sb->sock == clientfd);

    // Send as much response bytes as the client will receive.
    // response header (resphead) gets sent first,
    // followed by response body (respbody).
    if (ctx->resphead_bytes_sent < ctx->resphead->bytes_len) {
        // send resphead
        int z = sock_send(ctx->sock,
            ctx->resphead->bytes + ctx->resphead_bytes_sent,
            ctx->resphead->bytes_len - ctx->resphead_bytes_sent);
        if (z == -1) {
            return;
        }
        if (z == -2) {
            FD_CLR(ctx->sock, &writefds);
            remove_clientctx(&ctxhead, ctx->sock);
            return;
        }
        ctx->resphead_bytes_sent += z;
    } else if (ctx->respbody_bytes_sent < ctx->resp->body->bytes_len) {
        // send respbody
        int z = sock_send(ctx->sock,
            ctx->resp->body->bytes + ctx->respbody_bytes_sent,
            ctx->resp->body->bytes_len - ctx->respbody_bytes_sent);
        if (z == -1) {
            return;
        }
        if (z == -2) {
            FD_CLR(ctx->sock, &writefds);
            remove_clientctx(&ctxhead, ctx->sock);
            return;
        }
        ctx->respbody_bytes_sent += z;
    } else {
        FD_CLR(ctx->sock, &readfds);
        FD_CLR(ctx->sock, &writefds);
        int z= shutdown(ctx->sock, SHUT_RDWR);
        if (z == -1) {
            print_err("shutdown()");
        }
        z = close(ctx->sock);
        if (z == -1) {
            print_err("close()");
        }
        remove_clientctx(&ctxhead, ctx->sock);
    }
}

