/*
 * A partial implementation of HTTP/1.0
 *
 * This code is mainly intended as a replacement for the book's 'tiny.c' server
 * It provides a *partial* implementation of HTTP/1.0 which can form a basis for
 * the assignment.
 *
 * @author G. Back for CS 3214 Spring 2018
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <assert.h>
#include <linux/limits.h>
#include <dirent.h>
#include <jansson.h>

#include "http.h"
#include "hexdump.h"
#include "socket.h"
#include "bufio.h"
#include "main.h"

// Need macros here because of the sizeof
#define CRLF "\r\n"
#define CR "\r"
#define STARTS_WITH(field_name, header) \
    (!strncasecmp(field_name, header, sizeof(header) - 1))

/*
 * Ideas and code were derived from Claude. 
 */
struct claims_ctx {
    char *grants;
};

/*
 * Ideas and code were derived from Claude.
 */
static int
extract_claims_cb(jwt_t *jwt, jwt_config_t *config)
{
    jwt_value_t jval;
    jwt_set_GET_JSON(&jval, NULL);
    jwt_value_error_t err = jwt_claim_get(jwt, &jval);
    if (err)
    {
        return err;
    }
    struct claims_ctx *ctx = config->ctx;
    ctx->grants = jval.json_val;
    return 0;
}

/* Parse HTTP request line, setting req_method, req_path, and req_version. */
static bool
http_parse_request(struct http_transaction *ta)
{
    size_t req_offset;
    ssize_t len = bufio_readline(ta->client->bufio, &req_offset);
    if (len < 2)       // error, EOF, or less than 2 characters
        return false;

    char *request = bufio_offset2ptr(ta->client->bufio, req_offset);
    request[len-2] = '\0';  // replace LF with 0 to ensure zero-termination
    char *endptr;
    char *method = strtok_r(request, " ", &endptr);
    if (method == NULL)
        return false;

    if (!strcmp(method, "GET"))
        ta->req_method = HTTP_GET;
    else if (!strcmp(method, "POST"))
        ta->req_method = HTTP_POST;
    else
        ta->req_method = HTTP_UNKNOWN;

    char *req_path = strtok_r(NULL, " ", &endptr);
    if (req_path == NULL)
        return false;

    ta->req_path = bufio_ptr2offset(ta->client->bufio, req_path);

    char *http_version = strtok_r(NULL, CR, &endptr);
    if (http_version == NULL)  // would be HTTP 0.9
        return false;

    // record client's HTTP version in request
    if (!strcmp(http_version, "HTTP/1.1"))
        ta->req_version = HTTP_1_1;
    else if (!strcmp(http_version, "HTTP/1.0"))
        ta->req_version = HTTP_1_0;
    else
        return false;

    return true;
}

/* Process HTTP headers. */
static bool
http_process_headers(struct http_transaction *ta)
{
    for (;;) {
        size_t header_offset;
        ssize_t len = bufio_readline(ta->client->bufio, &header_offset);
        if (len <= 0)
            return false;

        char *header = bufio_offset2ptr(ta->client->bufio, header_offset);
        if (len == 2 && STARTS_WITH(header, CRLF))       // empty CRLF
            return true;

        header[len-2] = '\0';
        /* Each header field consists of a name followed by a 
         * colon (":") and the field value. Field names are 
         * case-insensitive. The field value MAY be preceded by 
         * any amount of LWS, though a single SP is preferred.
         */
        char *endptr;
        char *field_name = strtok_r(header, ":", &endptr);
        if (field_name == NULL)
            return false;

        // skip white space
        char *field_value = endptr;
        while (*field_value == ' ' || *field_value == '\t')
            field_value++;

        // you may print the header like so
        // printf("Header: %s: %s\n", field_name, field_value);
        if (!strcasecmp(field_name, "Content-Length")) {
            ta->req_content_len = atoi(field_value);
        }

        /*
         * Ideas and code were derived from Claude. 
         */
        if (STARTS_WITH(field_name, "Connection"))
        {
            if (strcasestr(field_value, "close"))
            {
                ta->req_wants_close = true;
            }
        }

        /*
         * Ideas and code were derived from Claude.
         */
        if (STARTS_WITH(field_name, "Cookie"))
        {
            ta->cookie_header = strdup(field_value);
        }

        if (STARTS_WITH(field_name, "Range"))
        {
            long long start, end;
            if (sscanf(field_value, "bytes=%lld-%lld", &start, &end) == 2) {
                ta->req_has_range = true;
                ta->req_range_start = (off_t)start;
                ta->req_range_end = (off_t)end;
            } else if (sscanf(field_value, "bytes=%lld-", &start) == 1) {
                ta->req_has_range = true;
                ta->req_range_start = (off_t)start;
                ta->req_range_end = -1;
            }
        }

        /* Handle other headers here. Both field_value and field_name
         * are zero-terminated strings.
         */
    }
}

