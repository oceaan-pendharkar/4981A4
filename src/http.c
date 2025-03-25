#include "http.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

void my_function(const char *str)
{
    for(size_t i = 0; i < strlen(str); i++)
    {
        printf("%c", toupper(str[i]));
    }
}
