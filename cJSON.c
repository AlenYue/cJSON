/*
  Copyright (c) 2009 Dave Gamble

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/* cJSON */
/* JSON parser in C. */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <float.h>
#include <limits.h>
#include <ctype.h>
#include "cJSON.h"

/* define our own boolean type */
typedef int cjbool;
#define true ((cjbool)1)
#define false ((cjbool)0)

static const unsigned char *global_ep = NULL;

const char *cJSON_GetErrorPtr(void)
{
    return (const char*) global_ep;
}

/* This is a safeguard to prevent copy-pasters from using incompatible C and header files */
#if (CJSON_VERSION_MAJOR != 1) || (CJSON_VERSION_MINOR != 3) || (CJSON_VERSION_PATCH != 0)
    #error cJSON.h and cJSON.c have different versions. Make sure that both have the same.
#endif

extern const char* cJSON_Version(void)
{
    static char version[15];
    sprintf(version, "%i.%i.%i", CJSON_VERSION_MAJOR, CJSON_VERSION_MINOR, CJSON_VERSION_PATCH);

    return version;
}

/* case insensitive strcmp */
static int cJSON_strcasecmp(const unsigned char *s1, const unsigned char *s2)
{
    if (!s1)
    {
        return (s1 == s2) ? 0 : 1; /* both NULL? */
    }
    if (!s2)
    {
        return 1;
    }
    for(; tolower(*s1) == tolower(*s2); ++s1, ++s2)
    {
        if (*s1 == '\0')
        {
            return 0;
        }
    }

    return tolower(*s1) - tolower(*s2);
}

static void *(*cJSON_malloc)(size_t sz) = malloc;
static void (*cJSON_free)(void *ptr) = free;
static void *(*cJSON_realloc)(void *pointer, size_t size) = realloc;

static unsigned char* cJSON_strdup(const unsigned char* str)
{
    size_t len = 0;
    unsigned char *copy = NULL;

    if (str == NULL)
    {
        return NULL;
    }

    len = strlen((const char*)str) + 1;
    if (!(copy = (unsigned char*)cJSON_malloc(len)))
    {
        return NULL;
    }
    memcpy(copy, str, len);

    return copy;
}

void cJSON_InitHooks(cJSON_Hooks* hooks)
{
    if (hooks == NULL)
    {
        /* Reset hooks */
        cJSON_malloc = malloc;
        cJSON_free = free;
        cJSON_realloc = realloc;
        return;
    }

    cJSON_malloc = malloc;
    if (hooks->malloc_fn != NULL)
    {
        cJSON_malloc = hooks->malloc_fn;
    }

    cJSON_free = free;
    if (hooks->free_fn != NULL)
    {
        cJSON_free = hooks->free_fn;
    }

    /* use realloc only if both free and malloc are used */
    cJSON_realloc = NULL;
    if ((cJSON_malloc == malloc) && (cJSON_free == free))
    {
        cJSON_realloc = realloc;
    }
}

/* Internal constructor. */
static cJSON *cJSON_New_Item(void)
{
    cJSON* node = (cJSON*)cJSON_malloc(sizeof(cJSON));
    if (node)
    {
        memset(node, '\0', sizeof(cJSON));
    }

    return node;
}

/* Delete a cJSON structure. */
void cJSON_Delete(cJSON *c)
{
    cJSON *next = NULL;
    while (c)
    {
        next = c->next;
        if (!(c->type & cJSON_IsReference) && c->child)
        {
            cJSON_Delete(c->child);
        }
        if (!(c->type & cJSON_IsReference) && c->valuestring)
        {
            cJSON_free(c->valuestring);
        }
        if (!(c->type & cJSON_StringIsConst) && c->string)
        {
            cJSON_free(c->string);
        }
        cJSON_free(c);
        c = next;
    }
}

/* Parse the input text to generate a number, and populate the result into item. */
static const unsigned char *parse_number(cJSON * const item, const unsigned char * const input)
{
    double number = 0;
    unsigned char *after_end = NULL;

    if (input == NULL)
    {
        return NULL;
    }

    number = strtod((const char*)input, (char**)&after_end);
    if (input == after_end)
    {
        return NULL; /* parse_error */
    }

    item->valuedouble = number;

    /* use saturation in case of overflow */
    if (number >= INT_MAX)
    {
        item->valueint = INT_MAX;
    }
    else if (number <= INT_MIN)
    {
        item->valueint = INT_MIN;
    }
    else
    {
        item->valueint = (int)number;
    }

    item->type = cJSON_Number;

    return after_end;
}

/* don't ask me, but the original cJSON_SetNumberValue returns an integer or double */
double cJSON_SetNumberHelper(cJSON *object, double number)
{
    if (number >= INT_MAX)
    {
        object->valueint = INT_MAX;
    }
    else if (number <= INT_MIN)
    {
        object->valueint = INT_MIN;
    }
    else
    {
        object->valueint = cJSON_Number;
    }

    return object->valuedouble = number;
}

typedef struct
{
    unsigned char *buffer;
    size_t length;
    size_t offset;
    cjbool noalloc;
} printbuffer;