const int MAX_HEADER_LEN = 2048;

/* add a formatted header to the response buffer. */
void 
http_add_header(buffer_t * resp, char* key, char* fmt, ...)
{
    va_list ap;

    buffer_appends(resp, key);
    buffer_appends(resp, ": ");

    va_start(ap, fmt);
    char *error = buffer_ensure_capacity(resp, MAX_HEADER_LEN);
    int len = vsnprintf(error, MAX_HEADER_LEN, fmt, ap);
    resp->len += len > MAX_HEADER_LEN ? MAX_HEADER_LEN - 1 : len;
    va_end(ap);

    buffer_appends(resp, "\r\n");
}

/* add a content-length header. */
static void
add_content_length(buffer_t *res, size_t len)
{
    http_add_header(res, "Content-Length", "%ld", len);
}

/* start the response by writing the first line of the response 
 * to the response buffer.  Used in send_response_header */
static void
start_response(struct http_transaction * ta, buffer_t *res)
{
    buffer_init(res, 80);

    /* Hint: you must change this as you implement HTTP/1.1.
     * Respond with the highest version the client supports
     * as indicated in the version field of the request.
     *
     * Ideas and code were derived from Claude.
     */
    if (ta->req_version == HTTP_1_1)
    {
        buffer_appends(res, "HTTP/1.1 ");
    }
    else
    {
        buffer_appends(res, "HTTP/1.0 ");
    }

    switch (ta->resp_status) {
    case HTTP_OK:
        buffer_appends(res, "200 OK");
        break;
    case HTTP_PARTIAL_CONTENT:
        buffer_appends(res, "206 Partial Content");
        break;
    case HTTP_BAD_REQUEST:
        buffer_appends(res, "400 Bad Request");
        break;
    case HTTP_PERMISSION_DENIED:
        buffer_appends(res, "403 Permission Denied");
        break;
    case HTTP_NOT_FOUND:
        buffer_appends(res, "404 Not Found");
        break;
    case HTTP_METHOD_NOT_ALLOWED:
        buffer_appends(res, "405 Method Not Allowed");
        break;
    case HTTP_REQUEST_TIMEOUT:
        buffer_appends(res, "408 Request Timeout");
        break;
    case HTTP_REQUEST_TOO_LONG:
        buffer_appends(res, "414 Request Too Long");
        break;
    case HTTP_NOT_IMPLEMENTED:
        buffer_appends(res, "501 Not Implemented");
        break;
    case HTTP_SERVICE_UNAVAILABLE:
        buffer_appends(res, "503 Service Unavailable");
        break;
    case HTTP_RANGE_NOT_SATISFIABLE:
        buffer_appends(res, "416 Range Not Satisfiable");
        break;
    case HTTP_INTERNAL_ERROR:
        buffer_appends(res, "500 Internal Server Error");
        break;
    default:  /* else */
        buffer_appends(res, "500 This is not a valid status code."
                "Did you forget to set resp_status?");
        break;
    }
    buffer_appends(res, CRLF);
}

