#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>


#define TAG "pdfgrep"


#define _P(_tag, ...) \
    do {printf("["TAG"]"_tag" "__VA_ARGS__); putc('\n', stdout);} while(0)


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


/* Entry for a cross reference table */
typedef struct {size_t offset; size_t state;} xref_entry_t;


/* Cross reference table */
typedef struct {
    int           n_entries;
    xref_entry_t *entries;
    size_t        root_idx;
} xref_t;


/* Data type: Contains a pointer to the raw pdf data */
typedef struct {
    const char   *data;
    size_t        len;
    int           ver_major, ver_minor;
    int           n_xrefs;
    xref_t      **xrefs;
}pdf_t;


/* Iterator type: Index into data */
typedef struct {ssize_t idx; const pdf_t *pdf;} iter_t;
#define ITR_VAL(_itr)       _itr->pdf->data[_itr->idx]
#define ITR_VAL_INT(_itr)   atoll(_itr->pdf->data + _itr->idx)
#define ITR_VAL_STR(_itr)   (char *)(_itr->pdf->data + _itr->idx)
#define ITR_IN_BOUNDS(_itr) ((_itr->idx>-1 && (_itr->idx < _itr->pdf->len)))


static void usage(const char *execname)
{
    printf("Usage: %s <file> <-e regexp>\n", execname);
    exit(EXIT_SUCCESS);
}


static void get_version(pdf_t *pdf)
{
    ERR(sscanf(pdf->data,"%%PDF-%d.%d",&pdf->ver_major,&pdf->ver_minor), !=2,
        "Bad version string");
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
static void find_next(iter_t *itr, char match)
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
    find_next(itr, '\n');
    iter_next(itr);
}


static void find_string_reverse(FILE *fp, const char *match)
{
}



static iter_t *new_iter(const pdf_t *pdf, ssize_t start_offset)
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


static void set_iter(iter_t *itr, size_t offset)
{
    itr->idx = offset;
    if (!ITR_IN_BOUNDS(itr))
      abort();
}


static void get_xref(iter_t *itr)
{
    size_t i, first_obj, n_entries, offset, state;

    seek_next_line(itr);
    first_obj = ITR_VAL_INT(itr);
    find_next(itr, ' ');
    n_entries = ITR_VAL_INT(itr);
    D("xref starts at object %lu and contains %lu entries",
      first_obj, n_entries);

    for (i=0; i<n_entries; ++i)
    {
        seek_next_line(itr);
        offset = ITR_VAL_INT(itr);
        find_next(itr, ' ');
        state = ITR_VAL_INT(itr);
        D("    Offset %lu State %lu", offset, state);
    }

    /* Get trailer */
    seek_next_line(itr);
    ERR(strncmp("trailer", ITR_VAL_STR(itr), strlen("trailer")), !=0,
        "Could not locate trailer");
}


static void get_initial_xref(const pdf_t *pdf)
{
    size_t xref;
    iter_t *itr;

    /* Skip end of lines at the end of the file */
    itr = new_iter(pdf, -1);
    find_prev(itr, '%');
    find_prev(itr, '%');

    /* Get xref offset */
    seek_previous_line(itr);
    xref = ITR_VAL_INT(itr);
    D("Initial xref table located at offset %lu", xref);

    /* Get xref */
    set_iter(itr, xref);
    get_xref(itr);

    destroy_iter(itr);
}


int main(int argc, char **argv)
{
    int i, fd;
    pdf_t pdf;
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
        else if (argv[i][0] != '-')
          fname = argv[i];
        else
          usage(argv[0]);
    }

    if (!fname || !expr)
      usage(argv[0]);

    D("File: %s", fname);
    D("Expr: %s", expr);

    /* Open and map the file into memory */
    ERR((fd = open(fname, O_RDONLY)), ==-1, "Opening file '%s'", fname);
    ERR(fstat(fd, &stat), ==-1, "Obtaining file size");
    ERR((pdf.data=mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0)),==NULL,
        "Mapping file into memory");
    pdf.len = stat.st_size;

    /* Get pdf version info */
    get_version(&pdf);
    D("PDF Version: %d.%d", pdf.ver_major, pdf.ver_minor);

    /* Get the initial cross reference table */
    get_initial_xref(&pdf);

    /* Clean up */
    close(fd);

    return 0;
}