/* realloc printbuffer if necessary to have at least "needed" bytes more */
static unsigned char* ensure(printbuffer * const p, size_t needed)
{
    unsigned char *newbuffer = NULL;
    size_t newsize = 0;

    if ((p == NULL) || (p->buffer == NULL))
    {
        return NULL;
    }

    if (needed > INT_MAX)
    {
        /* sizes bigger than INT_MAX are currently not supported */
        return NULL;
    }

    needed += p->offset;
    if (needed <= p->length)
    {
        return p->buffer + p->offset;
    }

    if (p->noalloc) {
        return NULL;
    }

    /* calculate new buffer size */
    newsize = needed * 2;
    if (newsize > INT_MAX)
    {
        /* overflow of int, use INT_MAX if possible */
        if (needed <= INT_MAX)
        {
            newsize = INT_MAX;
        }
        else
        {
            return NULL;
        }
    }

    if (cJSON_realloc != NULL)
    {
        /* reallocate with realloc if available */
        newbuffer = (unsigned char*)cJSON_realloc(p->buffer, newsize);
    }
    else
    {
        /* otherwise reallocate manually */
        newbuffer = (unsigned char*)cJSON_malloc(newsize);
        if (!newbuffer)
        {
            cJSON_free(p->buffer);
            p->length = 0;
            p->buffer = NULL;

            return NULL;
        }
        if (newbuffer)
        {
            memcpy(newbuffer, p->buffer, p->offset + 1);
        }
        cJSON_free(p->buffer);
    }
    p->length = newsize;
    p->buffer = newbuffer;

    return newbuffer + p->offset;
}

/* calculate the new length of the string in a printbuffer */
static size_t update(const printbuffer *p)
{
    const unsigned char *str = NULL;
    if (!p || !p->buffer)
    {
        return 0;
    }
    str = p->buffer + p->offset;

    return p->offset + strlen((const char*)str);
}

/* Render the number nicely from the given item into a string. */
static unsigned char *print_number(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    double d = item->valuedouble;

    if (output_buffer == NULL)
    {
        return NULL;
    }

    /* value is an int */
    if ((fabs(((double)item->valueint) - d) <= DBL_EPSILON) && (d <= INT_MAX) && (d >= INT_MIN))
    {
        /* 2^64+1 can be represented in 21 chars. */
        output_pointer = ensure(output_buffer, 21);
        if (output_pointer != NULL)
        {
            sprintf((char*)output_pointer, "%d", item->valueint);
        }
    }
    /* value is a floating point number */
    else
    {
        /* This is a nice tradeoff. */
        output_pointer = ensure(output_buffer, 64);
        if (output_pointer != NULL)
        {
            /* This checks for NaN and Infinity */
            if ((d * 0) != 0)
            {
                sprintf((char*)output_pointer, "null");
            }
            else if ((fabs(floor(d) - d) <= DBL_EPSILON) && (fabs(d) < 1.0e60))
            {
                sprintf((char*)output_pointer, "%.0f", d);
            }
            else if ((fabs(d) < 1.0e-6) || (fabs(d) > 1.0e9))
            {
                sprintf((char*)output_pointer, "%e", d);
            }
            else
            {
                sprintf((char*)output_pointer, "%f", d);
            }
        }
    }

    return output_pointer;
}

/* parse 4 digit hexadecimal number */
static unsigned parse_hex4(const unsigned char * const input)
{
    unsigned int h = 0;
    size_t i = 0;

    for (i = 0; i < 4; i++)
    {
        /* parse digit */
        if ((input[i] >= '0') && (input[i] <= '9'))
        {
            h += (unsigned int) input[i] - '0';
        }
        else if ((input[i] >= 'A') && (input[i] <= 'F'))
        {
            h += (unsigned int) 10 + input[i] - 'A';
        }
        else if ((input[i] >= 'a') && (input[i] <= 'f'))
        {
            h += (unsigned int) 10 + input[i] - 'a';
        }
        else /* invalid */
        {
            return 0;
        }

        if (i < 3)
        {
            /* shift left to make place for the next nibble */
            h = h << 4;
        }
    }

    return h;
}

/* converts a UTF-16 literal to UTF-8
 * A literal can be one or two sequences of the form \uXXXX */
static unsigned char utf16_literal_to_utf8(const unsigned char * const input_pointer, const unsigned char * const input_end, unsigned char **output_pointer, const unsigned char **error_pointer)
{
    /* first bytes of UTF8 encoding for a given length in bytes */
    static const unsigned char firstByteMark[5] =
    {
        0x00, /* should never happen */
        0x00, /* 0xxxxxxx */
        0xC0, /* 110xxxxx */
        0xE0, /* 1110xxxx */
        0xF0 /* 11110xxx */
    };

    long unsigned int codepoint = 0;
    unsigned int first_code = 0;
    const unsigned char *first_sequence = input_pointer;
    unsigned char utf8_length = 0;
    unsigned char sequence_length = 0;

    /* get the first utf16 sequence */
    first_code = parse_hex4(first_sequence + 2);
    if ((input_end - first_sequence) < 6)
    {
        /* input ends unexpectedly */
        *error_pointer = first_sequence;
        goto fail;
    }

    /* check that the code is valid */
    if (((first_code >= 0xDC00) && (first_code <= 0xDFFF)) || (first_code == 0))
    {
        *error_pointer = first_sequence;
        goto fail;
    }

    /* UTF16 surrogate pair */
    if ((first_code >= 0xD800) && (first_code <= 0xDBFF))
    {
        const unsigned char *second_sequence = first_sequence + 6;
        unsigned int second_code = 0;
        sequence_length = 12; /* \uXXXX\uXXXX */

        if ((input_end - second_sequence) < 6)
        {
            /* input ends unexpectedly */
            *error_pointer = first_sequence;
            goto fail;
        }

        if ((second_sequence[0] != '\\') || (second_sequence[1] != 'u'))
        {
            /* missing second half of the surrogate pair */
            *error_pointer = first_sequence;
            goto fail;
        }

        /* get the second utf16 sequence */
        second_code = parse_hex4(second_sequence + 2);
        /* check that the code is valid */
        if ((second_code < 0xDC00) || (second_code > 0xDFFF))
        {
            /* invalid second half of the surrogate pair */
            *error_pointer = first_sequence;
            goto fail;
        }


        /* calculate the unicode codepoint from the surrogate pair */
        codepoint = 0x10000 + (((first_code & 0x3FF) << 10) | (second_code & 0x3FF));
    }
    else
    {
        sequence_length = 6; /* \uXXXX */
        codepoint = first_code;
    }

    /* encode as UTF-8
     * takes at maximum 4 bytes to encode:
     * 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
    if (codepoint < 0x80)
    {
        /* normal ascii, encoding 0xxxxxxx */
        utf8_length = 1;
    }
    else if (codepoint < 0x800)
    {
        /* two bytes, encoding 110xxxxx 10xxxxxx */
        utf8_length = 2;
    }
    else if (codepoint < 0x10000)
    {
        /* three bytes, encoding 1110xxxx 10xxxxxx 10xxxxxx */
        utf8_length = 3;
    }
    else if (codepoint <= 0x10FFFF)
    {
        /* four bytes, encoding 1110xxxx 10xxxxxx 10xxxxxx 10xxxxxx */
        utf8_length = 4;
    }
    else
    {
        /* invalid unicode codepoint */
        *error_pointer = first_sequence;
        goto fail;
    }

    /* encode as utf8 */
    switch (utf8_length)
    {
        case 4:
            /* 10xxxxxx */
            (*output_pointer)[3] = (unsigned char)((codepoint | 0x80) & 0xBF);
            codepoint >>= 6;
        case 3:
            /* 10xxxxxx */
            (*output_pointer)[2] = (unsigned char)((codepoint | 0x80) & 0xBF);
            codepoint >>= 6;
        case 2:
            (*output_pointer)[1] = (unsigned char)((codepoint | 0x80) & 0xBF);
            codepoint >>= 6;
        case 1:
            /* depending on the length in bytes this determines the
               encoding of the first UTF8 byte */
            (*output_pointer)[0] = (unsigned char)((codepoint | firstByteMark[utf8_length]) & 0xFF);
            break;
        default:
            *error_pointer = first_sequence;
            goto fail;
    }
    *output_pointer += utf8_length;

    return sequence_length;

