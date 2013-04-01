#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
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


/* Data type: Contains a pointer to the raw pdf data */
typedef struct {
    const char   *data;
    size_t        len;
    int           ver_major, ver_minor;
    int           n_xrefs;
    xref_t      **xrefs;
}pdf_t;


/* Iterator type: Index into data */
typedef struct {off_t idx; const pdf_t *pdf;} iter_t;
#define ITR_VAL(_itr)       _itr->pdf->data[_itr->idx]
#define ITR_VAL_INT(_itr)   atoll(_itr->pdf->data + _itr->idx)
#define ITR_VAL_STR(_itr)   (char *)(_itr->pdf->data + _itr->idx)
#define ITR_POS(_itr)       _itr->idx
#define ITR_ADDR(_itr)      (_itr->pdf->data + _itr->idx)
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


typedef struct {off_t begin; off_t end;} obj_t;
static obj_t get_object(const pdf_t *pdf, off_t obj_id)
{
    int i;
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
    itr = new_iter(pdf, xref->entries[obj_id].offset);
    seek_next(itr, ' '); /* Skip obj number     */
    seek_next(itr, ' '); /* Skip obj generation */
    iter_next(itr);
    ERR(strncmp("obj ", ITR_VAL_STR(itr), strlen("obj")), !=0,
                "Could not locate object");
    seek_string(itr, "<<");
    obj.begin = ITR_POS(itr);
    seek_string(itr, ">>");
    obj.end = ITR_POS(itr);
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


static void get_xref(pdf_t *pdf, iter_t *itr)
{
    off_t i, first_obj, n_entries, trailer;
    obj_t obj;
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
    seek_next(itr, ' ');
    xref->root_obj = ITR_VAL_INT(itr);
    D("Document root located at %ld", xref->root_obj);

    /* Find /Prev */
    iter_set(itr, trailer);
    if (seek_string(itr, "/Prev") != -1)
    {
        seek_next(itr, ' ');
        iter_set(itr, ITR_VAL_INT(itr));
        get_xref(pdf, itr);
    }

    /* Get the root object (might be /Pages or /Linearized) */
    obj = get_object(pdf, xref->root_obj);
    if (find_in_object(itr, obj, "/Linearized"))
    {
        //TODO
    }
}


static void get_initial_xref(pdf_t *pdf)
{
    off_t xref;
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
    iter_set(itr, xref);
    get_xref(pdf, itr);

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
   
    /* New pdf */ 
    memset(&pdf, 0, sizeof(pdf_t));

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