/* Send response headers to client in a single system call. */
static bool
send_response_header(struct http_transaction *ta)
{
    buffer_t response;
    start_response(ta, &response);
    buffer_appends(&ta->resp_headers, CRLF);

    buffer_t *response_and_headers[2] = {
        &response, &ta->resp_headers
    };

    int rc = bufio_sendbuffers(ta->client->bufio, response_and_headers, 2);
    buffer_delete(&response);
    return rc != -1;
}

/* Send a full response to client with the content in resp_body. */
static bool
send_response(struct http_transaction *ta)
{
    // add content-length.  All other headers must have already been set.
    add_content_length(&ta->resp_headers, ta->resp_body.len);
    buffer_appends(&ta->resp_headers, CRLF);

    buffer_t response;
    start_response(ta, &response);

    buffer_t *response_and_headers[3] = {
        &response, &ta->resp_headers, &ta->resp_body
    };

    int rc = bufio_sendbuffers(ta->client->bufio, response_and_headers, 3);
    buffer_delete(&response);
    return rc != -1;
}

const int MAX_ERROR_LEN = 2048;

/* Send an error response. */
static bool
send_error(struct http_transaction * ta, enum http_response_status status, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    char *error = buffer_ensure_capacity(&ta->resp_body, MAX_ERROR_LEN);
    int len = vsnprintf(error, MAX_ERROR_LEN, fmt, ap);
    ta->resp_body.len += len > MAX_ERROR_LEN ? MAX_ERROR_LEN - 1 : len;
    va_end(ap);
    ta->resp_status = status;
    http_add_header(&ta->resp_headers, "Content-Type", "text/plain");
    return send_response(ta);
}

/* Send Not Found response. */
static bool
send_not_found(struct http_transaction *ta)
{
    return send_error(ta, HTTP_NOT_FOUND, "File %s not found", 
        bufio_offset2ptr(ta->client->bufio, ta->req_path));
}

/* A start at assigning an appropriate mime type.  Real-world 
 * servers use more extensive lists such as /etc/mime.types
 */
static const char *
guess_mime_type(char *filename)
{
    char *suffix = strrchr(filename, '.');
    if (suffix == NULL)
        return "text/plain";

    if (!strcasecmp(suffix, ".html"))
        return "text/html";

    if (!strcasecmp(suffix, ".gif"))
        return "image/gif";

    if (!strcasecmp(suffix, ".png"))
        return "image/png";

    if (!strcasecmp(suffix, ".jpg"))
        return "image/jpeg";

    if (!strcasecmp(suffix, ".js"))
        return "text/javascript";

    if (!strcasecmp(suffix, ".css"))
    {
        return "text/css";
    }

    if (!strcasecmp(suffix, ".svg"))
    {
        return "image/svg+xml";
    }

    if (!strcasecmp(suffix, ".mp4"))
    {
        return "video/mp4";
    }

    if (!strcasecmp(suffix, ".ico"))
    {
        return "image/x-icon";
    }

    /* hint: you need to add support for (at least) .css, .svg, and .mp4
     * You can grep /etc/mime.types for the correct types */
    return "text/plain";
}

/* 
 * Handle HTTP transaction for static files. 
 * Ideas and code were derived from Claude.
 */
