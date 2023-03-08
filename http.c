#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "http.h"

// Remove trailing CRLF or LF (\n) from string.
void chomp(char* s) {
    int slen = strlen(s);
    for (int i=slen-1; i >= 0; i--) {
        if (s[i] != '\n' && s[i] != '\r') {
            break;
        }
        // Replace \n and \r with null chars. 
        s[i] = '\0';
    }
}

httpreq_t *httpreq_new() {
    httpreq_t *req = malloc(sizeof(httpreq_t));
    req->method = NULL;
    req->uri = NULL;
    req->version = NULL;
    req->body = NULL;
    req->body_len = 0;

    req->headers_size = 10; // start with room for n headers
    req->headers_len = 0;
    req->headers = malloc(req->headers_size * sizeof(keyval_t));
}

void httpreq_free(httpreq_t *req) {
    if (req->method) free(req->method);
    if (req->uri) free(req->uri);
    if (req->version) free(req->version);
    if (req->body) free(req->body);
    free(req->headers);

    req->method = NULL;
    req->uri = NULL;
    req->version = NULL;
    req->body = NULL;
    req->headers = NULL;
    free(req);
}

// Parse initial request line
// Ex. GET /path/to/index.html HTTP/1.0
int httpreq_parse_request_line(httpreq_t *req, char *line) {
    char *toks[3];
    int ntoksread = 0;

    char *saveptr;
    char *delim = " \t";
    char *linetmp = strdup(line);
    chomp(linetmp);
    char *p = linetmp;
    while (ntoksread < 3) {
        toks[ntoksread] = strtok_r(p, delim, &saveptr);
        if (toks[ntoksread] == NULL) {
            break;
        }
        ntoksread++;
        p = NULL; // for the next call to strtok_r()
    }

    if (ntoksread < 3) {
        free(linetmp);
        return -1;
    }

    char *method = toks[0];
    char *uri = toks[1];
    char *version = toks[2];

    if (strcmp(method, "GET") != 0 &&
        strcmp(method, "POST") != 0 && 
        strcmp(method, "PUT") != 0 && 
        strcmp(method, "DELETE") != 0) {
        free(linetmp);
        return -1;
    }

    req->method = strdup(method);
    req->uri = strdup(uri);
    req->version = strdup(version);

    free(linetmp);
    return 0;
}

void httpreq_add_header(httpreq_t *req, char *k, char *v) {
    assert(req->headers_size >= req->headers_len);

    if (req->headers_len == req->headers_size) {
        req->headers_size += 10;
        req->headers = realloc(req->headers, req->headers_size * sizeof(keyval_t));
    }

    req->headers[req->headers_len].k = strdup(k);
    req->headers[req->headers_len].v = strdup(v);
    req->headers_len++;
}

// Parse header line
// Ex. User-Agent: browser
int httpreq_parse_header_line(httpreq_t *req, char *line) {
    char *saveptr;
    char *delim = ":";

    char *linetmp = strdup(line);
    chomp(linetmp);
    char *k = strtok_r(linetmp, delim, &saveptr);
    if (k == NULL) {
        free(linetmp);
        return -1;
    }
    char *v = strtok_r(NULL, delim, &saveptr);
    if (v == NULL) {
        free(linetmp);
        return -1;
    }

    // skip over leading whitespace
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    httpreq_add_header(req, k, v);

    free(linetmp);
    return 0;
}

int httpreq_append_body(httpreq_t *req, char *bytes, int bytes_len) {
    // First time setting the body.
    if (req->body == NULL) {
        req->body = malloc(bytes_len);
        if (req->body == NULL) {
            return -1;
        }
        memcpy(req->body, bytes, bytes_len);
        req->body_len = bytes_len;
        return 0;
    }

    // Append bytes to existing body.
    req->body = realloc(req->body, req->body_len + bytes_len);
    if (req->body == NULL) {
        return -1;
    }
    memcpy(req->body + req->body_len, bytes, bytes_len);
    req->body_len += bytes_len;
    return 0;
}

void httpreq_debugprint(httpreq_t *req) {
    if (req->method) {
        printf("method: %s\n", req->method);
    }
    if (req->uri) {
        printf("uri: %s\n", req->uri);
    }
    if (req->version) {
        printf("version: %s\n", req->version);
    }
    printf("headers_size: %ld\n", req->headers_size);
    printf("headers_len: %ld\n", req->headers_len);

    printf("Headers:\n");
    for (int i=0; i < req->headers_len; i++) {
        printf("%s: %s\n", req->headers[i].k, req->headers[i].v);
    }

    if (req->body) {
        printf("Body:\n");
        for (int i=0; i < req->body_len; i++) {
            putchar(req->body[i]);
        }
        printf("\n");
    }
}

