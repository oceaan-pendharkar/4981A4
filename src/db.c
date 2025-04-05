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
#define BUFFER_SIZE 16
#define BASE_TEN 10

typedef enum
{
    CMD_NONE,
    CMD_KEY,
    CMD_ALL,
    CMD_LATEST,
    CMD_HELP
} DBCommand;

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

typedef struct
{
    DBCommand   cmd;
    const char *key_arg;
} ParsedArgs;

static int            fetch_value(DBContext *ctx, const char *key_str);
static void           parse_arguments(int argc, char *argv[], ParsedArgs *parsed_args);
static int            fetch_all(DBContext *ctx);
static int            get_last_key(DBM *db, char *key_out, size_t buf_size);
_Noreturn static void usage(const char *program_name, int exit_code, const char *message);
static void           print_key_value(const char *key, const char *value);

int main(int argc, char *argv[])
{
    DBContext  db_ctx;
    ParsedArgs args;
    int        result = EXIT_SUCCESS;
    char       db_name[BUFFER_SIZE];

    strcpy(db_name, "requests_db");
    parse_arguments(argc, argv, &args);

    if(args.cmd == CMD_HELP)
    {
        usage(argv[0], EXIT_SUCCESS, NULL);
    }

    db_ctx.request_db = dbm_open(db_name, O_RDONLY, 0);
    if(!db_ctx.request_db)
    {
        perror("dbm_open");
        return EXIT_FAILURE;
    }

    if(args.cmd == CMD_ALL)
    {
        result = fetch_all(&db_ctx);
    }

    if(args.cmd == CMD_KEY)
    {
        result = fetch_value(&db_ctx, args.key_arg);
    }

    if(args.cmd == CMD_LATEST)
    {
        char last_key[BUFFER_SIZE];

        result = get_last_key(db_ctx.request_db, last_key, BUFFER_SIZE);
        if(result == EXIT_SUCCESS)
        {
            result = fetch_value(&db_ctx, last_key);
        }
    }

    dbm_close(db_ctx.request_db);
    return result;
}

/*
    Fetches and prints the value for a given key from the database

    @param
    ctx: Pointer to the DB context containing the open database
    key_str: Key to look up

    @return
    EXIT_SUCCESS: Key found and printed
    EXIT_FAILURE: Key not found
 */
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
        print_key_value(key_str, result.dptr);
        return EXIT_SUCCESS;
    }

    printf("Key \"%s\" not found in database.\n", key_str);
    return EXIT_FAILURE;
}

/*
    Parses command-line arguments and stores results in ParsedArgs

    @param
    argc: Argument count
    argv: Argument vector
    parsed_args: Output struct to store the parsed command and key (if any)
 */
static void parse_arguments(int argc, char *argv[], ParsedArgs *parsed_args)
{
    int opt;
    parsed_args->cmd     = CMD_NONE;
    parsed_args->key_arg = NULL;

    opterr = 0;

    while((opt = getopt(argc, argv, "hak:l")) != -1)
    {
        if(opt == 'h')
        {
            parsed_args->cmd = CMD_HELP;
            return;
        }

        if(opt == 'a')
        {
            parsed_args->cmd = CMD_ALL;
        }

        if(opt == 'k')
        {
            parsed_args->cmd     = CMD_KEY;
            parsed_args->key_arg = optarg;
        }

        if(opt == 'l')
        {
            parsed_args->cmd = CMD_LATEST;
        }

        if(opt != 'a' && opt != 'k' && opt != 'l')
        {
            usage(argv[0], EXIT_FAILURE, "Invalid option.");
        }
    }

    if(parsed_args->cmd == CMD_KEY && parsed_args->key_arg == NULL)
    {
        usage(argv[0], EXIT_FAILURE, "Option -k requires a key argument.");
    }

    if(parsed_args->cmd == CMD_NONE)
    {
        usage(argv[0], EXIT_FAILURE, "No options provided.");
    }

    if(optind < argc)
    {
        usage(argv[0], EXIT_FAILURE, "Too many arguments provided.");
    }
}

/*
    Prints usage information and exits the program

    @param
    program_name: Name of the executable
    exit_code: Exit status code
    message: Optional error or help message to display
 */
_Noreturn static void usage(const char *program_name, int exit_code, const char *message)
{
    if(message)
    {
        fprintf(stderr, "%s\n", message);
    }

    fprintf(stderr, "Usage: %s [-h] [-l] [-k <key>] [-a]\n", program_name);
    fputs("Options:\n", stderr);
    fputs("  -h         Display this help message\n", stderr);
    fputs("  -l         Print the most recent entry\n", stderr);
    fputs("  -k <key>   Print the value for the specified key\n", stderr);
    fputs("  -a         Print all key-value pairs in the database\n", stderr);
    exit(exit_code);
}

/*
    Retrieves the last inserted key from the database using the __counter__ value.

    @param
    db: Open DBM database pointer
    key_out: Output buffer to store the last key
    buf_size: Size of the output buffer

    @return
    0 on success, -1 on failure
 */
static int get_last_key(DBM *db, char *key_out, size_t buf_size)
{
    datum counter_key;
    datum counter_val;
    char  counter_key_buf[BUFFER_SIZE];
    int   counter;
    char *endptr = NULL;

    strcpy(counter_key_buf, "__counter__");
    counter_key.dptr  = counter_key_buf;
    counter_key.dsize = strlen("__counter__") + 1;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
    counter_val = dbm_fetch(db, counter_key);
#pragma GCC diagnostic pop

    if(!counter_val.dptr)
    {
        fprintf(stderr, "No counter found in DB.\n");
        return -1;
    }

    counter = (int)strtol(counter_val.dptr, &endptr, BASE_TEN);

    if(endptr == counter_val.dptr || *endptr != '\0' || counter <= 0)
    {
        fprintf(stderr, "Invalid or empty counter value.\n");
        return -1;
    }

    snprintf(key_out, buf_size, "%d", counter - 1);
    return 0;
}

/*
    Iterates over all key-value pairs in the DB and prints them.

    @param
    ctx: The open DB context

    @return
    EXIT_SUCCESS if completed, EXIT_FAILURE if DB error occurs
 */
static int fetch_all(DBContext *ctx)
{
    datum key;
    datum value;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
    key = dbm_firstkey(ctx->request_db);
#pragma GCC diagnostic pop

    if(key.dptr == NULL)
    {
        printf("Database is empty.\n");
        return EXIT_SUCCESS;
    }

    while(key.dptr != NULL)
    {
        // Skip internal counter key
        if(strcmp(key.dptr, "__counter__") != 0)
        {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
            value = dbm_fetch(ctx->request_db, key);
#pragma GCC diagnostic pop

            if(value.dptr)
            {
                print_key_value(key.dptr, value.dptr);
            }
        }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
        key = dbm_nextkey(ctx->request_db);
#pragma GCC diagnostic pop
    }

    return EXIT_SUCCESS;
}

/*
    Prints a key-value pair in a formatted style

    @param
    key: The key string
    value: The associated value string
 */
static void print_key_value(const char *key, const char *value)
{
    printf("Key:\t%s\nValue:\t%s\n\n", key, value);
}