static bool
handle_static_asset(struct http_transaction *ta, char *basedir)
{
    char fname[PATH_MAX];

    char *req_path = bufio_offset2ptr(ta->client->bufio, ta->req_path);

    // Block path traversal attacks
    if (strstr(req_path, ".."))
    {
        return send_error(ta, HTTP_NOT_FOUND, "Not found.");
    }

    // Need an HTML5 fallback so that it can resolve what file to actually serve
    if (html5_fallback)
    {
        // "/" -> "/.index.html"
        if (strcmp(req_path, "/") == 0)
        {
            snprintf(fname, sizeof fname, "%s/index.html", basedir);
        }
        else
        {
            snprintf(fname, sizeof fname, "%s%s", basedir, req_path);
            struct stat st;
            if (stat(fname, &st) != 0 || !S_ISREG(st.st_mode))
            {
                // try <path>.html
                char try2[PATH_MAX];
                snprintf(try2, sizeof try2, "%s%s.html", basedir, req_path);
                if (stat(try2, &st) == 0 && S_ISREG(st.st_mode))
                {
                    snprintf(fname, sizeof fname, "%s", try2);
                }
                else
                {
                    // fallback to /200.html
                    snprintf(fname, sizeof fname, "%s/200.html", basedir);
                }
            }
        }
    }
    else
    {
        if (strcmp(req_path, "/") == 0)
        {
            snprintf(fname, sizeof fname, "%s/index.html", basedir);
        }
        else
        {
            snprintf(fname, sizeof fname, "%s%s", basedir, req_path);
        }
    }

    // Use stat (not access) to check the existence and distinguish files vs dirs
    struct stat st;
    if (stat(fname, &st) == -1)
    {
        return send_not_found(ta);
    }

    if (!S_ISREG(st.st_mode))
    {
        return send_not_found(ta);
    }

    int filefd = open(fname, O_RDONLY);
    if (filefd == -1)
    {
        return send_not_found(ta);
    }

    http_add_header(&ta->resp_headers, "Accept-Ranges", "bytes");

    off_t from = 0, to = st.st_size - 1;

    if (ta->req_has_range) {
        from = ta->req_range_start;
        off_t range_end = ta->req_range_end;
        to = (range_end == -1 || range_end >= st.st_size)
             ? st.st_size - 1 : range_end;

        if (from < 0 || from >= st.st_size || from > to) {
            close(filefd);
            ta->resp_status = HTTP_RANGE_NOT_SATISFIABLE;
            http_add_header(&ta->resp_headers, "Content-Range",
                            "bytes */%ld", (long)st.st_size);
            http_add_header(&ta->resp_headers, "Content-Type", "text/plain");
            buffer_appends(&ta->resp_body, "Range Not Satisfiable");
            return send_response(ta);
        }

        ta->resp_status = HTTP_PARTIAL_CONTENT;
        http_add_header(&ta->resp_headers, "Content-Range",
                        "bytes %ld-%ld/%ld", (long)from, (long)to, (long)st.st_size);
    } else {
        ta->resp_status = HTTP_OK;
    }

    http_add_header(&ta->resp_headers, "Content-Type", "%s", guess_mime_type(fname));
    off_t content_length = to + 1 - from;
    add_content_length(&ta->resp_headers, content_length);

    bool success = send_response_header(ta);
    if (!success)
    {
        goto out;
    }

    while (success && from <= to)
    {
        success = bufio_sendfile(ta->client->bufio, filefd, &from, to + 1 - from) > 0;
    }
out: 
    close(filefd);
    return success;
}

/*
 * Ideas and code were derived from Claude. 
 * Load JWT signing key from SECRET_KEYFILE env var.
 * Caller must jwks_free() the result. 
 */

static jwk_set_t *
load_keyfile(void)
{
    char *keyfilename = getenv("SECRET_KEYFILE");
    if (keyfilename == NULL)
    {
        fprintf(stderr, "SECRET_KEYFILE not set\n");
        return NULL;
    }

    return jwks_load_fromfile(NULL, keyfilename);
}

/*
 * Ideas and code were derived from Claude.
 * Returns allocated claims JSON string on success,
 * NULL on failure. Caller must free() the result.
 */
