#ifndef __PDF_H_INCLUDE
#include <stdio.h>


#define TAG      "pdf"
#define PDF_ERR -1
#define PDF_OK   0


#ifdef DEBUG_PDF
#define D(...) \
    do {fprintf(stderr,TAG"[debug]" __VA_ARGS__); putc('\n',stderr);} while(0)
#else
#define D(...)
#endif



/* Use this for all offsets */
#ifndef __off_t_defined
typedef unsigned long long off_t;
#endif


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
#define ITR_IN_BOUNDS(_itr) (_itr->idx < _itr->pdf->len)
#define ITR_IN_BOUNDS_V(_itr, _val) \
    ((_itr->idx+_val) < _itr->pdf->len)


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


/* Allocate or destroy a PDF instance (this does loads the pdf) */
extern pdf_t *pdf_new(const char *filename);
extern void pdf_destroy(pdf_t *pdf);


/* Load the pdf data.  
 * Returns PDF_OK success or error otherwise.
 */
extern int pdf_load_data(pdf_t *pdf);


/* Given an object number within the pdf, get the obj object associated to the
 * beginging and end indicies for the object within the pdf.
 * Result is placed in 'obj'
 * Returns 'true' on success, 'false' otherwise
 */
extern _Bool pdf_get_object(const pdf_t *pdf, off_t object_number, obj_t *obj);


/* Given a decode object (which contains a pdf and a page number to decode).
 * The callback in the decode object is called, possibly multiple times during
 * decoding, with a buffer of decoded page data (ascii).
 * PDF_OK is returned on success, PDF_ERR is returned otherwise.
 */
extern int pdf_decode_page(decode_t *decode);


/* Create or destroy an iterator (for parsing a pdf)
 * offset: Byte offset into the pdf to start the iterator at.
 */
extern iter_t *iter_new(const pdf_t *pdf);
extern iter_t *iter_new_offset(const pdf_t *pdf, off_t offset);
extern void iter_destroy(iter_t *iter);


/* Iterator manipulation routines */
extern void iter_set(iter_t *itr, off_t offset); /* Byte offset into the pdf */
extern void iter_prev(iter_t *itr);              /* Previous character       */
extern void iter_next(iter_t *itr);              /* Next character           */

/* Locate a string in a pdf starting at 'itr'
 * Returns 'true' if the string is found, 'false otherwise.
 */
extern _Bool seek_string(iter_t *itr, const char *search);


/* Seek iterator to the next or previous instance of 'search' */
extern void seek_next(iter_t *itr, char search);
extern void seek_prev(iter_t *itr, char search);


/* Starting at 'itr' seek past all non-whitespace until a whitespace chacacter
 * is reached.  The first non-whitespace following the latter is where itr will
 * be located when this routine completes.
 * Example 'itr' is pointing to the 'a' in 'bar' in the following string:
 *  "Foo bar baz"
 * Calling this funtcion will advance 'itr' to point to the 'b' in baz.
 */
extern void seek_next_nonwhitespace(iter_t *itr);


/* If the character at 'itr' is whitespace, itr will be advanced past it, else
 * nothing happens.
 */
extern void skip_whitespace(iter_t *itr);


/* Find the previous or next line in a pdf starting at iter.
 * 'iter' is updated to begin at the start of the line.
 */
extern void seek_previous_line(iter_t *itr);
extern void seek_next_line(iter_t *itr);


/* Locate a string in a pdf obj
 * Returns 'true' if the string is found, 'false' otherwise.
 * If the strings is found, iter is set to point to the first character of the
 * match.
 */
extern _Bool find_in_object(iter_t *itr, obj_t obj, const char *search);


#endif /* __PDF_H_INCLUDE */
