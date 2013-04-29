/******************************************************************************
 * pdf.c
 *
 * libnachopdf - A basic PDF text extraction library
 *
 * Copyright (C) 2013, Matt Davis (enferex)
 *
 * This file is part of libnachopdf.
 * libnachopdf is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * libnachopdf is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libnachopdf.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "pdf.h"


void iter_prev(iter_t *itr)
{
    --itr->idx;
}


void iter_next(iter_t *itr)
{
    ++itr->idx;
}


/* Keep moving backwards until we hit 'match' */
void seek_next(iter_t *itr, char match)
{
    /* If we are already on the character, backup one */
    if (ITR_IN_BOUNDS(itr) && ITR_VAL(itr) == match)
      iter_next(itr);

    while (ITR_IN_BOUNDS(itr) && ITR_VAL(itr) != match)
      iter_next(itr);
}


/* Keep moving backwards until we hit 'match' */
void seek_prev(iter_t *itr, char match)
{
    /* If we are already on the character, backup one */
    if (ITR_IN_BOUNDS(itr) && ITR_VAL(itr) == match)
      iter_prev(itr);

    while (ITR_IN_BOUNDS(itr) && ITR_VAL(itr) != match)
      iter_prev(itr);
}


void seek_previous_line(iter_t *itr)
{
    seek_prev(itr, '\n'); /* Rewind to this line's start */
    seek_prev(itr, '\n'); /* Beginning of previous line  */
    iter_next(itr);
}


void seek_next_line(iter_t *itr)
{
    seek_next(itr, '\n');
    iter_next(itr);
}


static iter_t *_iter_new(const pdf_t *pdf, off_t start_offset)
{
    iter_t *itr = malloc(sizeof(iter_t));
    itr->idx = start_offset;
    itr->pdf = pdf;
    if (!ITR_IN_BOUNDS(itr))
      abort();
    return itr;
}


iter_t *iter_new(const pdf_t *pdf)
{
    return _iter_new(pdf, pdf->len - 1);
}


iter_t *iter_new_offset(const pdf_t *pdf, off_t offset)
{
    return _iter_new(pdf, offset);
}


void iter_destroy(iter_t *itr)
{
    free(itr);
}


void iter_set(iter_t *itr, off_t offset)
{
    itr->idx = offset;
    if (!ITR_IN_BOUNDS(itr))
      abort();
}


/* Returns true if found, false if not found */
_Bool seek_string(iter_t *itr, const char *search)
{
    const char *en, *st = ITR_ADDR(itr);
    if (!(en = strstr(st, search)))
      return false;
    iter_set(itr, ITR_POS(itr) + en - st);
    return true;
}


void skip_whitespace(iter_t *itr)
{
    while (isspace(ITR_VAL(itr)) && ITR_IN_BOUNDS_V(itr, 1))
      iter_next(itr);
}


/* Locate the first whitespace character and seek to the first character of the
 * whitespace.
 */
void seek_next_nonwhitespace(iter_t *itr)
{
    while (!isspace(ITR_VAL(itr)) && ITR_IN_BOUNDS_V(itr, 1))
      iter_next(itr);
    skip_whitespace(itr);
}


/* Creates a fresh object from "<<" to ">>" */
_Bool pdf_get_object(const pdf_t *pdf, off_t obj_id, obj_t *obj)
{
    int i;
    off_t idx;
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
    itr = iter_new_offset(pdf, xref->entries[idx].offset);
    seek_next(itr, ' '); /* Skip obj number     */
    seek_next(itr, ' '); /* Skip obj generation */
    iter_next(itr);

    if (strncmp("obj ", ITR_VAL_STR(itr), strlen("obj")) != 0)
      return false; /* Could not locate object */
    seek_string(itr, "<<");
    obj->begin = ITR_POS(itr);
    seek_string(itr, ">>");
    seek_string(itr, "endobj");
    obj->end = ITR_POS(itr);
    obj->id = obj_id;
    iter_destroy(itr);
    return true;
}


/* Locate string in object.  If it cannot be found 'false' is returned else, the
 * iterator is updated an 'true' is returned.
 */
_Bool find_in_object(iter_t *itr, obj_t obj, const char *search)
{
    const char *en;
    off_t orig = ITR_POS(itr);

    iter_set(itr, obj.begin);
    if ((en = strstr(ITR_ADDR(itr), search)) &&
        ((en - itr->pdf->data) <= obj.end))
    {
        seek_string(itr, search);
        return true;
    }

    iter_set(itr, orig);
    return false;
}



static void add_kid(pdf_t *pdf, obj_t kid)
{
    kid_t *new_kid;
    static int pg_num_pool;
    iter_t *itr = iter_new(pdf);
    static kid_t *last;

    if (!find_in_object(itr, kid, "/Page"))
    {
        iter_destroy(itr);
        return;
    }

    new_kid = calloc(1, sizeof(kid_t));
    new_kid->pg_num = ++pg_num_pool;
    new_kid->id = kid.id;

    if (last)
      last->next = new_kid;
    last = new_kid;

    if (!pdf->kids)
      pdf->kids = new_kid;

    iter_destroy(itr);
}