static char *
get_validated_claims(struct http_transaction *ta)
{
    if (ta->cookie_header == NULL)
    {
        return NULL;
    }

    char *token = NULL;
    char *copy = strdup(ta->cookie_header);
    if (!copy)
    {
        return NULL;
    }

    char *saveptr;
    char *part = strtok_r(copy, ";", &saveptr);
    while (part)
    {
        while (*part == ' ')
        {
            part++;
        }
        if (strncmp(part, "auth_jwt=", 9) == 0)
        {
            token = strdup(part + 9);
            break;
        }
        part = strtok_r(NULL, ";", &saveptr);
    }
    free(copy);
    if (!token)
    {
        return NULL;
    }

    jwk_set_t *keyset = load_keyfile();
    if (!keyset)
    {
        free(token);
        return NULL;
    }

    jwt_checker_t *checker = jwt_checker_new();
    if (!checker)
    {
        jwks_free(keyset);
        free(token);
        return NULL;
    }

    const jwk_item_t *key = jwks_item_get(keyset, 0);
    if (jwt_checker_setkey(checker, JWT_ALG_HS256, key) != 0)
    {
        jwt_checker_free(checker);
        jwks_free(keyset);
        free(token);
        return NULL;
    }

    struct claims_ctx ctx = {
        NULL
    };

    jwt_checker_setcb(checker, extract_claims_cb, &ctx);

    int rc = jwt_checker_verify(checker, token);

    jwt_checker_free(checker);
    jwks_free(keyset);
    free(token);

    if (rc != 0)
    {
        free(ctx.grants);
        return NULL;
    }
    return ctx.grants;
}

/*
 * Ideas and code were derived from Claude.
 * Handles the POST /api/login
 */
static bool
handle_login(struct http_transaction *ta)
{
    char *expected_user = getenv("USER_NAME");
    char *expected_pass = getenv("USER_PASS");
    if (!expected_user || !expected_pass)
    {
        return send_error(ta, HTTP_INTERNAL_ERROR, "Server misconfigured");
    }

    // need to parse the JSON body
    char *body = bufio_offset2ptr(ta->client->bufio, ta->req_body);
    json_error_t jerr;
    json_t *jbody = json_loadb(body, ta->req_content_len, 0, &jerr);
    if (!jbody)
    {
        return send_error(ta, HTTP_BAD_REQUEST, "Invalid JSON");
    }

    const char *username;
    const char *password;
    if (json_unpack(jbody, "{s:s, s:s}",
        "username", &username, "password", &password) != 0)
        {
            json_decref(jbody);
            return send_error(ta, HTTP_BAD_REQUEST, "Missing username or password");
        }
    
    if (strcmp(username, expected_user) != 0 ||
        strcmp(password, expected_pass) != 0) {
            json_decref(jbody);
            return send_error(ta, HTTP_PERMISSION_DENIED, "Invalid credentials");
        }
    json_decref(jbody);

    // Need to build the JWT
    jwk_set_t *keyset = load_keyfile();
    if (!keyset)
    {
        return send_error(ta, HTTP_INTERNAL_ERROR, "Could not load key");
    }

    jwt_builder_t *builder = jwt_builder_new();
    if (!builder)
    {
        jwks_free(keyset);
        return send_error(ta, HTTP_INTERNAL_ERROR, "JWT error");
    }

    // Need to set the sub claim
    jwt_value_t jval;
    jwt_set_SET_STR(&jval, "sub", expected_user);
    if (jwt_builder_claim_set(builder, &jval) != 0)
    {
        jwt_builder_free(builder);
        jwks_free(keyset);
        return send_error(ta, HTTP_INTERNAL_ERROR, "JWT claim error");
    }

    // Need to set exp (the iat is added automatically by libjwt)
    if (jwt_builder_time_offset(builder, JWT_CLAIM_EXP, token_expiration_time) != 0)
    {
        jwt_builder_free(builder);
        jwks_free(keyset);
        return send_error(ta, HTTP_INTERNAL_ERROR, "JWT exp error");
    }

    const jwk_item_t *key = jwks_item_get(keyset, 0);
    if (jwt_builder_setkey(builder, JWT_ALG_HS256, key) != 0)
    {
        jwt_builder_free(builder);
        jwks_free(keyset);
        return send_error(ta, HTTP_INTERNAL_ERROR, "JWT key error");
    }

    struct claims_ctx ctx = {
        NULL
    };
    jwt_builder_setcb(builder, extract_claims_cb, &ctx);

    char *encoded = jwt_builder_generate(builder);
    jwt_builder_free(builder);
    jwks_free(keyset);

    if (!encoded)
    {
        free(ctx.grants);
        return send_error(ta, HTTP_INTERNAL_ERROR, "JWT generate error");
    }

    // need to set cookie
    http_add_header(&ta->resp_headers, "Set-Cookie",
                    "auth_jwt=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d",
                    encoded, token_expiration_time);
    free(encoded);

    // Respond with claims as JSON body
    ta->resp_status = HTTP_OK;
    http_add_header(&ta->resp_headers, "Content-Type", "application/json");
    if (ctx.grants)
    {
        buffer_appends(&ta->resp_body, ctx.grants);
        free(ctx.grants);
    }
    else
    {
        buffer_appends(&ta->resp_body, "{}");
    }
    return send_response(ta);
}

