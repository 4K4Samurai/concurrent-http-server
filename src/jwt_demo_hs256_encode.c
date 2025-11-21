/*
 * Quick demo of how to use libjwt 3.x using a HS256 key.
 *
 * Create a JWT and output it encoded to standard output.
 * Run this with
 *
 * SECRET_KEYFILE=../tests/oct_key_256.json sh -c "./jwt_demo_hs256_encode  | ./jwt_demo_hs256_decode"
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
die_with_builder_error(const char *msg, jwt_builder_t *builder)
{
    fprintf(stderr, __FILE__"%s: %s\n", msg, jwt_builder_error_msg(builder));
    exit(EXIT_FAILURE);
}

struct userinfo {
    char *grants;
};

/* Extract the value of the sub claim */
static int 
jwt_extract_info_callback(jwt_t *jwt, jwt_config_t *config)
{
	jwt_value_error_t err;
	jwt_value_t jval;

    jwt_set_GET_JSON(&jval, NULL);
    err = jwt_claim_get(jwt, &jval);
    if (err) {
        return err;
    }

    struct userinfo *uinfo = config->ctx;
    uinfo->grants = jval.json_val;
    return 0;
}

int
main(int ac, char *av[])
{
    jwt_builder_t *token_builder  = jwt_builder_new();
    if (token_builder == NULL)
        die("jwt_builder_new", JWT_VALUE_ERR_NOMEM);

    jwt_value_t jval;
    jwt_set_SET_STR(&jval, "sub", "user0");

    jwt_value_error_t rc = jwt_builder_claim_set(token_builder, &jval);
    if (rc)
        die("jwt_builder_claim_set sub", rc);

    rc = jwt_builder_time_offset(token_builder, JWT_CLAIM_EXP, 3600*24);
    if (rc)
        die("jwt_builder_time_offset exp", JWT_VALUE_ERR_INVALID);

    char *keyfilename = getenv("SECRET_KEYFILE");
    if (keyfilename == NULL) {
        fprintf(stderr, "please set the `SECRET_KEYFILE' environment variable, e.g., run with\n"
                        "SECRET_KEYFILE=.... %s\n", av[0]);
        exit (EXIT_FAILURE);
    }

    jwk_set_t *keyset = jwks_load_fromfile(NULL, keyfilename);
    const jwk_item_t *key = jwks_item_get(keyset, 0);     // get first key
    int rc2 = jwt_builder_setkey(token_builder, JWT_ALG_HS256, key);
    if (rc2 != 0)
        die_with_builder_error("jwt_builder_setkey", token_builder);

    struct userinfo uinfo;
	rc2 = jwt_builder_setcb(token_builder, jwt_extract_info_callback, &uinfo);
    if (rc2 != 0)
        die_with_builder_error("jwt_builder_setcb", token_builder);

    char *encoded = jwt_builder_generate(token_builder);
    if (encoded == NULL)
        die_with_builder_error("jwt_builder_generate", token_builder);

    fprintf(stderr, __FILE__":full grants are %s\n", uinfo.grants);
    fprintf(stderr, __FILE__":now outputting encoded JWT to stdout\n(you may try entering this at jwt.io)\n");
    printf("%s", encoded);

    // valgrind checking
    free(encoded);
    free(uinfo.grants);
    jwks_free(keyset);
    jwt_builder_free(token_builder);
}
