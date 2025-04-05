#include <errno.h>
#include <fcntl.h>
#include <ndbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
typedef size_t datum_size;
#else
typedef int datum_size;
#endif

#define MAKE_CONST_DATUM(str) ((const_datum){(str), (datum_size)strlen(str) + 1})
#define DEFAULT_KEY "post_data"
#define BUFFER_SIZE 16

typedef struct
{
    // cppcheck-suppress unusedStructMember
    const void *dptr;
    // cppcheck-suppress unusedStructMember
    datum_size dsize;
} const_datum;

typedef struct
{
    DBM *request_db;
} DBContext;

static int fetch_value(DBContext *ctx, const char *key_str);

int main(int argc, const char *argv[])
{
    DBContext   db_ctx;
    const char *key_str;
    int         result;
    char        db_name[BUFFER_SIZE];

    strcpy(db_name, "requests_db");

    // Determine the key
    if(argc == 2)
    {
        key_str = argv[1];
    }
    else
    {
        key_str = DEFAULT_KEY;
        printf("No key provided. Defaulting to key: \"%s\"\n", DEFAULT_KEY);
    }

    // Open the DB
    db_ctx.request_db = dbm_open(db_name, O_RDONLY, 0);
    if(!db_ctx.request_db)
    {
        perror("dbm_open");
        return EXIT_FAILURE;
    }

    // Fetch and print
    result = fetch_value(&db_ctx, key_str);

    // Clean up
    dbm_close(db_ctx.request_db);
    return result;
}

static int fetch_value(DBContext *ctx, const char *key_str)
{
    const_datum key_datum;
    datum       result;

    key_datum = MAKE_CONST_DATUM(key_str);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
    result = dbm_fetch(ctx->request_db, *(datum *)&key_datum);
#pragma GCC diagnostic pop

    if(result.dptr)
    {
        printf("Value for key \"%s\":\n%s\n", key_str, result.dptr);
        return EXIT_SUCCESS;
    }

    printf("Key \"%s\" not found in database.\n", key_str);
    return EXIT_FAILURE;
}
