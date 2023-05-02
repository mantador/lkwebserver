#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "lktables.h"
#include "lklib.h"
#include "lknet.h"

// local functions
void FD_SET_READ(int fd, LKHttpServer *server);
void FD_SET_WRITE(int fd, LKHttpServer *server);
void FD_CLR_READ(int fd, LKHttpServer *server);
void FD_CLR_WRITE(int fd, LKHttpServer *server);

void read_request(LKHttpServer *server, LKContext *ctx);
void read_cgi_output(LKHttpServer *server, LKContext *ctx);
void write_cgi_input(LKHttpServer *server, LKContext *ctx);
void process_request(LKHttpServer *server, LKContext *ctx);

void serve_files(LKHttpServer *server, LKContext *ctx, LKHostConfig *hc);
void serve_cgi(LKHttpServer *server, LKContext *ctx, LKHostConfig *hc);
void process_response(LKHttpServer *server, LKContext *ctx);
void process_error_response(LKHttpServer *server, LKContext *ctx, int status, char *msg);

void set_cgi_env1(LKHttpServer *server);
void set_cgi_env2(LKHttpServer *server, LKContext *ctx, LKHostConfig *hc);

void get_localtime_string(char *time_str, size_t time_str_len);
int open_path_file(char *home_dir, char *path);
int read_path_file(char *home_dir, char *path, LKBuffer *buf);
char *fileext(char *filepath);

void write_response(LKHttpServer *server, LKContext *ctx);
void terminate_fd(int fd, FDType fd_type, LKHttpServer *server);
void terminate_client_session(LKHttpServer *server, LKContext *ctx);

void serve_proxy(LKHttpServer *server, LKContext *ctx, char *targethost);
void write_proxy_request(LKHttpServer *server, LKContext *ctx);
void read_proxy_response(LKHttpServer *server, LKContext *ctx);
void write_proxy_response(LKHttpServer *server, LKContext *ctx);


/*** LKHttpServer functions ***/

LKHttpServer *lk_httpserver_new(LKConfig *cfg) {
    LKHttpServer *server = malloc(sizeof(LKHttpServer));
    server->cfg = cfg;
    server->ctxhead = NULL;
    server->maxfd = 0;
    return server;
}

void lk_httpserver_free(LKHttpServer *server) {
    lk_config_free(server->cfg);

    // Free ctx linked list
    LKContext *ctx = server->ctxhead;
    while (ctx != NULL) {
        LKContext *ptmp = ctx;
        ctx = ctx->next;
        lk_context_free(ptmp);
    }

    memset(server, 0, sizeof(LKHttpServer));
    free(server);
}

void FD_SET_READ(int fd, LKHttpServer *server) {
    FD_SET(fd, &server->readfds);
    if (fd > server->maxfd) {
        server->maxfd = fd;
    }
}
void FD_SET_WRITE(int fd, LKHttpServer *server) {
    FD_SET(fd, &server->writefds);
    if (fd > server->maxfd) {
        server->maxfd = fd;
    }
}
void FD_CLR_READ(int fd, LKHttpServer *server) {
    FD_CLR(fd, &server->readfds);
}
void FD_CLR_WRITE(int fd, LKHttpServer *server) {
    FD_CLR(fd, &server->writefds);
}

