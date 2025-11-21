/*
 * Decode a JWT read from standard input
 *
 * @author gback, CS 3214, Fall'25
 */
#include <jwt.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <jansson.h>

/*
 * Note: you cannot call `die` in your server - it would exit
 * the server process altogether.  You must find a way to handle
 * errors more gracefully.
 */
static void 
die(const char *msg, int error)
{
    const char *errmsg[] = {
	    /* JWT_VALUE_ERR_NONE = 0 */ "no error, success",
	    /* JWT_VALUE_ERR_EXIST */	 "item exists (when setting)",
	    /* JWT_VALUE_ERR_NOEXIST */	 "item doesn't exist (when getting)",
	    /* JWT_VALUE_ERR_TYPE */	 "item is not of the type requested",
	    /* JWT_VALUE_ERR_INVALID */	 "invalid request (general error)",
	    /* JWT_VALUE_ERR_NOMEM */    "memory allocation error",
    };
    assert (0 <= error && error <= JWT_VALUE_ERR_NOMEM);
    fprintf(stderr, __FILE__"%s: %s\n", msg, errmsg[error]);
    exit(EXIT_FAILURE);
}

static void 
die_with_checker_error(const char *msg, jwt_checker_t *checker)
{
    fprintf(stderr, __FILE__"%s: %s\n", msg, jwt_checker_error_msg(checker));
    exit(EXIT_FAILURE);
}

/* libjwt 3.x uses callbacks during verification. 
 * Access to the JWT's content is provided only in these callbacks. 
 * The callback below extracts, as an example, the value of all
 * claims, and separately also the value of the `sub` for
 * demonstration purposes.
 */
struct userinfo {
    char *sub;
    char *grants;
};

/* Extract the value of the sub claim */
static int 
jwt_extract_info_callback(jwt_t *jwt, jwt_config_t *config)
{
	jwt_value_error_t err;
	jwt_value_t jval;

	jwt_set_GET_STR(&jval, "sub");
	err = jwt_claim_get(jwt, &jval);
    if (err)
        return err;

    struct userinfo *uinfo = config->ctx;
    uinfo->sub = strdup(jval.str_val);

    jwt_set_GET_JSON(&jval, NULL);
    err = jwt_claim_get(jwt, &jval);
    if (err) {
        free((void *)uinfo->sub);
        return err;
    }

    uinfo->grants = jval.json_val;
    return 0;       // successful validation
}

int
main(int ac, char *av[])
{
    jwt_checker_t *token_checker  = jwt_checker_new();
    if (token_checker == NULL)
        die("jwt_checker_new", JWT_VALUE_ERR_NOMEM);

    char *keyfilename = getenv("SECRET_KEYFILE");
    if (keyfilename == NULL) {
        fprintf(stderr, "please set the `SECRET_KEYFILE' environment variable, e.g., run with\n"
                        "SECRET_KEYFILE=.... %s\n", av[0]);
        exit (EXIT_FAILURE);
    }
    jwk_set_t *keyset = jwks_load_fromfile(NULL, keyfilename);
    const jwk_item_t *key = jwks_item_get(keyset, 0);     // get first key
    int rc2 = jwt_checker_setkey(token_checker, JWT_ALG_HS256, key);
    if (rc2 != 0)
        die_with_checker_error("jwt_checker_setkey", token_checker);

    const int TOKEN_MAX_SIZE = 8192;
    char *token = calloc(TOKEN_MAX_SIZE+1, 1);
    size_t token_size = read(0, token, TOKEN_MAX_SIZE);
    if (token_size < 0)
        die("did not find a token in standard input", errno);

    struct userinfo uinfo;
	rc2 = jwt_checker_setcb(token_checker, jwt_extract_info_callback, &uinfo);
    if (rc2 != 0)
        die_with_checker_error("jwt_checker_setcb", token_checker);

    printf (__FILE__":decoding %s\n", token);
    rc2 = jwt_checker_verify(token_checker, token);
    if (rc2 != 0)
        die_with_checker_error("jwt_checker_verify", token_checker);

    jwks_free(keyset);
    jwt_checker_free(token_checker);
    printf (__FILE__":`sub` claim %s\n", uinfo.sub);
    printf (__FILE__":all claims %s\n", uinfo.grants);

    // an example of how to use Jansson
    json_error_t error;
    char * grants = uinfo.grants;
    json_t *jgrants = json_loadb(grants, strlen(grants), 0, &error);
    if (jgrants == NULL)
        die("json_loadb", EINVAL);

    free (grants);

    json_int_t exp, iat;
    const char *sub;
    int rc = json_unpack(jgrants, "{s:I, s:I, s:s}", 
            "exp", &exp, "iat", &iat, "sub", &sub);
    if (rc == -1)
        die("json_unpack", EINVAL);

    printf (__FILE__":exp: %lld\n", exp);
    printf (__FILE__":iat: %lld\n", iat);
    printf (__FILE__":sub: %s\n", sub);

    json_decref(jgrants);

    // for valgrind leak checking
    free(token);
    free(uinfo.sub);
}
