#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <zlib.h>
#include "pdf.h"


/* If the value idx is greater than the buffer size,
 * issue a callback to the decode listner.
 * 
 * 'buf_idx' -- Should have the current buffer size, it will be updated if the
 *              callback is issued.
 */
static decode_exit_e cb_if_full(decode_t *decode, size_t *buf_idx)
{
    if (*buf_idx >= decode->buffer_length)
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
    double Tc=0.0, val_stack[6]={0.0}, Tm[6]={0.0};
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
                if (cb_if_full(decode, &bufidx) == DECODE_DONE)
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
            else if (c == 'm')
              memcpy(Tm, val_stack, sizeof(Tm));
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
    decode_t *decode,
    iter_t   *itr,
    size_t    length)
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


typedef struct _decoder_t
{
    const char *name;
    decode_exit_e (*do_decode)(decode_t *decode, iter_t *itr, size_t length);
} decoder_t;

static decoder_t decoders[] = 
{
    {"FlateDecode", decode_flate},
};
static const int n_decoders = 1;


/* Given a page object, figure out how it is decoded and call the proper decode
 * routine on it.
 */
static int find_and_decode(obj_t obj, decode_t *decode)
{
    int i;
    size_t pg_length;
    char name[32] = {0};
    iter_t *itr = iter_new(decode->pdf);
    
    /* Get the pages data */
    if (!find_in_object(itr, obj, "/Length"))
    {
        iter_destroy(itr);
        return PDF_ERR; /* Could not find length of the pages data */
    }
    seek_next_nonwhitespace(itr);
    pg_length = ITR_VAL_INT(itr);
    D("Decoding page %d (%lu bytes)", decode->pg_num, pg_length);

    /* Locate the Filter type (e.g. FlateDecode) */
    if (!find_in_object(itr, obj, "/Filter"))
    {
        iter_destroy(itr);
        return PDF_ERR;
    }
    seek_next(itr, '/');

    /* Get the Filte name */
    i = 0;
    iter_next(itr);
    while (i<sizeof(name)-1 && isalnum(ITR_VAL(itr)))
    {
        name[i++] = ITR_VAL(itr);
        iter_next(itr);
    }

    /* Get the start of the stream */
    seek_string(itr, "stream");
    seek_next(itr, '\n');
    iter_next(itr);

    /* Look through all decoders until we find the corresponding one */
    for (i=0; i<n_decoders; ++i)
      if (strncmp(decoders[i].name, name, strlen(name)) == 0)
      {
          if (decoders[i].do_decode(decode, itr, pg_length) == DECODE_DONE)
          {
              iter_destroy(itr);
              return PDF_OK;
          }
      }

    iter_destroy(itr);
    return PDF_ERR; /* Could not locate decoder */
}


int pdf_decode_page(decode_t *decode)
{
    const kid_t *k;
    obj_t obj;
    iter_t *itr = iter_new(decode->pdf);

    for (k=decode->pdf->kids; k; k=k->next)
      if (k->pg_num == decode->pg_num)
        break;

    if (!k)
      return PDF_ERR; /* Page not found */

    /* Get contents */
    if (!pdf_get_object(decode->pdf, k->id, &obj))
      return PDF_ERR; /* Could not locate page object */
    if (!find_in_object(itr, obj, "/Contents"))
      return PDF_ERR; /* Could not locate page contents */
    seek_next_nonwhitespace(itr);
    if (!pdf_get_object(decode->pdf, ITR_VAL_INT(itr), &obj))
      return PDF_ERR; /* Could not locate page */
    
    /* Decode the data */
    return find_and_decode(obj, decode);
}