int lk_httpserver_serve(LKHttpServer *server) {
    int z;
    LKConfig *cfg = server->cfg;
    lk_config_finalize(cfg);

    int backlog = 50;
    struct sockaddr sa;
    int s0 = lk_open_listen_socket(cfg->serverhost->s, cfg->port->s, backlog, &sa);
    if (s0 == -1) {
        lk_print_err("lk_open_listen_socket() failed");
        return -1;
    }

    LKString *server_ipaddr_str = lk_get_ipaddr_string(&sa);
    printf("Serving HTTP on %s port %s...\n", server_ipaddr_str->s, cfg->port->s);
    lk_string_free(server_ipaddr_str);

    clearenv();
    set_cgi_env1(server);

    FD_ZERO(&server->readfds);
    FD_ZERO(&server->writefds);
    FD_SET_READ(s0, server);

    while (1) {
        // readfds contain the master list of read sockets
        fd_set cur_readfds = server->readfds;
        fd_set cur_writefds = server->writefds;
        z = select(server->maxfd+1, &cur_readfds, &cur_writefds, NULL, NULL);
        if (z == -1 && errno == EINTR) {
            continue;
        }
        if (z == -1) {
            lk_print_err("select()");
            return z;
        }
        if (z == 0) {
            // timeout returned
            continue;
        }

        // fds now contain list of clients with data available to be read.
        for (int i=0; i <= server->maxfd; i++) {
            if (FD_ISSET(i, &cur_readfds)) {
                // New client connection
                if (i == s0) {
                    socklen_t sa_len = sizeof(struct sockaddr_in);
                    struct sockaddr_in sa;
                    int clientfd = accept(s0, (struct sockaddr*)&sa, &sa_len);
                    if (clientfd == -1) {
                        lk_print_err("accept()");
                        continue;
                    }

                    // Add new client socket to list of read sockets.
                    FD_SET_READ(clientfd, server);

                    LKContext *ctx = create_initial_context(clientfd, &sa);
                    add_new_client_context(&server->ctxhead, ctx);
                    continue;
                } else {
                    //printf("read fd %d\n", i);

                    int selectfd = i;
                    LKContext *ctx = match_select_ctx(server->ctxhead, selectfd);
                    if (ctx == NULL) {
                        printf("read selectfd %d not in ctx list\n", selectfd);
                        terminate_fd(selectfd, FD_SOCK, server);
                        continue;
                    }

                    if (ctx->type == CTX_READ_REQ) {
                        read_request(server, ctx);
                    } else if (ctx->type == CTX_READ_CGI_OUTPUT) {
                        read_cgi_output(server, ctx);
                    } else if (ctx->type == CTX_PROXY_READ_RESP) {
                        read_proxy_response(server, ctx);
                    } else {
                        printf("read selectfd %d with unknown ctx type %d\n", selectfd, ctx->type);
                    }
                }
            } else if (FD_ISSET(i, &cur_writefds)) {
                //printf("write fd %d\n", i);

                int selectfd = i;
                LKContext *ctx = match_select_ctx(server->ctxhead, selectfd);
                if (ctx == NULL) {
                    printf("write selectfd %d not in ctx list\n", selectfd);
                    terminate_fd(selectfd, FD_SOCK, server);
                    continue;
                }

                if (ctx->type == CTX_WRITE_RESP) {
                    assert(ctx->resp != NULL);
                    assert(ctx->resp->head != NULL);
                    write_response(server, ctx);
                } else if (ctx->type == CTX_WRITE_CGI_INPUT) {
                    write_cgi_input(server, ctx);
                } else if (ctx->type == CTX_PROXY_WRITE_REQ) {
                    assert(ctx->req != NULL);
                    assert(ctx->req->head != NULL);
                    write_proxy_request(server, ctx);
                } else if (ctx->type == CTX_PROXY_WRITE_RESP) {
                    assert(ctx->proxy_respbuf != NULL);
                    write_proxy_response(server, ctx);
                } else {
                    printf("write selectfd %d with unknown ctx type %d\n", selectfd, ctx->type);
                }
            }
        }
    } // while (1)

    return 0;
}

// Sets the cgi environment variables that stay the same across http requests.
void set_cgi_env1(LKHttpServer *server) {
    int z;
    LKConfig *cfg = server->cfg;

    char hostname[LK_BUFSIZE_SMALL];
    z = gethostname(hostname, sizeof(hostname)-1);
    if (z == -1) {
        lk_print_err("gethostname()");
        hostname[0] = '\0';
    }
    hostname[sizeof(hostname)-1] = '\0';
    
    setenv("SERVER_NAME", hostname, 1);
    setenv("SERVER_SOFTWARE", "littlekitten/0.1", 1);
    setenv("SERVER_PROTOCOL", "HTTP/1.0", 1);
    setenv("SERVER_PORT", cfg->port->s, 1);

}

