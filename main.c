#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <regex.h>
#include "pdf.h"


#undef TAG
#define TAG "pdfgrep"


#define _P(_tag, ...) \
    do {printf("["TAG"]"_tag" "__VA_ARGS__); putc('\n', stdout);} while(0)


#define P(...) do {printf(__VA_ARGS__); putc('\n', stdout);} while(0)

#undef D
#ifdef DEBUG
#define D(...) _P("[debug]", __VA_ARGS__);
#else
#define D(...) 
#endif


#define ERR(_expr, _fail, ...) \
    if ((_expr) _fail) {                    \
        fprintf(stderr, "["TAG"] Error: " __VA_ARGS__);\
        fputc('\n', stderr); \
        exit(EXIT_FAILURE);\
    }


static void usage(const char *execname)
{
    printf("Usage: %s <file> <-e regexp>\n", execname);
    exit(EXIT_SUCCESS);
}


/* Gets called back from the decode routine when the buffer is full */
static decode_exit_e regexp_callback(decode_t *decode)
{
    int match = regexec(
        (regex_t*)decode->user_data, decode->buffer, 0, NULL, 0);

    if (match == 0)
    {
        P("%s: Found match on page %d", decode->pdf->fname, decode->pg_num);
        return DECODE_DONE;
    }

    return DECODE_CONTINUE;
}


static void run_regex(const pdf_t *pdf, const regex_t *re)
{
    const kid_t *kid;
    char buf[2048] = {0};
    decode_t decode;

    decode.pdf = pdf;
    decode.callback = regexp_callback;
    decode.buffer = buf;
    decode.buffer_length = sizeof(buf);
    decode.user_data = (void *)re;

    for (kid=pdf->kids; kid; kid=kid->next)
    {
        decode.pg_num = kid->pg_num;
        decode.buffer_used = 0;
        pdf_decode_page(&decode);
    }
}


static decode_exit_e print_buffer_callback(decode_t *decode)
{
    printf(decode->buffer);
    memset(decode->buffer, 0, decode->buffer_length);
    decode->buffer_used = 0;
    return DECODE_CONTINUE;
}


static void debug_page(const pdf_t *pdf, int pg_num)
{
    decode_t decode;
    char buf[2048] = {0};

    decode.pdf = pdf;
    decode.pg_num = pg_num;
    decode.buffer = buf;
    decode.buffer_length = sizeof(buf) - 1;
    decode.buffer_used = 0;
    decode.callback = print_buffer_callback;
    decode.user_data = NULL;
    pdf_decode_page(&decode);
}


int main(int argc, char **argv)
{
    int i, fd;
#ifdef DEBUG
    int debug_page_num = 0;
#endif
    pdf_t pdf;
    regex_t re;
    struct stat stat;
    const char *fname = NULL, *expr = NULL;

    for (i=1; i<argc; ++i)
    {
        if (strncmp(argv[i], "-e", 2) == 0)
        {
            /* -e "expr" or -e"expr" */
            if (i+1<argc && argv[i+1][0] != '-')
              expr = argv[++i];
            else if (strlen(argv[i]) > 2)
              expr = argv[i] + 2;
            else 
              usage(argv[0]);
        }
#ifdef DEBUG
        else if (strncmp(argv[i], "-d", 2) == 0)
          debug_page_num = atoi(argv[++i]);
#endif
        else if (argv[i][0] != '-')
          fname = argv[i];
        else
          usage(argv[0]);
    }

    if (!fname || !expr)
      usage(argv[0]);

    D("File: %s", fname);
    D("Expr: %s", expr);

    /* Build regex */
    ERR(regcomp(&re, expr, REG_EXTENDED), !=0,
        "Could not build regex");
   
    /* New pdf */ 
    memset(&pdf, 0, sizeof(pdf_t));
    pdf.fname = fname;

    /* Open and map the file into memory */
    ERR((fd = open(fname, O_RDONLY)), ==-1, "Opening file '%s'", fname);
    ERR(fstat(fd, &stat), ==-1, "Obtaining file size");
    ERR((pdf.data=mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0)),==NULL,
        "Mapping file into memory");
    pdf.len = stat.st_size;

    /* Get the initial cross reference table */
    ERR(pdf_load_data(&pdf), != PDF_OK, "Could not load pdf");

    /* Run the match routine */
    run_regex(&pdf, &re);

#ifdef DEBUG
    if (debug_page_num)
      debug_page(&pdf, debug_page_num);
#endif

#if 0
    print_page_tree(&pdf);
#endif

    /* Clean up */
    close(fd);
    regfree(&re);

    return 0;
}
