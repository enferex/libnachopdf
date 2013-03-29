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


static void usage(const char *execname)
{
    printf("Usage: %s <file> <-e regexp>\n", execname);
    exit(EXIT_SUCCESS);
}

typedef struct {const char *data; size_t len;} data_t;
static void get_version(const data_t *d, int *major, int *minor)
{
    ERR(sscanf(d->data, "%%PDF-%d.%d", major, minor), !=2,
        "Bad version string");
}


static void seek_previous_line(FILE *fp)
{
    char c;
    
    while ((c=fgetc(fp)) && (c != '\n' && c != '\r'))
      fseek(fp, -2, SEEK_CUR);
    fseek(fp, -1, SEEK_CUR);

    while ((c=fgetc(fp)) && (c == '\n' || c == '\r'))
      fseek(fp, -2, SEEK_CUR);
    ungetc(c, fp);

    while ((c=fgetc(fp)) && (c != '\n' && c != '\r'))
      fseek(fp, -2, SEEK_CUR);
    fseek(fp, 0, SEEK_CUR);
}


static void find_string_reverse(FILE *fp, const char *match)
{
    char *st, buf[1024];

    while (ftell(fp) != 0)
    {
        seek_previous_line(fp);
        fgets(buf, sizeof(buf), fp);
        if ((st = strstr(buf, match)))
          break;
    }
}


static const char *find_reverse(const char *data, char c)
{
    while (*data != c && offset > 0)
    {
        --data;
        --offset;
    }
    return data;
}


static iter_t *new_iter(data_t *d, 


static void get_trailer(const data_t *d)
{
    const char *c;

    /* Skip end of lines at the end of the file */
    iter = new_iter(data, -1);
    iter = find_reverse(iter,d->data + d->len, '%');
    iter = find_reverse(c, '%');

//    ERR(fread(buf, 1, sizeof(buf), fp), ==0, "Reading EOF");
//    ERR(strncmp("%%EOF", buf, 5), !=0, "Could not locate EOF");
//
//    /* Get xref offset */
//    fseek(fp, -5, SEEK_CUR);
//    seek_previous_line(fp);
//    ERR(fgets(buf, sizeof(buf), fp), ==0, "Locating xref offset");
//    fseek(fp, -strlen(buf)-2, SEEK_CUR);
//    xset = atoi(buf);
//    D("Initial xref table located at offset %d", xset);
//
//    /* Get startxref */
//    ERR(fgets(buf, sizeof(buf), fp), ==0, "Locating startxref");
//    find_string_reverse(fp, "trailer");
}


int main(int argc, char **argv)
{
    int i, fd, maj, min;
    data_t d;
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
    ERR((d.data=mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0)),==NULL,
        "Mapping file into memory");
    d.len = stat.st_size;
                   
    get_version(&d, &maj, &min);
    D("PDF Version: %d.%d", maj, min);

    //get_trailer(fp);

    close(fd);
    return 0;
}