/*
 * Ideas and code were derived from Claude.
 * Handle the GET /api/login - return claims
 * if authenticated, {} if not.
 */
static bool
handle_login_get(struct http_transaction *ta)
{
    char *claims= get_validated_claims(ta);
    ta->resp_status = HTTP_OK;
    http_add_header(&ta->resp_headers, "Content-Type", "application/json");
    if (claims)
    {
        buffer_appends(&ta->resp_body, claims);
        free(claims);
    }
    else
    {
        buffer_appends(&ta->resp_body, "{}");
    }
    return send_response(ta);
}

/*
 * Ideas and code were derived from Claude.
 * Handle the POST /api/logout - clear the cookie
 */
static bool
handle_logout(struct http_transaction *ta)
{
    http_add_header(&ta->resp_headers, "Set-Cookie",
                    "auth_jwt=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
    ta->resp_status = HTTP_OK;
    http_add_header(&ta->resp_headers, "Content-Type", "application/json");
    buffer_appends(&ta->resp_body, "{}");
    return send_response(ta);
}

/*
 * Ideas and code were derived from Claude.
 */
static bool
handle_video_api(struct http_transaction *ta)
{
    json_t *array = json_array();
    if (!array)
        return send_error(ta, HTTP_INTERNAL_ERROR, "Out of memory");

    DIR *dir = opendir(server_root);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            char *name = entry->d_name;
            size_t len = strlen(name);
            if (len < 4 || strcasecmp(name + len - 4, ".mp4") != 0)
                continue;

            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", server_root, name);

            struct stat st;
            if (stat(path, &st) == -1 || !S_ISREG(st.st_mode))
                continue;

            json_t *obj = json_pack("{s:s, s:I}",
                                    "name", name,
                                    "size", (json_int_t)st.st_size);
            if (obj)
                json_array_append_new(array, obj);
        }
        closedir(dir);
    }

    char *body = json_dumps(array, JSON_COMPACT);
    json_decref(array);
    if (!body)
        return send_error(ta, HTTP_INTERNAL_ERROR, "JSON encode error");

    ta->resp_status = HTTP_OK;
    http_add_header(&ta->resp_headers, "Content-Type", "application/json");
    buffer_appends(&ta->resp_body, body);
    free(body);
    return send_response(ta);
}

