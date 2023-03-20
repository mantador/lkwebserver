#ifndef NETFUNCS_H
#define NETFUNCS_H

// sockbuf_t - Buffered input for sockets.
typedef struct {
    int sock;
    char *buf;
    size_t buf_size;
    size_t buf_len;
    unsigned int next_read_pos;
    int sockclosed;
} sockbuf_t;

// keyval_t - Key/Value pair
typedef struct {
    char *k;
    char *v;
} keyval_t;

// stringmap_t - Dynamic array of key-value fields
typedef struct {
    keyval_t *items;
    size_t items_len;
    size_t items_size;
} stringmap_t;

// buf_t - Dynamic bytes buffer
typedef struct {
    char *bytes;        // bytes buffer
    size_t bytes_cur;   // index to current buffer position
    size_t bytes_len;   // length of buffer
    size_t bytes_size;  // capacity of buffer in bytes
} buf_t;

// httpreq_t - HTTP Request struct
typedef struct {
    char *method;       // GET
    char *uri;          // /path/to/index.html
    char *version;      // HTTP/1.0
    stringmap_t *headers;
    buf_t *head;
    buf_t *body;
} httpreq_t;

// httpresp_t - HTTP Response struct
typedef struct {
    int status;         // 404
    char *statustext;   // File not found
    char *version;      // HTTP/1.0
    stringmap_t *headers;
    buf_t *head;
    buf_t *body;
} httpresp_t;

/** stringmap functions **/
stringmap_t *stringmap_new();
void stringmap_free(stringmap_t *sm);
void stringmap_set(stringmap_t *sm, char *k, char *v);

/** buf functions **/
buf_t *buf_new(size_t bytes_size);
void buf_free(buf_t *buf);
int buf_append(buf_t *buf, char *bytes, size_t len);
void buf_sprintf(buf_t *buf, const char *fmt, ...);
void buf_asprintf(buf_t *buf, const char *fmt, ...);

/** socket helper functions **/
ssize_t sock_recv(int sock, char *buf, size_t count);
ssize_t sock_send(int sock, char *buf, size_t count);
void set_sock_timeout(int sock, int nsecs, int ms);
void set_sock_nonblocking(int sock);

/** Other helper functions **/
// Remove trailing CRLF or LF (\n) from string.
void chomp(char* s);

// Read entire file into buf.
ssize_t readfile(char *filepath, buf_t *buf);

// Read entire file descriptor contents into buf.
ssize_t readfd(int fd, buf_t *buf);

// Append src to dest, allocating new memory in dest if needed.
// Return new pointer to dest.
char *astrncat(char *dest, char *src, size_t src_len);

/** sockbuf functions **/
sockbuf_t *sockbuf_new(int sock, size_t initial_size);
void sockbuf_free(sockbuf_t *sb);

// Read one line from socket using buffered input.
ssize_t sockbuf_readline(sockbuf_t *sb, char *dst, size_t dst_len);

// print state of sockbuf/buf
void sockbuf_debugprint(sockbuf_t *sb);
void debugprint_buf(char *buf, size_t buf_size);

/** httpreq functions **/
httpreq_t *httpreq_new();
void httpreq_free(httpreq_t *req);
// Parse http request initial line into req.
void httpreq_parse_request_line(httpreq_t *req, char *line);
// Parse an http header line into req.
void httpreq_parse_header_line(httpreq_t *req, char *line);
// Add a key/val http header into req.
void httpreq_add_header(httpreq_t *req, char *k, char *v);
// Append to req message body.
void httpreq_append_body(httpreq_t *req, char *bytes, int bytes_len);
// Print contents of req.
void httpreq_debugprint(httpreq_t *req);

/** httpresp functions **/
httpresp_t *httpresp_new();
void httpresp_free(httpresp_t *resp);
void httpresp_gen_headbuf(httpresp_t *resp);
void httpresp_debugprint(httpresp_t *resp);

#endif