fail:
    return 0;
}

/* Parse the input text into an unescaped cinput, and populate item. */
static const unsigned char *parse_string(cJSON * const item, const unsigned char * const input, const unsigned char ** const error_pointer)
{
    const unsigned char *input_pointer = input + 1;
    const unsigned char *input_end = input + 1;
    unsigned char *output_pointer = NULL;
    unsigned char *output = NULL;

    /* not a string */
    if (*input != '\"')
    {
        *error_pointer = input;
        goto fail;
    }

    {
        /* calculate approximate size of the output (overestimate) */
        size_t allocation_length = 0;
        size_t skipped_bytes = 0;
        while ((*input_end != '\"') && (*input_end != '\0'))
        {
            /* is escape sequence */
            if (input_end[0] == '\\')
            {
                if (input_end[1] == '\0')
                {
                    /* prevent buffer overflow when last input character is a backslash */
                    goto fail;
                }
                skipped_bytes++;
                input_end++;
            }
            input_end++;
        }
        if (*input_end == '\0')
        {
            goto fail; /* string ended unexpectedly */
        }

        /* This is at most how much we need for the output */
        allocation_length = (size_t) (input_end - input) - skipped_bytes;
        output = (unsigned char*)cJSON_malloc(allocation_length + sizeof('\0'));
        if (output == NULL)
        {
            goto fail; /* allocation failure */
        }
    }

    output_pointer = output;
    /* loop through the string literal */
    while (input_pointer < input_end)
    {
        if (*input_pointer != '\\')
        {
            *output_pointer++ = *input_pointer++;
        }
        /* escape sequence */
        else
        {
            unsigned char sequence_length = 2;
            switch (input_pointer[1])
            {
                case 'b':
                    *output_pointer++ = '\b';
                    break;
                case 'f':
                    *output_pointer++ = '\f';
                    break;
                case 'n':
                    *output_pointer++ = '\n';
                    break;
                case 'r':
                    *output_pointer++ = '\r';
                    break;
                case 't':
                    *output_pointer++ = '\t';
                    break;
                case '\"':
                case '\\':
                case '/':
                    *output_pointer++ = input_pointer[1];
                    break;

                /* UTF-16 literal */
                case 'u':
                    sequence_length = utf16_literal_to_utf8(input_pointer, input_end, &output_pointer, error_pointer);
                    if (sequence_length == 0)
                    {
                        /* failed to convert UTF16-literal to UTF-8 */
                        goto fail;
                    }
                    break;

                default:
                    *error_pointer = input_pointer;
                    goto fail;
            }
            input_pointer += sequence_length;
        }
    }

    /* zero terminate the output */
    *output_pointer = '\0';

    item->type = cJSON_String;
    item->valuestring = (char*)output;

    return input_end + 1;

fail:
    if (output != NULL)
    {
        cJSON_free(output);
    }

    return NULL;
}

