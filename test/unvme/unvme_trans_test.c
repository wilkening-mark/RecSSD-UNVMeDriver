/**
 * @file
 * @brief UNVMe translation bandwidth test.
 *
 * Modified by Mark Wilkening
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <time.h>
#include <err.h>
#include <sys/mman.h>

#include "unvme.h"
#include "unvme_log.h"
#include "rdtsc.h"

/// Print fatal error and exit
#define FATAL(fmt, arg...)  do { ERROR(fmt, ##arg); abort(); } while (0)

/// macro to print an io related error message
#define IOERROR(s, p)   errx(1, "ERROR: " s " lba=%#lx", (p)->lba)

/// page structure
typedef struct {
    void*           buf;        ///< IO buffer
    u64             lba;        ///< lba
    unvme_iod_t     iod;        ///< returned IO descriptor
    u64             tsc;        ///< tsc time
} bw_page_t;

typedef struct {
  u32 attribute_size;
  u32 number_of_columns;
  u32 number_of_rows;

  // Formatted as a list of requested column groups.
  // Each column group is a list of columns which will
  // be returned in a PAX style layout, prefaced with
  // the number of columns in that group.
  //
  // Ex.
  // 2,1,3,3,2,17,18,END
  //
  // Would return columns 1 and 3 in PAX2, followed
  // by columns 2, 17, and 18 in PAX3.
  u32 column_group_list[1021];
} trans_config_t;

// Global variables
static const unvme_ns_t* ns;    ///< unvme namespace pointer
static int qcount = 8;          ///< queue count
static int qsize = 256;         ///< queue size
static u64 timeout = 0;         ///< tsc elapsed timeout
static sem_t sm_ready;          ///< semaphore to start thread
static sem_t sm_start;          ///< semaphore to start test
static pthread_t* ses;          ///< array of thread sessions
static int validate = 0;        ///< Run functional validation test

/**
 * Submit an io and record the submission latency time.
 */
static void io_submit(int q, int rw, bw_page_t* p)
{
    p->tsc = rdtsc();
    if (rw) {
        p->iod = unvme_awrite(ns, q, p->buf, p->lba, ns->nbpp);
        if (!p->iod) IOERROR("awrite", p);
    } else {
        p->iod = unvme_aread(ns, q, p->buf, p->lba, ns->nbpp);
        if (!p->iod) IOERROR("aread", p);
    }
}

/**
 * Reads/writes a region of data.
 */
static void rw_region(void *buf, u64 slba, u64 nlb,
                      int rw, int q)
{
  u64 lba = slba;
  int qdepth = qsize - 1;

  bw_page_t* pages = calloc(qdepth, sizeof(bw_page_t));
  void* buf_p = buf;
  bw_page_t* p = pages;
  int i;
  for (i = 0; i < qdepth; i++) {
    p->buf = buf_p;
    p->lba = lba;
    buf_p += ns->pagesize;
    lba += ns->nbpp;
    p++;
  }

  for (i = 0; i < qdepth; i++) io_submit(q, rw, pages + i);

  i = 0;
  int pending = qdepth;
  do {
    p = pages + i;
    if (p->iod) {
      if (unvme_apoll(p->iod, 0) == 0) {
        if (lba < slba + nlb) {
          p->buf = buf_p;
          p->lba = lba;
          buf_p += ns->pagesize;
          lba += ns->nbpp;
          io_submit(q, rw, p);
        } else {
          p->iod = 0;
          pending--;
        }
      } else if ((rdtsc_elapse(p->tsc)) > timeout) {
        IOERROR("apoll timeout", p);
      }
    }
    if (++i == qdepth) i = 0;
  } while (pending > 0);

  free(pages);
}

static void translate_region(void *scratchpad, void *buf,
                             trans_config_t config)
{
  struct attribute
  {
    char bytes[config.attribute_size];
  } *fromBase, *fromAtr, *toAtr;
  unsigned int groupidx, listidx, colidx;
  unsigned int row;

  fromBase = scratchpad;
  fromAtr = fromBase;
  toAtr = buf;

  for (groupidx = 0;
       config.column_group_list[groupidx] != 0xffffffff;
       groupidx += (1 + config.column_group_list[groupidx]))
  {
    for (row = 0;
         row < config.number_of_rows;
         row++)
    {
      for (listidx = 0;
           listidx < config.column_group_list[groupidx];
           listidx++)
      {
        colidx = groupidx + 1 + listidx;
        fromAtr = fromBase +
                  row * config.number_of_columns +
                  config.column_group_list[colidx];
        *toAtr = *fromAtr;
        toAtr++;
      }
    }
  }
}