/* This should be a parent with /Count and /Kids entries */
static _Bool pages_from_parent(pdf_t *pdf, obj_t obj)
{
    off_t next_id;
    obj_t search;
    iter_t *itr = iter_new(pdf);
    
    /* Get count */
    if (!find_in_object(itr, obj, "/Count"))
    {
        add_kid(pdf, obj);
        return true;
    }

    /* Get the child pages */
    seek_next_nonwhitespace(itr);
    if (!find_in_object(itr, obj, "/Kids"))
    {
        add_kid(pdf, obj);
        return true;
    }

    /* Must be a parent if we get here */
    seek_next(itr, '[');
    while (ITR_VAL(itr) != ']')
    {
        /* Get decendents */
        iter_next(itr);
        skip_whitespace(itr);
        if (ITR_VAL(itr) == ']')
          break;
        next_id = ITR_VAL_INT(itr);
        seek_next_nonwhitespace(itr); /* Skip version */
        seek_next_nonwhitespace(itr); /* Skip ref     */
        iter_next(itr);
        if (!pdf_get_object(pdf, next_id, &search))
          return false;
        pages_from_parent(pdf, search);
    }

    iter_destroy(itr);
    return true;
}


/* Returns -1 on error (cannot find /Pages) */
static int get_page_tree(pdf_t *pdf)
{
    obj_t obj;
    iter_t *itr = iter_new(pdf);

    /* Get the root object (might be /Pages or /Linearized) */
    if (!pdf_get_object(pdf, pdf->xrefs[0]->root_obj, &obj))
      return PDF_ERR;

    if (!find_in_object(itr, obj, "/Pages"))
      return PDF_ERR;

    seek_next_nonwhitespace(itr);

    if (!pdf_get_object(pdf, ITR_VAL_INT(itr), &obj))
      return PDF_ERR;

    pages_from_parent(pdf, obj);
    iter_destroy(itr);
    return PDF_OK;
}


#if 0
static void print_page_tree(const pdf_t *pdf)
{
    const kid_t *k;
    for (k=pdf->kids; k; k=k->next)
      D("Page %d: %ld", k->pg_num, k->id);
}
#endif


static int get_xref(pdf_t *pdf, iter_t *itr)
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
    if (strncmp("trailer", ITR_VAL_STR(itr), strlen("trailer")) != 0)
      return PDF_ERR; /*  Could not locate trailer */
    
    trailer = ITR_POS(itr);

    /* Find /Root */
    if (!seek_string(itr, "/Root"))
      return PDF_ERR;
    seek_next_nonwhitespace(itr);
    xref->root_obj = ITR_VAL_INT(itr);
    D("Document root located at %lu", xref->root_obj);

    /* Find /Prev */
    iter_set(itr, trailer);
    if (seek_string(itr, "/Prev"))
    {
        seek_next_nonwhitespace(itr);
        iter_set(itr, ITR_VAL_INT(itr));
        get_xref(pdf, itr);
    }

    return PDF_OK;
}


static int get_xrefs(pdf_t *pdf)
{
    off_t xref;
    iter_t *itr;
    
    /* Skip end of lines at the end of the file */
    itr = iter_new(pdf);
    seek_prev(itr, '%');
    seek_prev(itr, '%');
    seek_previous_line(itr); /* Get xref offset */
    xref = ITR_VAL_INT(itr);
    D("Initial xref table located at offset %lu", xref);

    /* Get xref */
    iter_set(itr, xref);
    get_xref(pdf, itr);
    iter_destroy(itr);

    return PDF_OK;
}


static int get_version(pdf_t *pdf)
{
    if (sscanf(pdf->data,"%%PDF-%d.%d",&pdf->ver_major,&pdf->ver_minor) != 2)
      return PDF_ERR; /* "Bad version string" */
    D("PDF Version: %d.%d", pdf->ver_major, pdf->ver_minor);
    return PDF_OK;
}


/* Loads cross reference tables and page tree */
int pdf_load_data(pdf_t *pdf)
{
    int err;
    
    if ((err = get_version(pdf)) != PDF_OK)
      return err;
    if ((err = get_xrefs(pdf)) != PDF_OK)
      return err;
    if ((err = get_page_tree(pdf)) != PDF_OK)
      return err;
    return PDF_OK;
}


pdf_t *pdf_new(const char *fname)
{
    int fd;
    struct stat stat;
    pdf_t *pdf;
   
    pdf = calloc(1, sizeof(pdf_t));
    pdf->fname = fname;

    /* Open and map the file into memory */
    ERR((fd = open(fname, O_RDONLY)), ==-1, "Opening file '%s'", fname);
    ERR(fstat(fd, &stat), ==-1, "Obtaining file size");
    ERR((pdf->data=mmap(
         NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0)),==NULL,
        "Mapping file into memory");
    pdf->len = stat.st_size;

    /* Get the initial cross reference table */
    ERR(pdf_load_data(pdf), != PDF_OK, "Could not load pdf");
    return pdf;
}


void pdf_destroy(pdf_t *pdf)
{
    int i;
    for (i=0; i<pdf->n_xrefs; ++i)
    {
        free(pdf->xrefs[i]->entries);
        free(pdf->xrefs[i]);
    }
    munmap((void *)pdf->data, pdf->len);
    free(pdf);
}
