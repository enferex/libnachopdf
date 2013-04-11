#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pdf.h"

static void usage(const char *execname)
{
    printf("Usage: %s <pdf>\n", execname);
    exit(EXIT_SUCCESS);
}


/* Ignore everything past the first newline character */
static decode_exit_e get_title(decode_t *decode)
{
    if (strchr(decode->buffer, '\n'))
      *(strchr(decode->buffer, '\n')) = '\0';
    printf("Title: %s\n", decode->buffer);
    return DECODE_DONE;
}

int main(int argc, char **argv)
{
    pdf_t *pdf;
    decode_t decode;
    char buf[256];

    if (argc != 2)
      usage(argv[0]);

    pdf = pdf_new(argv[1]);

    memset(&decode, 0, sizeof(decode_t));
    decode.pdf = pdf;
    decode.pg_num = 1;
    decode.callback = get_title;
    decode.buffer = buf;
    decode.buffer_length = sizeof(buf);
    if (pdf_decode_page(&decode) == PDF_ERR)
      fprintf(stderr, "Error decoding pdf\n");

    pdf_destroy(pdf);
    return 0;
}