/**
 * Reads a region of the table off disk
 * and pulls out the desired columns.
 */
static void* rw_thread_conventional(void* arg)
{
  u64 *argp = (u64*)arg;
  void *scratchpad = (void*)argp[5];
  trans_config_t *config = (trans_config_t*)argp[4];
  void *buf = (void*)argp[3];
  u64 slba = (long)argp[2];
  u64 nlb = (long)argp[1];
  int rw = ((long)argp[0] >> 16) & 0xffff;
  int q = (long)argp[0] & 0xffff;

  sem_post(&sm_ready);
  sem_wait(&sm_start);

  for(u64 lboff = 0;
      lboff < nlb;
      lboff += ns->blocksize)
  {
    rw_region(buf + lboff * ns->blocksize, slba + lboff, ns->blocksize, rw, q);
    translate_region(buf + lboff * ns->blocksize,
                     scratchpad, // just dump to dummy location
                     *config);
  }

  return 0;
}

static void* read_thread_ndp(void* arg)
{
  u64 *argp = (u64*)arg;
  u64 ncols = argp[5];
  trans_config_t *config = (trans_config_t*)argp[4];
  void *buf = (void*)argp[3];
  u64 slba = (long)argp[2];
  u64 nlb = (long)argp[1];
  int rw = ((long)argp[0] >> 16) & 0xffff;
  int q = (long)argp[0] & 0xffff;
  u64 nlbToRead = ns->blocksize / (config->number_of_columns / ncols);

  sem_post(&sm_ready);
  sem_wait(&sm_start);

  for(u64 chunkoff = 0;
      chunkoff < nlb / ns->blocksize;
      chunkoff++)
  {
    printf("Translating Region: buf %x, lba %d, q %d, nlbToRead %d\n",
        buf + chunkoff * ns->blocksize * nlbToRead,
        slba + chunkoff * ns->blocksize,
        q,
        nlbToRead);
    *(trans_config_t*)(buf + chunkoff * ns->blocksize * nlbToRead) = *config;
    int err = unvme_translate_region(ns, q,
        buf + chunkoff * ns->blocksize * nlbToRead,
        slba + chunkoff * ns->blocksize, nlbToRead);
    if (err) errx(1, "translate");
  }

  return 0;
}

/**
 * Run test to spawn one thread for each queue.
 */
