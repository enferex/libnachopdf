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


#define TAG "pdfgrep"


#define _P(_tag, ...) \
    do {printf("["TAG"]"_tag" "__VA_ARGS__); putc('\n', stdout);} while(0)


#define P(...) do {printf(__VA_ARGS__); putc('\n', stdout);} while(0)


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


/* Use this for all offsets */
#ifndef __off_t_defined
typedef unsigned long long off_t;
#endif
#define NOT_FOUND false


/* Entry for a cross reference table */
typedef struct {off_t offset; off_t generation; char is_free;} xref_entry_t;


/* Cross reference table */
typedef struct {
    int           n_entries;
    xref_entry_t *entries;
    off_t         first_entry_id;
    off_t         root_obj;
} xref_t;


/* Page type (just keep the kids not their parents) */
typedef struct _kid_t {int pg_num; off_t id; struct _kid_t *next;} kid_t;


/* Data type: Contains a pointer to the raw pdf data */
typedef struct {
    const char   *data;
    const char   *fname; /* File name */
    size_t        len;
    int           ver_major, ver_minor;
    int           n_xrefs;
    xref_t      **xrefs;
    kid_t        *kids; /* Linked-list of all pages */
}pdf_t;


/* Range type */
typedef struct {off_t id; off_t begin; off_t end;} obj_t;


/* Iterator type: Index into data */
typedef struct {off_t idx; const pdf_t *pdf;} iter_t;
#define ITR_VAL(_itr)       _itr->pdf->data[_itr->idx]
#define ITR_VAL_INT(_itr)   atoll(_itr->pdf->data + _itr->idx)
#define ITR_VAL_STR(_itr)   (char *)(_itr->pdf->data + _itr->idx)
#define ITR_POS(_itr)       _itr->idx
#define ITR_ADDR(_itr)      (_itr->pdf->data + _itr->idx)
#define ITR_IN_BOUNDS(_itr) ((_itr->idx>-1 && (_itr->idx < _itr->pdf->len)))
#define ITR_IN_BOUNDS_V(_itr, _val) \
    (((_itr->idx+_val)>-1) && ((_itr->idx+_val) < _itr->pdf->len))


/* Decoding return values, all decoding routines and the callback return one
 * of these values.
 */
typedef enum {DECODE_DONE, DECODE_CONTINUE} decode_exit_e;


/* Function pointer: Called back by decoding routine when buffer in decode_t is
 * full AND when decoding is done.
 */
struct _decode_t;
typedef  decode_exit_e (*decode_cb)(struct _decode_t *decode);


/* For decoding data */
typedef struct _decode_t
{
    const pdf_t *pdf;
    int          pg_num;
    decode_cb    callback;

    /* Decoded data will end up here... user must give a buffer and its length.
     * the buffer is NOT null terminated by the decoing routines.
     *
     * Decoding routines will place the length decoded into the buffer_used
     * when callback is called.
     *
     * Decoding routines call 'callback' when the buffer is full and when done
     * decoding.
     */
    char   *buffer;
    size_t  buffer_length; /* Should never change once set */
    size_t  buffer_used;

    /* Stash anything here, the pdf library should never touch this... like a
     * Swiss bank of data.
     */
    void *user_data;

} decode_t;


static void usage(const char *execname)
{
    printf("Usage: %s <file> <-e regexp>\n", execname);
    exit(EXIT_SUCCESS);
}


static void get_version(pdf_t *pdf)
{
    ERR(sscanf(pdf->data,"%%PDF-%d.%d",&pdf->ver_major,&pdf->ver_minor), !=2,
        "Bad version string");
    D("PDF Version: %d.%d", pdf->ver_major, pdf->ver_minor);
}


static inline void iter_prev(iter_t *itr)
{
    --itr->idx;
}


static inline void iter_next(iter_t *itr)
{
    ++itr->idx;
}


/* Keep moving backwards until we hit 'match' */
static void seek_next(iter_t *itr, char match)
{
    /* If we are already on the character, backup one */
    if (ITR_IN_BOUNDS(itr) && ITR_VAL(itr) == match)
      iter_next(itr);

    while (ITR_IN_BOUNDS(itr) && ITR_VAL(itr) != match)
      iter_next(itr);
}


/* Keep moving backwards until we hit 'match' */
static void find_prev(iter_t *itr, char match)
{
    /* If we are already on the character, backup one */
    if (ITR_IN_BOUNDS(itr) && ITR_VAL(itr) == match)
      iter_prev(itr);

    while (ITR_IN_BOUNDS(itr) && ITR_VAL(itr) != match)
      iter_prev(itr);
}


static void seek_previous_line(iter_t *itr)
{
    find_prev(itr, '\n'); /* Rewind to this line's start */
    find_prev(itr, '\n'); /* Beginning of previous line  */
    iter_next(itr);
}


static void seek_next_line(iter_t *itr)
{
    seek_next(itr, '\n');
    iter_next(itr);
}