/* Render the cstring provided to an escaped version that can be printed. */
static unsigned char *print_string_ptr(const unsigned char * const input, printbuffer * const output_buffer)
{
    const unsigned char *input_pointer = NULL;
    unsigned char *output = NULL;
    unsigned char *output_pointer = NULL;
    size_t length = 0;
    cjbool contains_special_char = false;
    unsigned char token = '\0';

    if (output_buffer == NULL)
    {
        return NULL;
    }

    /* empty string */
    if (input == NULL)
    {
        output = ensure(output_buffer, 3);
        if (output == NULL)
        {
            return NULL;
        }
        strcpy((char*)output, "\"\"");

        return output;
    }

    /* set "flag" to 1 if something needs to be escaped */
    for (input_pointer = input; *input_pointer; input_pointer++)
    {
        contains_special_char |= (((*input_pointer > 0) && (*input_pointer < 32)) /* unprintable characters */
                || (*input_pointer == '\"') /* double quote */
                || (*input_pointer == '\\')) /* backslash */
            ? 1
            : 0;
    }
    /* no characters have to be escaped */
    if (!contains_special_char)
    {
        length = (size_t)(input_pointer - input);

        output = ensure(output_buffer, length + 3);
        if (output == NULL)
        {
            return NULL;
        }

        output_pointer = output;
        *output_pointer++ = '\"';
        strcpy((char*)output_pointer, (const char*)input);
        output_pointer[length] = '\"';
        output_pointer[length + 1] = '\0';

        return output;
    }

    input_pointer = input;
    /* calculate additional space that is needed for escaping */
    while ((token = *input_pointer))
    {
        ++length;
        if (strchr("\"\\\b\f\n\r\t", token))
        {
            length++; /* +1 for the backslash */
        }
        else if (token < 32)
        {
            length += 5; /* +5 for \uXXXX */
        }
        input_pointer++;
    }

    output = ensure(output_buffer, length + 3);
    if (output == NULL)
    {
        return NULL;
    }

    output_pointer = output;
    input_pointer = input;
    *output_pointer++ = '\"';
    /* copy the string */
    while (*input_pointer)
    {
        if ((*input_pointer > 31) && (*input_pointer != '\"') && (*input_pointer != '\\'))
        {
            /* normal character, copy */
            *output_pointer++ = *input_pointer++;
        }
        else
        {
            /* character needs to be escaped */
            *output_pointer++ = '\\';
            switch (token = *input_pointer++)
            {
                case '\\':
                    *output_pointer++ = '\\';
                    break;
                case '\"':
                    *output_pointer++ = '\"';
                    break;
                case '\b':
                    *output_pointer++ = 'b';
                    break;
                case '\f':
                    *output_pointer++ = 'f';
                    break;
                case '\n':
                    *output_pointer++ = 'n';
                    break;
                case '\r':
                    *output_pointer++ = 'r';
                    break;
                case '\t':
                    *output_pointer++ = 't';
                    break;
                default:
                    /* escape and print as unicode codepoint */
                    sprintf((char*)output_pointer, "u%04x", token);
                    output_pointer += 5;
                    break;
            }
        }
    }
    *output_pointer++ = '\"';
    *output_pointer++ = '\0';

    return output;
}

/* Invoke print_string_ptr (which is useful) on an item. */
static unsigned char *print_string(const cJSON * const item, printbuffer * const p)
{
    return print_string_ptr((unsigned char*)item->valuestring, p);
}

/* Predeclare these prototypes. */
static const unsigned char *parse_value(cJSON * const item, const unsigned char * const input, const unsigned char ** const ep);
static unsigned char *print_value(const cJSON *item, size_t depth, cjbool fmt, printbuffer *p);
static const unsigned char *parse_array(cJSON * const item, const unsigned char *input, const unsigned char ** const ep);
static unsigned char *print_array(const cJSON *item, size_t depth, cjbool fmt, printbuffer *p);
static const unsigned char *parse_object(cJSON * const item, const unsigned char *input, const unsigned char ** const ep);
static unsigned char *print_object(const cJSON *item, size_t depth, cjbool fmt, printbuffer *p);

/* Utility to jump whitespace and cr/lf */
static const unsigned char *skip_whitespace(const unsigned char *in)
{
    while (in && *in && (*in <= 32))
    {
        in++;
    }

    return in;
}

/* Parse an object - create a new root, and populate. */
cJSON *cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cjbool require_null_terminated)
{
    const unsigned char *end = NULL;
    /* use global error pointer if no specific one was given */
    const unsigned char **ep = return_parse_end ? (const unsigned char**)return_parse_end : &global_ep;
    cJSON *c = cJSON_New_Item();
    *ep = NULL;
    if (!c) /* memory fail */
    {
        return NULL;
    }

    end = parse_value(c, skip_whitespace((const unsigned char*)value), ep);
    if (!end)
    {
        /* parse failure. ep is set. */
        cJSON_Delete(c);
        return NULL;
    }

    /* if we require null-terminated JSON without appended garbage, skip and then check for a null terminator */
    if (require_null_terminated)
    {
        end = skip_whitespace(end);
        if (*end)
        {
            cJSON_Delete(c);
            *ep = end;
            return NULL;
        }
    }
    if (return_parse_end)
    {
        *return_parse_end = (const char*)end;
    }

    return c;
}

/* Default options for cJSON_Parse */
cJSON *cJSON_Parse(const char *value)
{
    return cJSON_ParseWithOpts(value, 0, 0);
}

#define min(a, b) ((a < b) ? a : b)

static unsigned char *print(const cJSON * const item, cjbool format)
{
    printbuffer buffer[1];
    unsigned char *printed = NULL;

    memset(buffer, 0, sizeof(buffer));

    /* create buffer */
    buffer->buffer = (unsigned char*) cJSON_malloc(256);
    if (buffer->buffer == NULL)
    {
        goto fail;
    }

    /* print the value */
    if (print_value(item, 0, format, buffer) == NULL)
    {
        goto fail;
    }
    buffer->offset = update(buffer); /* update the length of the string */

    /* copy the buffer over to a new one */
    printed = (unsigned char*) cJSON_malloc(buffer->offset + 1);
    if (printed == NULL)
    {
        goto fail;
    }
    strncpy((char*)printed, (char*)buffer->buffer, min(buffer->length, buffer->offset + 1));
    printed[buffer->offset] = '\0'; /* just to be sure */

    /* free the buffer */
    cJSON_free(buffer->buffer);

    return printed;

fail:
    if (buffer->buffer != NULL)
    {
        cJSON_free(buffer->buffer);
    }

    if (printed != NULL)
    {
        cJSON_free(printed);
    }

    return NULL;
}