// Sets the cgi environment variables that vary for each http request.
void set_cgi_env2(LKHttpServer *server, LKContext *ctx, LKHostConfig *hc) {
    LKHttpRequest *req = ctx->req;

    setenv("DOCUMENT_ROOT", hc->homedir_abspath->s, 1);

    char *http_user_agent = lk_stringtable_get(req->headers, "User-Agent");
    if (!http_user_agent) http_user_agent = "";
    setenv("HTTP_USER_AGENT", http_user_agent, 1);

    char *http_host = lk_stringtable_get(req->headers, "Host");
    if (!http_host) http_host = "";
    setenv("HTTP_HOST", http_host, 1);

    LKString *lkscript_filename = lk_string_new(hc->homedir_abspath->s);
    lk_string_append(lkscript_filename, req->path->s);
    setenv("SCRIPT_FILENAME", lkscript_filename->s, 1);
    lk_string_free(lkscript_filename);

    setenv("REQUEST_METHOD", req->method->s, 1);
    setenv("SCRIPT_NAME", req->path->s, 1);
    setenv("REQUEST_URI", req->uri->s, 1);
    setenv("QUERY_STRING", req->querystring->s, 1);

    char *content_type = lk_stringtable_get(req->headers, "Content-Type");
    if (content_type == NULL) {
        content_type = "";
    }
    setenv("CONTENT_TYPE", content_type, 1);

    char content_length[10];
    snprintf(content_length, sizeof(content_length), "%ld", req->body->bytes_len);
    content_length[sizeof(content_length)-1] = '\0';
    setenv("CONTENT_LENGTH", content_length, 1);

    setenv("REMOTE_ADDR", ctx->client_ipaddr->s, 1);
    char portstr[10];
    snprintf(portstr, sizeof(portstr), "%d", ctx->client_port);
    setenv("REMOTE_PORT", portstr, 1);
}

void read_request(LKHttpServer *server, LKContext *ctx) {
    char buf[LK_BUFSIZE_LARGE];
    size_t nread;
    int z = 0;

    while (1) {
        if (!ctx->reqparser->head_complete) {
            z = lk_socketreader_readline(ctx->sr, buf, sizeof(buf), &nread);
            if (z == -1) {
                lk_print_err("lksocketreader_readline()");
            }
            if (nread > 0) {
                assert(buf[nread] == '\0');
                lk_httprequestparser_parse_line(ctx->reqparser, buf);
            }
        } else {
            z = lk_socketreader_readbytes(ctx->sr, buf, sizeof(buf), &nread);
            if (z == -1) {
                lk_print_err("lksocketreader_readbytes()");
            }
            if (nread > 0) {
                lk_httprequestparser_parse_bytes(ctx->reqparser, buf, nread);
            }
        }
        // No more data coming in.
        if (ctx->sr->sockclosed) {
            ctx->reqparser->body_complete = 1;
        }
        if (ctx->reqparser->body_complete) {
            FD_CLR_READ(ctx->selectfd, server);
            shutdown(ctx->selectfd, SHUT_RD);
            process_request(server, ctx);
            break;
        }
        if (z == -1 || nread == 0) {
            break;
        }
    }
}

// Send cgi_inputbuf input bytes to cgi program stdin set in selectfd.
void write_cgi_input(LKHttpServer *server, LKContext *ctx) {
    assert(ctx->cgi_inputbuf != NULL);
    LKBuffer *buf = ctx->cgi_inputbuf;

    // Write as much input bytes as the cgi program will receive.
    if (buf->bytes_cur < buf->bytes_len) {
        int z = lk_write_buf(ctx->selectfd, buf);
        if (nonblocking_error(z)) {
            return;
        }
        if (z == -1) {
            lk_print_err("lk_write_buf()");
            terminate_fd(ctx->cgifd, FD_FILE, server);
            ctx->cgifd = 0;
            remove_selectfd_context(&server->ctxhead, ctx->selectfd);
        }
        return;
    }

    // Completed writing input bytes.
    FD_CLR_WRITE(ctx->selectfd, server);
    shutdown(ctx->selectfd, SHUT_WR);
    remove_selectfd_context(&server->ctxhead, ctx->selectfd);
}

