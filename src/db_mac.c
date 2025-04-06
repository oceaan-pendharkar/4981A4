#include <errno.h>
#include <fcntl.h>
#include <gdbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUFFER_SIZE 25
#define BASE_TEN 10
#define PERMS 0666

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
    GDBM_FILE request_db;
} DBContext;

typedef struct
{
    DBCommand   cmd;
    const char *key_arg;
} ParsedArgs;

static int            fetch_value(DBContext *ctx, const char *key_str);
static void           parse_arguments(int argc, char *argv[], ParsedArgs *parsed_args);
static int            fetch_all(DBContext *ctx);
static int            get_last_key(GDBM_FILE db, char *key_out, size_t buf_size);
_Noreturn static void usage(const char *program_name, int exit_code, const char *message);
static void           print_key_value(datum key, const char *value);

int main(int argc, char *argv[])
{
    DBContext  db_ctx;
    ParsedArgs args;
    int        result = EXIT_SUCCESS;

    parse_arguments(argc, argv, &args);

    db_ctx.request_db = gdbm_open("requests_db.db", 0, GDBM_READER, PERMS, NULL);
    if(!db_ctx.request_db)
    {
        perror("gdbm_open");
        return EXIT_FAILURE;
    }

    if(args.cmd == CMD_ALL)
    {
        result = fetch_all(&db_ctx);
    }
    else if(args.cmd == CMD_KEY)
    {
        result = fetch_value(&db_ctx, args.key_arg);
    }
    else if(args.cmd == CMD_LATEST)
    {
        char last_key[BUFFER_SIZE];
        result = get_last_key(db_ctx.request_db, last_key, BUFFER_SIZE);
        if(result == EXIT_SUCCESS)
        {
            result = fetch_value(&db_ctx, last_key);
        }
    }

    gdbm_close(db_ctx.request_db);
    return result;
}

static int fetch_value(DBContext *ctx, const char *key_str)
{
    datum key;
    datum value;

    char mutable_key[BUFFER_SIZE];
    strncpy(mutable_key, key_str, BUFFER_SIZE);
    mutable_key[BUFFER_SIZE - 1] = '\0';

    key.dptr  = mutable_key;
    key.dsize = (int)strlen(mutable_key);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
    value = gdbm_fetch(ctx->request_db, key);
#pragma GCC diagnostic pop

    if(value.dptr)
    {
        char *safe_val = (char *)malloc((unsigned long)value.dsize + 1);
        if(!safe_val)
        {
            fprintf(stderr, "Memory allocation failed.\n");
            free(value.dptr);
            return EXIT_FAILURE;
        }
        memcpy(safe_val, value.dptr, (unsigned long)value.dsize);
        safe_val[value.dsize] = '\0';
        print_key_value(key, safe_val);
        free(safe_val);
        free(value.dptr);
        return EXIT_SUCCESS;
    }

    printf("Key \"%s\" not found in database.\n", key_str);
    return EXIT_FAILURE;
}

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
        else if(opt == 'k')
        {
            parsed_args->cmd     = CMD_KEY;
            parsed_args->key_arg = optarg;
        }
        else if(opt == 'l')
        {
            parsed_args->cmd = CMD_LATEST;
        }
        else
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

static int get_last_key(GDBM_FILE db, char *key_out, size_t buf_size)
{
    datum counter_key;
    datum counter_val;
    int   counter;
    char *endptr;
    char  counter_key_buf[] = "__counter__";
    char *counter_str;

    counter_key.dptr  = counter_key_buf;
    counter_key.dsize = (int)strlen(counter_key_buf);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
    counter_val = gdbm_fetch(db, counter_key);
#pragma GCC diagnostic pop

    if(!counter_val.dptr)
    {
        fprintf(stderr, "No counter found in DB.\n");
        return -1;
    }

    counter_str = (char *)malloc((unsigned long)counter_val.dsize + 1);
    if(!counter_str)
    {
        fprintf(stderr, "Memory allocation failed.\n");
        free(counter_val.dptr);
        return -1;
    }
    memcpy(counter_str, counter_val.dptr, (unsigned long)counter_val.dsize);
    counter_str[counter_val.dsize] = '\0';
    free(counter_val.dptr);

    counter = (int)strtol(counter_str, &endptr, BASE_TEN);
    free(counter_str);

    if(endptr == counter_str || *endptr != '\0' || counter <= 0)
    {
        fprintf(stderr, "Invalid or empty counter value.\n");
        return -1;
    }

    snprintf(key_out, buf_size, "%d", counter - 1);
    return 0;
}

static int fetch_all(DBContext *ctx)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
    datum key = gdbm_firstkey(ctx->request_db);
#pragma GCC diagnostic pop

    if(!key.dptr)
    {
        printf("Database is empty.\n");
        return EXIT_SUCCESS;
    }

    while(key.dptr)
    {
        datum next;
        if(strncmp(key.dptr, "__counter__", (unsigned long)key.dsize) != 0)
        {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
            datum value = gdbm_fetch(ctx->request_db, key);
#pragma GCC diagnostic pop
            if(value.dptr)
            {
                char *safe_val = (char *)malloc((unsigned long)value.dsize + 1);
                if(!safe_val)
                {
                    fprintf(stderr, "Memory allocation failed.\n");
                    free(value.dptr);
                    free(key.dptr);
                    return EXIT_FAILURE;
                }
                memcpy(safe_val, value.dptr, (unsigned long)value.dsize);
                safe_val[value.dsize] = '\0';
                print_key_value(key, safe_val);
                free(safe_val);
                free(value.dptr);
            }
        }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waggregate-return"
        next = gdbm_nextkey(ctx->request_db, key);
#pragma GCC diagnostic pop
        free(key.dptr);
        key = next;
    }

    return EXIT_SUCCESS;
}

static void print_key_value(datum key, const char *value)
{
    printf("Key:\t%.*s\nValue:\t%s\n\n", key.dsize, key.dptr, value);
}
