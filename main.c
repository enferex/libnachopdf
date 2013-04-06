#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <regex.h>
#include <zlib.h>
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


/* If the value idx is greater than the buffer size,
 * issue a callback to the decode listner.
 * 
 * 'buf_idx' -- Should have the current buffer size, it will be updated if the
 *              callback is issued.
 */
static decode_exit_e cb_if_full(decode_t *decode, size_t *buf_idx)
{
    if (*buf_idx > decode->buffer_length)
    {
        decode->buffer_used = *buf_idx;
        if ((decode->callback(decode)) == DECODE_DONE)
          return DECODE_DONE;
        *buf_idx = decode->buffer_used;
    }

    return DECODE_CONTINUE;
}


static decode_exit_e decode_ps(
    unsigned char *data,
    off_t          length,
    decode_t      *decode)
{
    unsigned char c;
    off_t i=0, stack_idx=0;
    double Tc=0.0, val_stack[2] = {0.0, 0.0};
    static const int X=0, Y=1;
    size_t bufidx = 0;
    char *buf = decode->buffer;

#ifdef DEBUG_PS
    for (i=0; i<length; ++i)
      putc(data[i], stdout);
#endif

    i = 0;
    while (i < length)
    {
        /* If we have filled the buffer... callback */
        if (cb_if_full(decode, &bufidx) == DECODE_DONE)
          return DECODE_DONE;

        /* Parse... */
        c = data[i];
        if (isspace(c))
        {
            i++;
            continue;
        }

        /* Text to display */
        if (c == '(')
        {
            ++i;
            while (data[i] != ')')
            {
                if (cb_if_full(decode, &bufidx))
                  return DECODE_DONE;
                buf[bufidx++] = data[i];
                ++i;
            }
        }

        /* Position value */
        else if (isdigit(c) || c == '-')
        {
            val_stack[stack_idx++%2] = atof((char *)data + i);
            while (isdigit(data[i]) || data[i] == '.' || data[i] == '-')
              ++i;
        }

        /* New line (skip other display options like Tf and Tj */
        else if (c == 'T') 
        {
            c = data[++i];
            /* Newline (or space) */
            if ((c=='D' || c=='d' || c=='*'))
            {
                if (val_stack[Y] != 0.0)
                  buf[bufidx++] = '\n';
                else if (Tc>=0.0 && val_stack[X]>0.0)
                  buf[bufidx++] = ' ';
            }
            else if (c == 'c')
              Tc = val_stack[X];
            val_stack[0] = val_stack[1] = 0.0;
            stack_idx = 0;
        }

        /* New line */
        else if (c == '\'' || c == '"')
          buf[bufidx++] = '\n';

        else
          ++i;
    }

    /* Done decoding call the callback */
    decode->buffer_used = bufidx;
    return decode->callback(decode);
}


static decode_exit_e decode_flate(
    iter_t   *itr,
    off_t     length,
    decode_t *decode)
{
    int ret;
    off_t n_read, to_read;
    decode_exit_e de;
    const off_t block_size = 1024;
    unsigned char in[block_size], out[block_size];
    z_stream stream;

    memset(&stream, 0, sizeof(z_stream));
    if ((ret = inflateInit(&stream)) != Z_OK)
      return DECODE_DONE;

    n_read = 0;

    /* Thanks to http://www.zlib.net/zpipe.c for the following */
    do
    {
        to_read = block_size - (n_read % block_size);
        stream.avail_in = to_read;
        memcpy(in, ITR_ADDR(itr) + n_read, to_read);
        stream.next_in = in;
        n_read += to_read;
        do
        {
            stream.avail_out = block_size;
            stream.next_out = out;
            ret = inflate(&stream, Z_NO_FLUSH);
            switch (ret)
            {
                case Z_NEED_DICT:
                case Z_DATA_ERROR:
                case Z_MEM_ERROR:
                    inflateEnd(&stream);
                    return DECODE_DONE;
            }

            /* Decode the compressed data (ps format) */
            de = decode_ps(out, block_size - stream.avail_out, decode);
            if (de != DECODE_CONTINUE)
              return de;

        /* If more data... keep on decompressing */
        } while (stream.avail_out == 0);
    } while (ret != Z_STREAM_END);

    inflateEnd(&stream);
    return DECODE_DONE;
}


static void decode_page(decode_t *decode)
{
    const kid_t *k;
    off_t pg_length, stream_start;
    obj_t obj;
    iter_t *itr = iter_new(decode->pdf);

    for (k=decode->pdf->kids; k; k=k->next)
      if (k->pg_num == decode->pg_num)
        break;

    if (!k)
      return;

    /* Get contents */
    ERR(pdf_get_object(decode->pdf, k->id, &obj), ==false,
        "Could not locate page object");
    ERR(find_in_object(itr, obj, "/Contents"), ==false,
        "Could not locate page contents");
    seek_next_nonwhitespace(itr);
    ERR(pdf_get_object(decode->pdf, ITR_VAL_INT(itr), &obj), ==false,
        "Could not locate page");

    /* Get the pages data */
    ERR(find_in_object(itr, obj, "/Length"), ==false,
        "Could not find length of the pages data");
    seek_next_nonwhitespace(itr);
    pg_length = ITR_VAL_INT(itr);
    D("Decoding page %d (%lu bytes)", decode->pg_num, pg_length);

    /* Get beginning of stream */
    seek_string(itr, "stream");
    seek_next(itr, '\n');
    iter_next(itr);
    stream_start = ITR_POS(itr);

    /* Decode the data */
    if (find_in_object(itr, obj, "/Filter"))
      if (find_in_object(itr, obj, "/FlateDecode"))
      {
          iter_set(itr, stream_start);
          if (decode_flate(itr, pg_length, decode) == DECODE_DONE)
            return;
      }
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
        decode_page(&decode);
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
    decode_page(&decode);
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