static iter_t *new_iter(const pdf_t *pdf, off_t start_offset)
{
    iter_t *itr = malloc(sizeof(iter_t));
    if (start_offset == -1)
      itr->idx = pdf->len - 1;
    else
      itr->idx = start_offset;

    itr->pdf = pdf;

    if (!ITR_IN_BOUNDS(itr))
      abort();

    return itr;
}


static void destroy_iter(iter_t *itr)
{
    free(itr);
}


static void iter_set(iter_t *itr, off_t offset)
{
    itr->idx = offset;
    if (!ITR_IN_BOUNDS(itr))
      abort();
}


/* Returns true if found, false if not found */
static _Bool seek_string(iter_t *itr, const char *match)
{
    const char *en, *st = ITR_ADDR(itr);
    if (!(en = strstr(st, match)))
      return NOT_FOUND;
    iter_set(itr, ITR_POS(itr) + en - st);
    return true;
}


/* Creates a fresh object */
static obj_t get_object(const pdf_t *pdf, off_t obj_id)
{
    int i;
    off_t idx;
    obj_t obj;
    iter_t *itr;
    const xref_t *xref;
  
    /* Find the proper xref table */
    for (i=0; i<pdf->n_xrefs; ++i)
    {
        xref = pdf->xrefs[i];
        if (obj_id >= xref->first_entry_id && 
            obj_id < xref->first_entry_id + xref->n_entries)
          break;
    }

    /* Create an object between "<<" and ">>" */
    idx = obj_id - xref->first_entry_id;
    itr = new_iter(pdf, xref->entries[idx].offset);
    seek_next(itr, ' '); /* Skip obj number     */
    seek_next(itr, ' '); /* Skip obj generation */
    iter_next(itr);
    ERR(strncmp("obj ", ITR_VAL_STR(itr), strlen("obj")), !=0,
                "Could not locate object");
    seek_string(itr, "<<");
    obj.begin = ITR_POS(itr);
    seek_string(itr, ">>");
    obj.end = ITR_POS(itr);
    obj.id = obj_id;
    destroy_iter(itr);
    return obj;
}


/* Locate string in object.  If it cannot be found 'false' is returned else, the
 * iterator is updated an 'true' is returned.
 */
static _Bool find_in_object(iter_t *itr, obj_t obj, const char *match)
{
    const char *en;
    off_t orig = ITR_POS(itr);

    iter_set(itr, obj.begin);
    if ((en = strstr(ITR_ADDR(itr), match)) &&
        ((en - itr->pdf->data) <= obj.end))
      return true;

    iter_set(itr, orig);
    return false;
}


static void seek_next_nonwhitespace(iter_t *itr)
{
    while (!isspace(ITR_VAL(itr)) && ITR_IN_BOUNDS_V(itr, 1))
      iter_next(itr);
    while (isspace(ITR_VAL(itr)) && ITR_IN_BOUNDS_V(itr, 1))
      iter_next(itr);
}


static void add_kid(pdf_t *pdf, obj_t kid)
{
    kid_t *tmp, *new_kid;
    static int pg_num_pool;
    iter_t *itr = new_iter(pdf, -1);

    if (!find_in_object(itr, kid, "/Page"))
    {
        destroy_iter(itr);
        return;
    }

    new_kid = malloc(sizeof(kid_t));
    new_kid->pg_num = ++pg_num_pool;
    new_kid->id = kid.id;
    tmp = pdf->kids;
    pdf->kids = new_kid;
    new_kid->next = tmp;
    destroy_iter(itr);
}


/* This should be a parent with /Count and /Kids entries */
static void pages_from_parent(pdf_t *pdf, obj_t obj)
{
    off_t next_id;
    iter_t *itr = new_iter(pdf, -1);
    
    /* Get count */
    if (!find_in_object(itr, obj, "/Count"))
    {
        add_kid(pdf, obj);
        return;
    }

    /* Get the child pages */
    seek_next_nonwhitespace(itr);
    if (!find_in_object(itr, obj, "/Kids"))
    {
        add_kid(pdf, obj);
        return;
    }

    /* Must be a parent if we get here */
    seek_next(itr, '[');
    while (ITR_VAL(itr) != ']')
    {
        /* Get decendents */
        iter_next(itr);
        next_id = ITR_VAL_INT(itr);
        seek_next_nonwhitespace(itr); /* Skip version */
        seek_next_nonwhitespace(itr); /* Skip ref     */
        iter_next(itr);
        pages_from_parent(pdf, get_object(pdf, next_id));
    }

    destroy_iter(itr);
}


static void get_page_tree(pdf_t *pdf)
{
    obj_t obj;
    iter_t *itr = new_iter(pdf, -1);

    /* Get the root object (might be /Pages or /Linearized) */
    obj = get_object(pdf, pdf->xrefs[0]->root_obj);
    ERR(find_in_object(itr, obj, "/Pages"), ==false,
        "Could not locate /Pages tree");
    seek_next_nonwhitespace(itr);
    pages_from_parent(pdf, get_object(pdf, ITR_VAL_INT(itr)));
    destroy_iter(itr);
}