static bool
handle_api(struct http_transaction *ta)
{
    char *req_path = bufio_offset2ptr(ta->client->bufio, ta->req_path);

    if (!strcmp(req_path, "/api/login"))
    {
        if (ta->req_method == HTTP_POST)
            return handle_login(ta);
        if (ta->req_method == HTTP_GET)
            return handle_login_get(ta);
        return send_error(ta, HTTP_METHOD_NOT_ALLOWED, "Method not allowed.");
    }

    if (!strcmp(req_path, "/api/logout"))
    {
        if (ta->req_method != HTTP_POST)
            return send_error(ta, HTTP_METHOD_NOT_ALLOWED, "Method not allowed.");
        return handle_logout(ta);
    }

    if (!strcmp(req_path, "/api/video"))
    {
        if (ta->req_method != HTTP_GET)
            return send_error(ta, HTTP_METHOD_NOT_ALLOWED, "Method not allowed.");
        return handle_video_api(ta);
    }

    return send_error(ta, HTTP_NOT_FOUND, "API endpoint not found");
}

/* Set up an http client, associating it with a bufio buffer. */
void 
http_setup_client(struct http_client *self, struct bufio *bufio)
{
    self->bufio = bufio;
}

/* Handle a single HTTP transaction.  Returns true on success. */
bool
http_handle_transaction(struct http_client *self)
{
    struct http_transaction ta;
    memset(&ta, 0, sizeof ta);
    ta.client = self;

    if (!http_parse_request(&ta))
        return false;

    if (!http_process_headers(&ta))
        return false;

    /* FAST PATH for unauthenticated GET /api/login (the hot path under load).
     * Skips buffer allocation, header building, and sendmsg/iovec assembly. */
    {
        char *path = bufio_offset2ptr(ta.client->bufio, ta.req_path);
        if (ta.req_method == HTTP_GET
            && ta.req_version == HTTP_1_1
            && !ta.req_wants_close
            && ta.cookie_header == NULL
            && strcmp(path, "/api/login") == 0)
        {
            static const char fast_response[] =
                "HTTP/1.1 200 OK\r\n"
                "Server: CS3214-Personal-Server\r\n"
                "Connection: keep-alive\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 2\r\n"
                "\r\n"
                "{}";
            bufio_send_raw(self->bufio, fast_response, sizeof(fast_response) - 1);
            return true;
        }
    }

    if (ta.req_content_len > 0) {
        int rc = bufio_read(self->bufio, ta.req_content_len, &ta.req_body);
        if (rc != ta.req_content_len)
            return false;

        // To see the body, use this:
        // char *body = bufio_offset2ptr(ta.client->bufio, ta.req_body);
        // hexdump(body, ta.req_content_len);
    }

    buffer_init(&ta.resp_headers, 1024);
    http_add_header(&ta.resp_headers, "Server", "CS3214-Personal-Server");

    // Need to tell the client whether connection stays open
    if (ta.req_wants_close || ta.req_version == HTTP_1_0)
    {
        http_add_header(&ta.resp_headers, "Connection", "close");
    }
    else
    {
        http_add_header(&ta.resp_headers, "Connection", "keep-alive");
    }

    buffer_init(&ta.resp_body, 0);

    bool rc = false;
    char *req_path = bufio_offset2ptr(ta.client->bufio, ta.req_path);
    if (STARTS_WITH(req_path, "/api")) {
        rc = handle_api(&ta);
    } else
    if (STARTS_WITH(req_path, "/private")) {
        char *claims = get_validated_claims(&ta);
        if (claims == NULL)
        {
            rc = send_error(&ta, HTTP_PERMISSION_DENIED, "Not logged in.");
        }
        else
        {
            free(claims);
            rc = handle_static_asset(&ta, server_root);
        }
    } else {
        rc = handle_static_asset(&ta, server_root);
    }

    buffer_delete(&ta.resp_headers);
    buffer_delete(&ta.resp_body);

    // free the cookie header if we have allocated one
    if (ta.cookie_header)
    {
        free(ta.cookie_header);
        ta.cookie_header = NULL;
    }

    return rc && !ta.req_wants_close && ta.req_version != HTTP_1_0;
}