// Read cgi output to cgi_outputbuf.
void read_cgi_output(LKHttpServer *server, LKContext *ctx) {
    int z = lk_read_buf(ctx->selectfd, ctx->cgi_outputbuf);
    if (nonblocking_error(z)) {
        return;
    }
    if (z == -1) {
        lk_print_err("lk_read_buf()");
        terminate_fd(ctx->cgifd, FD_FILE, server);
        ctx->cgifd = 0;
        process_error_response(server, ctx, 500, "Error processing CGI output.");
        return;
    }

    // EOF - finished reading cgi output.
    assert(z == 0);

    // Remove cgi output from read list.
    terminate_fd(ctx->cgifd, FD_FILE, server);
    ctx->cgifd = 0;

    parse_cgi_output(ctx->cgi_outputbuf, ctx->resp);
    process_response(server, ctx);
}

void process_request(LKHttpServer *server, LKContext *ctx) {
    char *hostname = lk_stringtable_get(ctx->req->headers, "Host");
    LKHostConfig *hc = lk_config_find_hostconfig(server->cfg, hostname);
    if (hc == NULL) {
        process_error_response(server, ctx, 404, "LittleKitten webserver: hostconfig not found.");
        return;
    }

    // Forward request to proxyhost if proxyhost specified.
    if (hc->proxyhost->s_len > 0) {
        serve_proxy(server, ctx, hc->proxyhost->s);
        return;
    }

    if (hc->homedir->s_len == 0) {
        process_error_response(server, ctx, 404, "LittleKitten webserver: hostconfig homedir not specified.");
        return;
    }

    // Replace path with any matching alias.
    char *match = lk_stringtable_get(hc->aliases, ctx->req->path->s);
    if (match != NULL) {
        lk_string_assign(ctx->req->path, match);
    }

    // Run cgi script if uri falls under cgidir
    if (hc->cgidir->s_len > 0 && lk_string_starts_with(ctx->req->path, hc->cgidir->s)) {
        serve_cgi(server, ctx, hc);
        return;
    }

    serve_files(server, ctx, hc);
    process_response(server, ctx);
}

// Generate an http response to an http request.
#define POSTTEST
void serve_files(LKHttpServer *server, LKContext *ctx, LKHostConfig *hc) {
    int z;
    static char *html_error_start = 
       "<!DOCTYPE html>\n"
       "<html>\n"
       "<head><title>Error response</title></head>\n"
       "<body><h1>Error response</h1>\n";
    static char *html_error_end =
       "</body></html>\n";

    LKHttpRequest *req = ctx->req;
    LKHttpResponse *resp = ctx->resp;
    LKString *method = req->method;
    LKString *path = req->path;

    if (lk_string_sz_equal(method, "GET") || lk_string_sz_equal(method, "HEAD")) {
        // For root, default to index.html, ...
        if (path->s_len == 0) {
            char *default_files[] = {"/index.html", "/index.htm", "/default.html", "/default.htm"};
            for (int i=0; i < sizeof(default_files) / sizeof(char *); i++) {
                z = read_path_file(hc->homedir->s, default_files[i], resp->body);
                if (z >= 0) {
                    lk_httpresponse_add_header(resp, "Content-Type", "text/html");
                    break;
                }
                // Update path with default file for File not found error message.
                lk_string_assign(path, default_files[i]);
            }
        } else {
            z = read_path_file(hc->homedir->s, path->s, resp->body);
            char *content_type = (char *) lk_lookup(mimetypes_tbl, fileext(path->s));
            if (content_type == NULL) {
                content_type = "text/plain";
            }
            lk_httpresponse_add_header(resp, "Content-Type", content_type);
        }
        if (z == -1) {
            // path not found
            resp->status = 404;
            lk_string_assign_sprintf(resp->statustext, "File not found '%s'", path->s);
            lk_httpresponse_add_header(resp, "Content-Type", "text/plain");
            lk_buffer_append_sprintf(resp->body, "File not found '%s'\n", path->s);
        }
        return;
    }
#ifdef POSTTEST
    if (lk_string_sz_equal(method, "POST")) {
        static char *html_start =
           "<!DOCTYPE html>\n"
           "<html>\n"
           "<head><title>Little Kitten Sample Response</title></head>\n"
           "<body>\n";
        static char *html_end =
           "</body></html>\n";

        lk_httpresponse_add_header(resp, "Content-Type", "text/html");
        lk_buffer_append(resp->body, html_start, strlen(html_start));
        lk_buffer_append_sz(resp->body, "<pre>\n");
        lk_buffer_append(resp->body, req->body->bytes, req->body->bytes_len);
        lk_buffer_append_sz(resp->body, "\n</pre>\n");
        lk_buffer_append(resp->body, html_end, strlen(html_end));
        return;
    }
#endif

    resp->status = 501;
    lk_string_assign_sprintf(resp->statustext, "Unsupported method ('%s')", method);

    lk_httpresponse_add_header(resp, "Content-Type", "text/html");
    lk_buffer_append(resp->body, html_error_start, strlen(html_error_start));
    lk_buffer_append_sprintf(resp->body, "<p>Error code %d.</p>\n", resp->status);
    lk_buffer_append_sprintf(resp->body, "<p>Message: Unsupported method ('%s').</p>\n", resp->statustext->s);
    lk_buffer_append(resp->body, html_error_end, strlen(html_error_end));
}