#if 0
static void print_page_tree(const pdf_t *pdf)
{
    const kid_t *k;
    for (k=pdf->kids; k; k=k->next)
      D("Page %d: %ld", k->pg_num, k->id);
}
#endif


static void get_xref(pdf_t *pdf, iter_t *itr)
{
    off_t i, first_obj, n_entries, trailer;
    xref_t *xref;

    seek_next_line(itr);
    first_obj = ITR_VAL_INT(itr);
    seek_next(itr, ' ');
    n_entries = ITR_VAL_INT(itr);
    D("xref starts at object %lu and contains %lu entries",
      first_obj, n_entries);

    /* Create a blank xref */
    if (!pdf->xrefs)
      pdf->xrefs = malloc(sizeof(xref_t *));
    xref = pdf->xrefs[pdf->n_xrefs] = malloc(sizeof(xref_t));
    ++pdf->n_xrefs;

    /* Add the entries */
    xref->first_entry_id = first_obj;
    xref->entries = malloc(sizeof(xref_entry_t) * n_entries);
    xref->n_entries = n_entries;
    for (i=0; i<n_entries; ++i)
    {
        /* Object offset */
        seek_next_line(itr);
        xref->entries[i].offset = ITR_VAL_INT(itr);
        
        /* Object generation */ 
        seek_next(itr, ' ');
        iter_next(itr);
        xref->entries[i].generation = ITR_VAL_INT(itr);
       
        /* Is free 'f' or in use 'n' */ 
        seek_next(itr, ' ');
        iter_next(itr);
        xref->entries[i].is_free = ITR_VAL(itr) == 'f';
    }

    /* Get trailer */
    seek_next_line(itr);
    ERR(strncmp("trailer", ITR_VAL_STR(itr), strlen("trailer")), !=0,
        "Could not locate trailer");
    
    trailer = ITR_POS(itr);

    /* Find /Root */
    if (seek_string(itr, "/Root") == NOT_FOUND)
      return;
    seek_next_nonwhitespace(itr);
    xref->root_obj = ITR_VAL_INT(itr);
    D("Document root located at %ld", xref->root_obj);

    /* Find /Prev */
    iter_set(itr, trailer);
    if (seek_string(itr, "/Prev") != -1)
    {
        seek_next_nonwhitespace(itr);
        iter_set(itr, ITR_VAL_INT(itr));
        get_xref(pdf, itr);
    }
}


static void get_xrefs(pdf_t *pdf)
{
    off_t xref;
    iter_t *itr;
    
    /* Skip end of lines at the end of the file */
    itr = new_iter(pdf, -1);
    find_prev(itr, '%');
    find_prev(itr, '%');
    seek_previous_line(itr); /* Get xref offset */
    xref = ITR_VAL_INT(itr);
    D("Initial xref table located at offset %lu", xref);

    /* Get xref */
    iter_set(itr, xref);
    get_xref(pdf, itr);
    destroy_iter(itr);
}


/* Loads cross reference tables and page tree */
static void load_pdf_structure(pdf_t *pdf)
{
    get_version(pdf);
    get_xrefs(pdf);
    get_page_tree(pdf);
}


static decode_exit_e decode_ps(
    unsigned char *data,
    off_t          length,
    decode_t      *decode)
{
    decode_exit_e de;
    unsigned char c;
    off_t i=0, stack_idx=0;
    double Tc=0.0, val_stack[2] = {0.0, 0.0};
    static const int X=0, Y=1;
    size_t bufidx = 0;
    const size_t max_buf = decode->buffer_length;
    char *buf = decode->buffer;

#ifdef DEBUG_PS
    for (i=0; i<length; ++i)
      putc(data[i], stdout);
#endif

    i = 0;
    while (i < length)
    {
        /* If we have filled the buffer... callback */
        if (bufidx > max_buf)
        {
            decode->buffer_used = bufidx;
            if ((de = decode->callback(decode)) != DECODE_CONTINUE)
              return DECODE_DONE;
            bufidx = decode->buffer_used;
        }

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
    iter_t *itr = new_iter(decode->pdf, -1);

    for (k=decode->pdf->kids; k; k=k->next)
      if (k->pg_num == decode->pg_num)
        break;

    if (!k)
      return;

    /* Get contents */
    obj = get_object(decode->pdf, k->id);
    ERR(find_in_object(itr, obj, "/Contents"), ==false,
        "Could not locate page contents");
    seek_next_nonwhitespace(itr);
    obj = get_object(decode->pdf, ITR_VAL_INT(itr));

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

    if (match)
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
    decode->buffer_used = 0;
    return DECODE_CONTINUE;
}


static void debug_page(const pdf_t *pdf, int pg_num)
{
    decode_t decode;
    char buf[2048] = {0};

    decode.pdf = pdf;
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
    load_pdf_structure(&pdf);

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