void run_test(int ncols)
{
    trans_config_t config;
    config.attribute_size = 8;
    config.number_of_columns = ns->blocksize / config.attribute_size;
    config.number_of_rows = ns->blocksize;
    u64 tablesize = (u64)config.attribute_size *
                    (u64)config.number_of_columns *
                    (u64)config.number_of_rows *
                    (u64)qcount * 4;
    u64 slba = 5000;
    u64 rows = tablesize / (config.attribute_size * config.number_of_columns);
    u64 nlb = tablesize / (u64)ns->blocksize;
    int q, rw, chunksize, chunknlb;
    u64 ts, tsc;

    void *buf = unvme_alloc(ns, tablesize);
    volatile u64 *atr = buf;
    void *scratchpad = malloc(ns->blocksize * ns->blocksize);

    // Timing configuration
    u64 tsec = rdtsc_second();
    // For breakpoint debugging on board
    //timeout = UNVME_TIMEOUT * tsec;
    timeout = 1000 * tsec;

    // Write test table
    u64 tag;
    for (int row = 0; row < rows; row++)
    {
      for (int col = 0; col < config.number_of_columns; col++)
      {
        tag = (row * config.number_of_columns + col);
        (*(atr++)) = tag;
      }
    }
    rw_region(buf, slba, nlb, 1, 0);
    // Write fence before translation
    unvme_flush(ns, 0);

    // Just test 1st N columns in
    // column major
    u32 *col_list = config.column_group_list;
    for (int i = 0; i < ncols; i++)
    {
      *col_list++ = 1;
      *col_list++ = i;
    }
    *col_list = 0xffffffff;

    //-----------------------------------
    // Conventional
    // ----------------------------------
    sem_init(&sm_ready, 0, 0);
    sem_init(&sm_start, 0, 0);

    rw = 0;
    chunksize = tablesize / qcount;
    chunknlb = chunksize / ns->blocksize;
    for (q = 0; q < qcount; q++) {
        u64 arg[6] = {(rw << 16) + q,
                      chunknlb,
                      slba + q * chunknlb,
                      (u64)(buf + q * chunksize),
                      (u64)&config,
                      (u64)scratchpad};
        pthread_create(&ses[q], 0, rw_thread_conventional, (void*)arg);
        sem_wait(&sm_ready);
    }

    sleep(1);

    printf("Starting translation test for %d columns.\n",
           ncols);

    tsc = rdtsc();

    for (q = 0; q < qcount; q++) sem_post(&sm_start);
    for (q = 0; q < qcount; q++) pthread_join(ses[q], 0);

    ts = rdtsc_elapse(tsc);

    printf("Conventional Translation w/ NCols %d: %lf Bytes/second\n",
            ncols,
            ((double)tablesize * (double)tsec) / (double)ts);

    //-----------------------------------
    // NDP
    // ----------------------------------

    sem_init(&sm_ready, 0, 0);
    sem_init(&sm_start, 0, 0);

    rw = 0;
    //chunksize = tablesize / qcount;
    // size of returned chunk
    chunksize = (ncols * rows * config.attribute_size) / qcount;
    // nlb in chunk to translate
    chunknlb = (tablesize / qcount) / ns->blocksize;
    for (q = 0; q < qcount; q++) {
        u64 arg[6] = {(rw << 16) + q,
                      chunknlb,
                      slba + q * chunknlb,
                      (u64)(buf + q * chunksize),
                      (u64)&config,
                      (u64)ncols};
        pthread_create(&ses[q], 0, read_thread_ndp, (void*)arg);
        sem_wait(&sm_ready);
    }

    sleep(1);

    printf("Starting translation test for %d columns.\n",
           ncols);

    tsc = rdtsc();

    for (q = 0; q < qcount; q++) sem_post(&sm_start);
    for (q = 0; q < qcount; q++) pthread_join(ses[q], 0);

    ts = rdtsc_elapse(tsc);

    sleep(1);

    printf("NDP Translation w/ NCols %d: %lf Bytes/second\n",
            ncols,
            ((double)tablesize * (double)tsec) / (double)ts);

    // Print/reset on board stats
    unvme_flush(ns, 0);

    //-----------------------------------
    // Validation
    // ----------------------------------
    if (validate) {
      // Check results
      atr = buf;
      int err = 0;
      for(int col = 0; col < ncols; col++)
      {
        for (int row = 0; row < rows; row++)
        {
          tag = (row * config.number_of_columns + col);
          if ( *atr != tag )
          {
            printf("Validation test w/ NCols %d: Failed\n%d/%d -- %ld != %ld\n",
                   ncols,
                   row, col,
                   *atr, tag);
            err = 1;
            break;
          }
          atr++;
        }
        if (err) break;
      }
      if (!err)
        printf("Validation test w/ NCols %d: Succeded\n", ncols);
    }

    unvme_free(ns, buf);
    free(scratchpad);
    sem_destroy(&sm_ready);
    sem_destroy(&sm_start);
}

/**
 * Main program.
 */
int main(int argc, char* argv[])
{
    const char* usage = "Usage: %s [OPTION]... PCINAME\n\
           -v          Perform functional validation test\n\
           -q QCOUNT   number of queues/threads (default 8)\n\
           -d QDEPTH   queue depth (default 8)\n\
           PCINAME     PCI device name (as 01:00.0[/1] format)";

    char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    int opt;
    while ((opt = getopt(argc, argv, "v:q:d:")) != -1) {
        switch (opt) {
        case 'v':
            validate = 1;
            break;
        case 'q':
            qcount = strtol(optarg, 0, 0);
            break;
        case 'd':
            qsize = strtol(optarg, 0, 0);
            break;
        default:
            warnx(usage, prog);
            exit(1);
        }
    }
    if ((optind + 1) != argc) {
        warnx(usage, prog);
        exit(1);
    }
    char* pciname = argv[optind];

    printf("TRANSLATION BANDWIDTH TEST BEGIN\n");
    time_t tstart = time(0);
    if (!(ns = unvme_open(pciname))) exit(1);
    if (qcount <= 0 || qcount > ns->qcount) errx(1, "qcount limit %d", ns->qcount);
    if (qsize <= 1 || qsize > ns->qsize) errx(1, "qsize limit %d", ns->qsize);

    if (!qcount) qcount = ns->qcount;
    if (!qsize) qsize = ns->qsize;

    printf("%s qc=%d/%d qs=%d/%d bc=%#lx bs=%d mbio=%d\n",
            ns->device, qcount, ns->qcount, qsize, ns->qsize,
            ns->blockcount, ns->blocksize, ns->maxbpio);

    ses = calloc(qcount, sizeof(pthread_t));

    for (int ncols = 1;
         ncols < 512;
         ncols *= 2)
    {
      run_test(ncols);
    }

    free(ses);
    unvme_close(ns);

    printf("TRANSLATION BANDWIDTH TEST COMPLETE (%ld secs)\n", time(0) - tstart);
    return 0;
}