void serve_cgi(LKHttpServer *server, LKContext *ctx, LKHostConfig *hc) {
    LKHttpRequest *req = ctx->req;
    LKHttpResponse *resp = ctx->resp;
    char *path = req->path->s;

    LKString *cgifile = lk_string_new(hc->homedir->s);
    lk_string_append(cgifile, req->path->s);

    // cgi file not found
    if (!lk_file_exists(cgifile->s)) {
        lk_string_free(cgifile);

        resp->status = 404;
        lk_string_assign_sprintf(resp->statustext, "File not found '%s'", path);
        lk_httpresponse_add_header(resp, "Content-Type", "text/plain");
        lk_buffer_append_sprintf(resp->body, "File not found '%s'\n", path);

        process_response(server, ctx);
        return;
    }

    set_cgi_env2(server, ctx, hc);

    // cgi stdout and stderr are streamed to fd_out.
    //$$todo pass any request body to fd_in.
    int fd_in, fd_out;
    int z = lk_popen3(cgifile->s, &fd_in, &fd_out, NULL);
    lk_string_free(cgifile);
    if (z == -1) {
        resp->status = 500;
        lk_string_assign_sprintf(resp->statustext, "Server error '%s'", strerror(errno));
        lk_httpresponse_add_header(resp, "Content-Type", "text/plain");
        lk_buffer_append_sprintf(resp->body, "Server error '%s'\n", strerror(errno));
        process_response(server, ctx);
        return;
    }

    close(fd_in);

    // Read cgi output in select()
    ctx->selectfd = fd_out;
    ctx->cgifd = fd_out;
    ctx->type = CTX_READ_CGI_OUTPUT;
    ctx->cgi_outputbuf = lk_buffer_new(0);
    FD_SET_READ(ctx->selectfd, server);

    // If req is POST with body, pass it to cgi process stdin.
    if (req->body->bytes_len > 0) {
        LKContext *ctx_in = lk_context_new();
        add_context(&server->ctxhead, ctx_in);

        ctx_in->selectfd = fd_in;
        ctx_in->cgifd = fd_in;
        ctx_in->clientfd = ctx->clientfd;
        ctx_in->type = CTX_WRITE_CGI_INPUT;

        ctx_in->cgi_inputbuf = lk_buffer_new(0);
        lk_buffer_append(ctx_in->cgi_inputbuf, req->body->bytes, req->body->bytes_len);

        FD_SET_WRITE(ctx_in->selectfd, server);
    }
}