/* Render a cJSON item/entity/structure to text. */
char *cJSON_Print(const cJSON *item)
{
    return (char*)print(item, true);
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    return (char*)print(item, false);
}

char *cJSON_PrintBuffered(const cJSON *item, int prebuffer, cjbool fmt)
{
    printbuffer p;

    if (prebuffer < 0)
    {
        return NULL;
    }

    p.buffer = (unsigned char*)cJSON_malloc((size_t)prebuffer);
    if (!p.buffer)
    {
        return NULL;
    }

    p.length = (size_t)prebuffer;
    p.offset = 0;
    p.noalloc = false;

    return (char*)print_value(item, 0, fmt, &p);
}

int cJSON_PrintPreallocated(cJSON *item, char *buf, const int len, const cjbool fmt)
{
    printbuffer p;

    if (len < 0)
    {
        return false;
    }

    p.buffer = (unsigned char*)buf;
    p.length = (size_t)len;
    p.offset = 0;
    p.noalloc = true;
    return print_value(item, 0, fmt, &p) != NULL;
}

/* Parser core - when encountering text, process appropriately. */
static const unsigned  char *parse_value(cJSON * const item, const unsigned char * const input, const unsigned char ** const error_pointer)
{
    if (input == NULL)
    {
        return NULL; /* no input */
    }

    /* parse the different types of values */
    /* null */
    if (!strncmp((const char*)input, "null", 4))
    {
        item->type = cJSON_NULL;
        return input + 4;
    }
    /* false */
    if (!strncmp((const char*)input, "false", 5))
    {
        item->type = cJSON_False;
        return input + 5;
    }
    /* true */
    if (!strncmp((const char*)input, "true", 4))
    {
        item->type = cJSON_True;
        item->valueint = 1;
        return input + 4;
    }
    /* string */
    if (*input == '\"')
    {
        return parse_string(item, input, error_pointer);
    }
    /* number */
    if ((*input == '-') || ((*input >= '0') && (*input <= '9')))
    {
        return parse_number(item, input);
    }
    /* array */
    if (*input == '[')
    {
        return parse_array(item, input, error_pointer);
    }
    /* object */
    if (*input == '{')
    {
        return parse_object(item, input, error_pointer);
    }

    /* failure. */
    *error_pointer = input;
    return NULL;
}

/* Render a value to text. */
static unsigned char *print_value(const cJSON *item, size_t depth, cjbool fmt, printbuffer *p)
{
    unsigned char *out = NULL;

    if (!item)
    {
        return NULL;
    }

    if (p == NULL)
    {
        return NULL;
    }

    switch ((item->type) & 0xFF)
    {
        case cJSON_NULL:
            out = ensure(p, 5);
            if (out != NULL)
            {
                strcpy((char*)out, "null");
            }
            break;
        case cJSON_False:
            out = ensure(p, 6);
            if (out != NULL)
            {
                strcpy((char*)out, "false");
            }
            break;
        case cJSON_True:
            out = ensure(p, 5);
            if (out != NULL)
            {
                strcpy((char*)out, "true");
            }
            break;
        case cJSON_Number:
            out = print_number(item, p);
            break;
        case cJSON_Raw:
        {
            size_t raw_length = 0;
            if (item->valuestring == NULL)
            {
                if (!p->noalloc)
                {
                    cJSON_free(p->buffer);
                }
                out = NULL;
                break;
            }

            raw_length = strlen(item->valuestring) + sizeof('\0');
            out = ensure(p, raw_length);
            if (out != NULL)
            {
                memcpy(out, item->valuestring, raw_length);
            }
            break;
        }
        case cJSON_String:
            out = print_string(item, p);
            break;
        case cJSON_Array:
            out = print_array(item, depth, fmt, p);
            break;
        case cJSON_Object:
            out = print_object(item, depth, fmt, p);
            break;
        default:
            out = NULL;
            break;
    }

    return out;
}