void process_response(LKHttpServer *server, LKContext *ctx) {
    LKHttpRequest *req = ctx->reqparser->req;
    LKHttpResponse *resp = ctx->resp;

    lk_httpresponse_finalize(resp);

    // Clear response body on HEAD request.
    if (lk_string_sz_equal(req->method, "HEAD")) {
        lk_buffer_clear(resp->body);
    }

    char time_str[TIME_STRING_SIZE];
    get_localtime_string(time_str, sizeof(time_str));

    printf("%s [%s] \"%s %s %s\" %d\n", 
        ctx->client_ipaddr->s, time_str,
        req->method->s, req->uri->s, resp->version->s,
        resp->status);
    if (resp->status >= 500 && resp->status < 600 && resp->statustext->s_len > 0) {
        printf("%s [%s] %d - %s\n", 
            ctx->client_ipaddr->s, time_str,
            resp->status, resp->statustext->s);
    }

    ctx->selectfd = ctx->clientfd;
    ctx->type = CTX_WRITE_RESP;
    FD_SET_WRITE(ctx->selectfd, server);
    return;
}

void process_error_response(LKHttpServer *server, LKContext *ctx, int status, char *msg) {
    LKHttpResponse *resp = ctx->resp;
    resp->status = status;
    lk_string_assign(resp->statustext, msg);
    lk_httpresponse_add_header(resp, "Content-Type", "text/plain");
    lk_buffer_append_sz(resp->body, msg);

    process_response(server, ctx);
}

// Open <home_dir>/<uri> file in nonblocking mode.
// Returns 0 for success, -1 for error.
int open_path_file(char *home_dir, char *path) {
    // full_path = home_dir + path
    // Ex. "/path/to" + "/index.html"
    LKString *full_path = lk_string_new(home_dir);
    lk_string_append(full_path, path);

    int z = open(full_path->s, O_RDONLY | O_NONBLOCK);
    lk_string_free(full_path);
    return z;
}