/* Build an array from input text. */
static const unsigned char *parse_array(cJSON * const item, const unsigned char *input, const unsigned char ** const error_pointer)
{
    cJSON *head = NULL; /* head of the linked list */
    cJSON *current_item = NULL;

    if (*input != '[')
    {
        /* not an array */
        *error_pointer = input;
        goto fail;
    }

    input = skip_whitespace(input + 1);
    if (*input == ']')
    {
        /* empty array */
        goto success;
    }

    /* step back to character in front of the first element */
    input--;
    /* loop through the comma separated array elements */
    do
    {
        /* allocate next item */
        cJSON *new_item = cJSON_New_Item();
        if (new_item == NULL)
        {
            goto fail; /* allocation failure */
        }

        /* attach next item to list */
        if (head == NULL)
        {
            /* start the linked list */
            current_item = head = new_item;
        }
        else
        {
            /* add to the end and advance */
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        /* parse next value */
        input = skip_whitespace(input + 1);
        input = parse_value(current_item, input, error_pointer);
        input = skip_whitespace(input);
        if (input == NULL)
        {
            goto fail; /* failed to parse value */
        }
    }
    while (*input == ',');

    if (*input != ']')
    {
        *error_pointer = input;
        goto fail; /* expected end of array */
    }

success:
    item->type = cJSON_Array;
    item->child = head;

    return input + 1;

fail:
    if (head != NULL)
    {
        cJSON_Delete(head);
    }

    return NULL;
}

/* Render an array to text */
static unsigned char *print_array(const cJSON *item, size_t depth, cjbool fmt, printbuffer *p)
{
    unsigned char *out = NULL;
    unsigned char *ptr = NULL;
    size_t len = 5;
    cJSON *child = item->child;
    size_t numentries = 0;
    size_t i = 0;
    cjbool fail = false;

    if (p == NULL)
    {
        return NULL;
    }

    /* How many entries in the array? */
    while (child)
    {
        numentries++;
        child = child->next;
    }

    /* Explicitly handle numentries == 0 */
    if (!numentries)
    {
        out = ensure(p, 3);
        if (out != NULL)
        {
            strcpy((char*)out, "[]");
        }

        return out;
    }

    /* Compose the output array. */
    /* opening square bracket */
    i = p->offset;
    ptr = ensure(p, 1);
    if (ptr == NULL)
    {
        return NULL;
    }
    *ptr = '[';
    p->offset++;

    child = item->child;
    while (child && !fail)
    {
        if (!print_value(child, depth + 1, fmt, p))
        {
            return NULL;
        }
        p->offset = update(p);
        if (child->next)
        {
            len = fmt ? 2 : 1;
            ptr = ensure(p, len + 1);
            if (ptr == NULL)
            {
                return NULL;
            }
            *ptr++ = ',';
            if(fmt)
            {
                *ptr++ = ' ';
            }
            *ptr = '\0';
            p->offset += len;
        }
        child = child->next;
    }
    ptr = ensure(p, 2);
    if (ptr == NULL)
    {
        return NULL;
    }
    *ptr++ = ']';
    *ptr = '\0';
    out = (p->buffer) + i;

    return out;
}

/* Build an object from the text. */
static const unsigned char *parse_object(cJSON * const item, const unsigned char *input, const unsigned char ** const error_pointer)
{
    cJSON *head = NULL; /* linked list head */
    cJSON *current_item = NULL;

    if (*input != '{')
    {
        *error_pointer = input;
        goto fail; /* not an object */
    }

    input = skip_whitespace(input + 1);
    if (*input == '}')
    {
        goto success; /* empty object */
    }

    /* step back to character in front of the first element */
    input--;
    /* loop through the comma separated array elements */
    do
    {
        /* allocate next item */
        cJSON *new_item = cJSON_New_Item();
        if (new_item == NULL)
        {
            goto fail; /* allocation failure */
        }

        /* attach next item to list */
        if (head == NULL)
        {
            /* start the linked list */
            current_item = head = new_item;
        }
        else
        {
            /* add to the end and advance */
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        /* parse the name of the child */
        input = skip_whitespace(input + 1);
        input = parse_string(current_item, input, error_pointer);
        input = skip_whitespace(input);
        if (input == NULL)
        {
            goto fail; /* faile to parse name */
        }

        /* swap valuestring and string, because we parsed the name */
        current_item->string = current_item->valuestring;
        current_item->valuestring = NULL;

        if (*input != ':')
        {
            *error_pointer = input;
            goto fail; /* invalid object */
        }

        /* parse the value */
        input = skip_whitespace(input + 1);
        input = parse_value(current_item, input, error_pointer);
        input = skip_whitespace(input);
        if (input == NULL)
        {
            goto fail; /* failed to parse value */
        }
    }
    while (*input == ',');

    if (*input != '}')
    {
        *error_pointer = input;
        goto fail; /* expected end of object */
    }

success:
    item->type = cJSON_Object;
    item->child = head;

    return input + 1;

fail:
    if (head != NULL)
    {
        cJSON_Delete(head);
    }

    return NULL;
}

/* Render an object to text. */
static unsigned char *print_object(const cJSON *item, size_t depth, cjbool fmt, printbuffer *p)
{
    unsigned char *out = NULL;
    unsigned char *ptr = NULL;
    size_t len = 7;
    size_t i = 0;
    size_t j = 0;
    cJSON *child = item->child;
    size_t numentries = 0;

    if (p == NULL)
    {
        return NULL;
    }

    /* Count the number of entries. */
    while (child)
    {
        numentries++;
        child = child->next;
    }

    /* Explicitly handle empty object case */
    if (!numentries)
    {
        out = ensure(p, fmt ? depth + 4 : 3);
        if (out == NULL)
        {
            return NULL;
        }
        ptr = out;
        *ptr++ = '{';
        if (fmt) {
            *ptr++ = '\n';
            for (i = 0; i < depth; i++)
            {
                *ptr++ = '\t';
            }
        }
        *ptr++ = '}';
        *ptr++ = '\0';

        return out;
    }

    /* Compose the output: */
    i = p->offset;
    len = fmt ? 2 : 1; /* fmt: {\n */
    ptr = ensure(p, len + 1);
    if (ptr == NULL)
    {
        return NULL;
    }

    *ptr++ = '{';
    if (fmt)
    {
        *ptr++ = '\n';
    }
    *ptr = '\0';
    p->offset += len;

    child = item->child;
    depth++;
    while (child)
    {
        if (fmt)
        {
            ptr = ensure(p, depth);
            if (ptr == NULL)
            {
                return NULL;
            }
            for (j = 0; j < depth; j++)
            {
                *ptr++ = '\t';
            }
            p->offset += depth;
        }

        /* print key */
        if (!print_string_ptr((unsigned char*)child->string, p))
        {
            return NULL;
        }
        p->offset = update(p);

        len = fmt ? 2 : 1;
        ptr = ensure(p, len);
        if (ptr == NULL)
        {
            return NULL;
        }
        *ptr++ = ':';
        if (fmt)
        {
            *ptr++ = '\t';
        }
        p->offset+=len;

        /* print value */
        if (!print_value(child, depth, fmt, p))
        {
            return NULL;
        }
        p->offset = update(p);

        /* print comma if not last */
        len = (size_t) (fmt ? 1 : 0) + (child->next ? 1 : 0);
        ptr = ensure(p, len + 1);
        if (ptr == NULL)
        {
            return NULL;
        }
        if (child->next)
        {
            *ptr++ = ',';
        }

        if (fmt)
        {
            *ptr++ = '\n';
        }
        *ptr = '\0';
        p->offset += len;

        child = child->next;
    }

    ptr = ensure(p, fmt ? (depth + 1) : 2);
    if (ptr == NULL)
    {
        return NULL;
    }
    if (fmt)
    {
        for (i = 0; i < (depth - 1); i++)
        {
            *ptr++ = '\t';
        }
    }
    *ptr++ = '}';
    *ptr = '\0';
    out = (p->buffer) + i;

    return out;
}

/* Get Array size/item / object item. */
int cJSON_GetArraySize(const cJSON *array)
{
    cJSON *c = array->child;
    size_t i = 0;
    while(c)
    {
        i++;
        c = c->next;
    }

    /* FIXME: Can overflow here. Cannot be fixed without breaking the API */

    return (int)i;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int item)
{
    cJSON *c = array ? array->child : NULL;
    while (c && item > 0)
    {
        item--;
        c = c->next;
    }

    return c;
}

cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string)
{
    cJSON *c = object ? object->child : NULL;
    while (c && cJSON_strcasecmp((unsigned char*)c->string, (const unsigned char*)string))
    {
        c = c->next;
    }
    return c;
}

cjbool cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string) ? 1 : 0;
}