// Read <home_dir>/<uri> file into buffer.
// Return number of bytes read or -1 for error.
int read_path_file(char *home_dir, char *path, LKBuffer *buf) {
    // full_path = home_dir + path
    // Ex. "/path/to" + "/index.html"
    LKString *full_path = lk_string_new(home_dir);
    lk_string_append(full_path, path);
    int z = lk_readfile(full_path->s, buf);
    lk_string_free(full_path);
    return z;
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

// Return ptr to start of file extension within filepath.
// Ex. "path/to/index.html" returns "index.html"
char *fileext(char *filepath) {
    int filepath_len = strlen(filepath);
    // filepath of "" returns ext of "".
    if (filepath_len == 0) {
        return filepath;
    }

    char *p = filepath + strlen(filepath) - 1;
    while (p >= filepath) {
        if (*p == '.') {
            return p+1;
        }
        p--;
    }
    return filepath;
}

void write_response(LKHttpServer *server, LKContext *ctx) {
    LKHttpResponse *resp = ctx->resp;

    // Send as much response bytes as the client will receive.
    // Send response head bytes first, then response body bytes.
    if (resp->head->bytes_cur < resp->head->bytes_len) {
        int z = lk_send_buf(ctx->selectfd, resp->head);
        if (nonblocking_error(z)) {
            return;
        }
        if (z == -1) {
            lk_print_err("lk_send_buf()");
            terminate_client_session(server, ctx);
            return;
        }
    } else if (resp->body->bytes_cur < resp->body->bytes_len) {
        int z = lk_send_buf(ctx->selectfd, resp->body);
        if (nonblocking_error(z)) {
            return;
        }
        if (z == -1) {
            lk_print_err("lk_send_buf()");
            terminate_client_session(server, ctx);
            return;
        }
    } else {
        // Completed sending http response.
        terminate_client_session(server, ctx);
    }
}

void serve_proxy(LKHttpServer *server, LKContext *ctx, char *targethost) {
    int proxyfd = lk_open_connect_socket(targethost, "", NULL);
    if (proxyfd == -1) {
        lk_print_err("lk_open_connect_socket()");
        terminate_client_session(server, ctx);
        return;
    }

    lk_httprequest_finalize(ctx->req);
    ctx->proxyfd = proxyfd;
    ctx->selectfd = proxyfd;
    ctx->type = CTX_PROXY_WRITE_REQ;
    FD_SET_WRITE(proxyfd, server);
}

void write_proxy_request(LKHttpServer *server, LKContext *ctx) {
    LKHttpRequest *req = ctx->req;

    // Send as much request bytes as the proxy will receive.
    // Send request head bytes first, then request body bytes.
    if (req->head->bytes_cur < req->head->bytes_len) {
        int z = lk_send_buf(ctx->selectfd, req->head);
        if (nonblocking_error(z)) {
            return;
        }
        if (z == -1) {
            lk_print_err("lk_send_buf()");
            terminate_client_session(server, ctx);
            return;
        }
    } else if (req->body->bytes_cur < req->body->bytes_len) {
        int z = lk_send_buf(ctx->selectfd, req->body);
        if (nonblocking_error(z)) {
            return;
        }
        if (z == -1) {
            lk_print_err("lk_send_buf()");
            terminate_client_session(server, ctx);
            return;
        }
    } else {
        // Completed sending http request.
        FD_CLR_WRITE(ctx->selectfd, server);
        shutdown(ctx->selectfd, SHUT_WR);

        ctx->type = CTX_PROXY_READ_RESP;
        ctx->proxy_respbuf = lk_buffer_new(0);
        FD_SET_READ(ctx->selectfd, server);
    }
}

void read_proxy_response(LKHttpServer *server, LKContext *ctx) {
    int z = lk_recv_buf(ctx->selectfd, ctx->proxy_respbuf);
    if (nonblocking_error(z)) {
        return;
    }
    if (z == -1) {
        lk_print_err("lk_recv_buf()");
        terminate_client_session(server, ctx);
        return;
    }
    if (z == 0) {
        FD_CLR_READ(ctx->proxyfd, server);
        shutdown(ctx->selectfd, SHUT_RD);

        ctx->type = CTX_PROXY_WRITE_RESP;
        ctx->selectfd = ctx->clientfd;
        FD_SET_WRITE(ctx->clientfd, server);

        // Close proxyfd as we don't need it anymore.
        z = close(ctx->proxyfd);
        if (z == -1) {
            lk_print_err("close()");
        }
        if (z == 0) {
            ctx->proxyfd = 0;
        }
    }
}

void write_proxy_response(LKHttpServer *server, LKContext *ctx) {
    LKBuffer *buf = ctx->proxy_respbuf;

    // Send as much response bytes as the client will receive.
    if (buf->bytes_cur < buf->bytes_len) {
        int z = lk_send_buf(ctx->selectfd, buf);
        if (nonblocking_error(z)) {
            return;
        }
        if (z == -1) {
            lk_print_err("lk_send_buf()");
            terminate_client_session(server, ctx);
            return;
        }
    } else {
        // Completed sending proxy response.
        terminate_client_session(server, ctx);
    }
}

// Clear fd from select()'s, shutdown, and close.
void terminate_fd(int fd, FDType fd_type, LKHttpServer *server) {
    int z;
    FD_CLR_READ(fd, server);
    FD_CLR_WRITE(fd, server);

    if (fd_type == FD_SOCK) {
        z = shutdown(fd, SHUT_RDWR);
        if (z == -1) {
            lk_print_err("terminate_fd shutdown()");
        }
    }
    z = close(fd);
    if (z == -1) {
        lk_print_err("close()");
    }
}

// Disconnect from client.
void terminate_client_session(LKHttpServer *server, LKContext *ctx) {
    if (ctx->clientfd) {
        terminate_fd(ctx->clientfd, FD_SOCK, server);
    }
    if (ctx->cgifd) {
        terminate_fd(ctx->cgifd, FD_FILE, server);
    }
    if (ctx->proxyfd) {
        terminate_fd(ctx->proxyfd, FD_SOCK, server);
    }
    // Remove from linked list and free ctx.
    remove_client_context(&server->ctxhead, ctx->clientfd);
}