/* Utility for array list handling. */
static void suffix_object(cJSON *prev, cJSON *item)
{
    prev->next = item;
    item->prev = prev;
}

/* Utility for handling references. */
static cJSON *create_reference(const cJSON *item)
{
    cJSON *ref = cJSON_New_Item();
    if (!ref)
    {
        return NULL;
    }
    memcpy(ref, item, sizeof(cJSON));
    ref->string = NULL;
    ref->type |= cJSON_IsReference;
    ref->next = ref->prev = NULL;
    return ref;
}

/* Add item to array/object. */
void cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    cJSON *child = NULL;

    if ((item == NULL) || (array == NULL))
    {
        return;
    }

    child = array->child;

    if (child == NULL)
    {
        /* list is empty, start new one */
        array->child = item;
    }
    else
    {
        /* append to the end */
        while (child->next)
        {
            child = child->next;
        }
        suffix_object(child, item);
    }
}

void   cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    /* call cJSON_AddItemToObjectCS for code reuse */
    cJSON_AddItemToObjectCS(object, (char*)cJSON_strdup((const unsigned char*)string), item);
    /* remove cJSON_StringIsConst flag */
    item->type &= ~cJSON_StringIsConst;
}

/* Add an item to an object with constant string as key */
void   cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
    if (!item)
    {
        return;
    }
    if (!(item->type & cJSON_StringIsConst) && item->string)
    {
        cJSON_free(item->string);
    }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
    item->string = (char*)string;
#pragma GCC diagnostic pop
    item->type |= cJSON_StringIsConst;
    cJSON_AddItemToArray(object, item);
}

void cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
    cJSON_AddItemToArray(array, create_reference(item));
}

void cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
    cJSON_AddItemToObject(object, string, create_reference(item));
}

static cJSON *DetachItemFromArray(cJSON *array, size_t which)
{
    cJSON *c = array->child;
    while (c && (which > 0))
    {
        c = c->next;
        which--;
    }
    if (!c)
    {
        /* item doesn't exist */
        return NULL;
    }
    if (c->prev)
    {
        /* not the first element */
        c->prev->next = c->next;
    }
    if (c->next)
    {
        c->next->prev = c->prev;
    }
    if (c==array->child)
    {
        array->child = c->next;
    }
    /* make sure the detached item doesn't point anywhere anymore */
    c->prev = c->next = NULL;

    return c;
}
cJSON *cJSON_DetachItemFromArray(cJSON *array, int which)
{
    if (which < 0)
    {
        return NULL;
    }

    return DetachItemFromArray(array, (size_t)which);
}

void cJSON_DeleteItemFromArray(cJSON *array, int which)
{
    cJSON_Delete(cJSON_DetachItemFromArray(array, which));
}

cJSON *cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
    size_t i = 0;
    cJSON *c = object->child;
    while (c && cJSON_strcasecmp((unsigned char*)c->string, (const unsigned char*)string))
    {
        i++;
        c = c->next;
    }
    if (c)
    {
        return DetachItemFromArray(object, i);
    }

    return NULL;
}

void cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}

/* Replace array/object items with new ones. */
void cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem)
{
    cJSON *c = array->child;
    while (c && (which > 0))
    {
        c = c->next;
        which--;
    }
    if (!c)
    {
        cJSON_AddItemToArray(array, newitem);
        return;
    }
    newitem->next = c;
    newitem->prev = c->prev;
    c->prev = newitem;
    if (c == array->child)
    {
        array->child = newitem;
    }
    else
    {
        newitem->prev->next = newitem;
    }
}

static void ReplaceItemInArray(cJSON *array, size_t which, cJSON *newitem)
{
    cJSON *c = array->child;
    while (c && (which > 0))
    {
        c = c->next;
        which--;
    }
    if (!c)
    {
        return;
    }
    newitem->next = c->next;
    newitem->prev = c->prev;
    if (newitem->next)
    {
        newitem->next->prev = newitem;
    }
    if (c == array->child)
    {
        array->child = newitem;
    }
    else
    {
        newitem->prev->next = newitem;
    }
    c->next = c->prev = NULL;
    cJSON_Delete(c);
}
void cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
    if (which < 0)
    {
        return;
    }

    ReplaceItemInArray(array, (size_t)which, newitem);
}

void cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem)
{
    size_t i = 0;
    cJSON *c = object->child;
    while(c && cJSON_strcasecmp((unsigned char*)c->string, (const unsigned char*)string))
    {
        i++;
        c = c->next;
    }
    if(c)
    {
        /* free the old string if not const */
        if (!(newitem->type & cJSON_StringIsConst) && newitem->string)
        {
             cJSON_free(newitem->string);
        }

        newitem->string = (char*)cJSON_strdup((const unsigned char*)string);
        ReplaceItemInArray(object, i, newitem);
    }
}

/* Create basic types: */
cJSON *cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item();
    if(item)
    {
        item->type = cJSON_NULL;
    }

    return item;
}

cJSON *cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item();
    if(item)
    {
        item->type = cJSON_True;
    }

    return item;
}

cJSON *cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item();
    if(item)
    {
        item->type = cJSON_False;
    }

    return item;
}

cJSON *cJSON_CreateBool(cjbool b)
{
    cJSON *item = cJSON_New_Item();
    if(item)
    {
        item->type = b ? cJSON_True : cJSON_False;
    }

    return item;
}

cJSON *cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item();
    if(item)
    {
        item->type = cJSON_Number;
        item->valuedouble = num;

        /* use saturation in case of overflow */
        if (num >= INT_MAX)
        {
            item->valueint = INT_MAX;
        }
        else if (num <= INT_MIN)
        {
            item->valueint = INT_MIN;
        }
        else
        {
            item->valueint = (int)num;
        }
    }

    return item;
}

cJSON *cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item();
    if(item)
    {
        item->type = cJSON_String;
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)string);
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

extern cJSON *cJSON_CreateRaw(const char *raw)
{
    cJSON *item = cJSON_New_Item();
    if(item)
    {
        item->type = cJSON_Raw;
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)raw);
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

cJSON *cJSON_CreateArray(void)
{
    cJSON *item = cJSON_New_Item();
    if(item)
    {
        item->type=cJSON_Array;
    }

    return item;
}

cJSON *cJSON_CreateObject(void)
{
    cJSON *item = cJSON_New_Item();
    if (item)
    {
        item->type = cJSON_Object;
    }

    return item;
}

/* Create Arrays: */
cJSON *cJSON_CreateIntArray(const int *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if (count < 0)
    {
        return NULL;
    }

    a = cJSON_CreateArray();
    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if (!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    return a;
}

cJSON *cJSON_CreateFloatArray(const float *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if (count < 0)
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    return a;
}

cJSON *cJSON_CreateDoubleArray(const double *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if (count < 0)
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0;a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    return a;
}

cJSON *cJSON_CreateStringArray(const char **strings, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if (count < 0)
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for (i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateString(strings[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p,n);
        }
        p = n;
    }

    return a;
}

/* Duplication */
cJSON *cJSON_Duplicate(const cJSON *item, cjbool recurse)
{
    cJSON *newitem = NULL;
    cJSON *child = NULL;
    cJSON *next = NULL;
    cJSON *newchild = NULL;

    /* Bail on bad ptr */
    if (!item)
    {
        goto fail;
    }
    /* Create new item */
    newitem = cJSON_New_Item();
    if (!newitem)
    {
        goto fail;
    }
    /* Copy over all vars */
    newitem->type = item->type & (~cJSON_IsReference);
    newitem->valueint = item->valueint;
    newitem->valuedouble = item->valuedouble;
    if (item->valuestring)
    {
        newitem->valuestring = (char*)cJSON_strdup((unsigned char*)item->valuestring);
        if (!newitem->valuestring)
        {
            goto fail;
        }
    }
    if (item->string)
    {
        newitem->string = (item->type&cJSON_StringIsConst) ? item->string : (char*)cJSON_strdup((unsigned char*)item->string);
        if (!newitem->string)
        {
            goto fail;
        }
    }
    /* If non-recursive, then we're done! */
    if (!recurse)
    {
        return newitem;
    }
    /* Walk the ->next chain for the child. */
    child = item->child;
    while (child != NULL)
    {
        newchild = cJSON_Duplicate(child, true); /* Duplicate (with recurse) each item in the ->next chain */
        if (!newchild)
        {
            goto fail;
        }
        if (next != NULL)
        {
            /* If newitem->child already set, then crosswire ->prev and ->next and move on */
            next->next = newchild;
            newchild->prev = next;
            next = newchild;
        }
        else
        {
            /* Set newitem->child and move to it */
            newitem->child = newchild;
            next = newchild;
        }
        child = child->next;
    }

    return newitem;

fail:
    if (newitem != NULL)
    {
        cJSON_Delete(newitem);
    }

    return NULL;
}

void cJSON_Minify(char *json)
{
    unsigned char *into = (unsigned char*)json;
    while (*json)
    {
        if (*json == ' ')
        {
            json++;
        }
        else if (*json == '\t')
        {
            /* Whitespace characters. */
            json++;
        }
        else if (*json == '\r')
        {
            json++;
        }
        else if (*json=='\n')
        {
            json++;
        }
        else if ((*json == '/') && (json[1] == '/'))
        {
            /* double-slash comments, to end of line. */
            while (*json && (*json != '\n'))
            {
                json++;
            }
        }
        else if ((*json == '/') && (json[1] == '*'))
        {
            /* multiline comments. */
            while (*json && !((*json == '*') && (json[1] == '/')))
            {
                json++;
            }
            json += 2;
        }
        else if (*json == '\"')
        {
            /* string literals, which are \" sensitive. */
            *into++ = (unsigned char)*json++;
            while (*json && (*json != '\"'))
            {
                if (*json == '\\')
                {
                    *into++ = (unsigned char)*json++;
                }
                *into++ = (unsigned char)*json++;
            }
            *into++ = (unsigned char)*json++;
        }
        else
        {
            /* All other characters. */
            *into++ = (unsigned char)*json++;
        }
    }

    /* and null-terminate. */
    *into = '\0';
}
