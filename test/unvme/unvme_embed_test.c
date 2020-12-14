/**
 * @file
 * @brief UNVMe RL embedding table lookup bandwidth test.
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

static int stride = 1;

/// page structure
typedef struct {
    void*           buf;        ///< IO buffer
    u64             lba;        ///< lba
    unvme_iod_t     iod;        ///< returned IO descriptor
    u64             tsc;        ///< tsc time
} bw_page_t;

typedef struct {
  u32 attribute_size;
  u32 embedding_length;
  u32 result_embeddings;
  u32 input_embeddings;

  u32 embedding_id_list[];
} embed_config_t;

// Global variables
static const unvme_ns_t* ns;    ///< unvme namespace pointer
static int qcount = 8;          ///< queue count
static int qsize = 256;         ///< queue size
static int embed_length = 64;
static int table_length = 700000;
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

/* DRAM based implementation of lookup. */
static void embedding_lookup(void *table, void *results,
                             embed_config_t *config)
{
  struct attribute
  {
    char bytes[config->attribute_size];
  } *fromBase, *fromAtr, *toBase, *toEmbed, *toAtr;
  unsigned int resultidx, embedlistidx, embedidx, atridx;

  fromBase = table;
  fromAtr = fromBase;
  toBase = results;
  toEmbed = toBase;
  toAtr = toBase;

  for (embedlistidx = 0;
       embedlistidx < 2*config->input_embeddings;
       embedlistidx+=2)
  {
    resultidx = config->embedding_id_list[embedlistidx];
    toEmbed = toBase + resultidx*config->embedding_length;
    toAtr = toEmbed;

    embedidx = config->embedding_id_list[embedlistidx+1];
    fromAtr = fromBase + (embedidx * config->embedding_length);
    for (atridx = 0;
         atridx < config->embedding_length;
         atridx++)
    {
      // Assume floats for now
      *((float*)toAtr) += *((float*)fromAtr);
      toAtr++;
      fromAtr++;
    }
  }
}

/* FileIO SSD based implementation of lookup. */
static void embedding_lookup_file(FILE *table, void *results,
                             embed_config_t *config)
{
  struct attribute
  {
    char bytes[config->attribute_size];
  } *toBase, *toEmbed, *toAtr, *fromAtr, fromEmbed[config->embedding_length];
  unsigned int resultidx, embedlistidx, embedidx, atridx;

  toBase = results;
  toEmbed = toBase;
  toAtr = toBase;

  for (embedlistidx = 0;
       embedlistidx < 2*config->input_embeddings;
       embedlistidx+=2)
  {
    resultidx = config->embedding_id_list[embedlistidx];
    toEmbed = toBase + resultidx*config->embedding_length;
    toAtr = toEmbed;

    embedidx = config->embedding_id_list[embedlistidx+1];
    fseek(table,
          config->attribute_size * config->embedding_length * embedidx,
          SEEK_SET);
    int err = fread(fromEmbed,
              config->attribute_size * config->embedding_length,
              1, table);
    if (!err) errx(1, "reading fileiotest");
    fromAtr = fromEmbed;
    for (atridx = 0;
         atridx < config->embedding_length;
         atridx++)
    {
      // Assume floats for now
      *((float*)toAtr) += *((float*)fromAtr);
      toAtr++;
      fromAtr++;
    }
  }
}

/* Unvme I/O SSD based implementation of lookup. */
static void embedding_lookup_io(unsigned int qid,
        unsigned int slba, void *results,  embed_config_t *config)
{
  struct attribute
  {
    char bytes[config->attribute_size];
  } *toBase, *toEmbed, *toAtr, *fromAtr, *fromEmbed, *fromPage;
  unsigned int resultidx, embedlistidx, embedidx, atridx;
  fromPage = unvme_alloc(ns, 4096);

  toBase = results;
  toEmbed = toBase;
  toAtr = toBase;

  for (embedlistidx = 0;
       embedlistidx < 2*config->input_embeddings;
       embedlistidx+=2)
  {
    resultidx = config->embedding_id_list[embedlistidx];
    toEmbed = toBase + resultidx*config->embedding_length;
    toAtr = toEmbed;

    embedidx = config->embedding_id_list[embedlistidx+1];
    unvme_read(ns, qid, fromPage,
        slba + ((config->attribute_size * config->embedding_length * embedidx) / 4096),
        1);
    fromEmbed = fromPage +
        ((embedidx % (4096 / (config->attribute_size * config->embedding_length))) * config->embedding_length);
    fromAtr = fromEmbed;
    for (atridx = 0;
         atridx < config->embedding_length;
         atridx++)
    {
      // Assume floats for now
      *((float*)toAtr) += *((float*)fromAtr);
      toAtr++;
      fromAtr++;
    }
  }
  unvme_free(ns, fromPage);
}


/**
 * Looks up embeddings from DRAM.
 */
static void* lookup_thread_conventional(void* arg)
{
  u64 *argp = (u64*)arg;
  void *results = (void*)argp[5];
  void *table = (void*)argp[4];
  long numbatches = (long)argp[3];
  long batchsize = (long)argp[2];
  long embedperrequest = (long)argp[1];
  //long q = (long)argp[0];

  void *result_ptr = results;
  embed_config_t *config = malloc(4*2*batchsize*embedperrequest + 16);
  config->attribute_size = 4;
  config->embedding_length = embed_length;
  config->result_embeddings = batchsize;
  config->input_embeddings = batchsize*embedperrequest;
  int list_idx;
  int id = 65;
  for (list_idx = 0;
       list_idx < 2*config->input_embeddings;
       list_idx+=2)
  {
    config->embedding_id_list[list_idx] = (list_idx/2)/embedperrequest;
    config->embedding_id_list[list_idx+1] = id;
    id += stride;
  }

  //sem_post(&sm_ready);
  //sem_wait(&sm_start);

  int batch_idx;
  for (batch_idx = 0;
       batch_idx < numbatches;
       batch_idx++)
  {
    embedding_lookup(table, result_ptr, config);
    result_ptr += 4 * embed_length * batchsize;
  }
  return 0;
}

/**
 * Looks up embeddings from SSD using FileIO.
 */
static void* lookup_thread_file(void* arg)
{
  u64 *argp = (u64*)arg;
  void *results = (void*)argp[5];
  FILE *table = (FILE*)argp[4];
  long numbatches = (long)argp[3];
  long batchsize = (long)argp[2];
  long embedperrequest = (long)argp[1];
  //long q = (long)argp[0];

  void *result_ptr = results;
  embed_config_t *config = malloc(4*2*batchsize*embedperrequest + 16);
  config->attribute_size = 4;
  config->embedding_length = embed_length;
  config->result_embeddings = batchsize;
  config->input_embeddings = batchsize*embedperrequest;
  int list_idx;
  int id = 65;
  for (list_idx = 0;
       list_idx < 2*config->input_embeddings;
       list_idx+=2)
  {
    config->embedding_id_list[list_idx] = (list_idx/2)/embedperrequest;
    config->embedding_id_list[list_idx+1] = id;
    id += stride;
  }

  //sem_post(&sm_ready);
  //sem_wait(&sm_start);

  int batch_idx;
  for (batch_idx = 0;
       batch_idx < numbatches;
       batch_idx++)
  {
    embedding_lookup_file(table, result_ptr, config);
    result_ptr += 4 * embed_length * batchsize;
  }
  return 0;
}

/**
 * Looks up embeddings from SSD using Unvme IO.
 */
static void* lookup_thread_io(void* arg)
{
  u64 *argp = (u64*)arg;
  void *results = (void*)argp[5];
  long slba = (long)argp[4];
  long numbatches = (long)argp[3];
  long batchsize = (long)argp[2];
  long embedperrequest = (long)argp[1];
  long q = (long)argp[0];

  void *result_ptr = results;
  embed_config_t *config = malloc(4*2*batchsize*embedperrequest + 16);
  config->attribute_size = 4;
  config->embedding_length = embed_length;
  config->result_embeddings = batchsize;
  config->input_embeddings = batchsize*embedperrequest;
  int list_idx;
  int id = 65;
  for (list_idx = 0;
       list_idx < 2*config->input_embeddings;
       list_idx+=2)
  {
    config->embedding_id_list[list_idx] = (list_idx/2)/embedperrequest;
    config->embedding_id_list[list_idx+1] = id;
    id += stride;
  }

  //sem_post(&sm_ready);
  //sem_wait(&sm_start);

  int batch_idx;
  for (batch_idx = 0;
       batch_idx < numbatches;
       batch_idx++)
  {
    embedding_lookup_io(q, slba, result_ptr, config);
    result_ptr += 4 * embed_length * batchsize;
  }
  return 0;
}

/**
 * Looks up embeddings from Flash.
 */
static void* lookup_thread_ndp(void* arg)
{
  u64 *argp = (u64*)arg;
  void *results = (void*)argp[5];
  long slba = (long)argp[4];
  long numbatches = (long)argp[3];
  long batchsize = (long)argp[2];
  long embedperrequest = (long)argp[1];
  long q = (long)argp[0];

  void *result_ptr = results;

  //sem_post(&sm_ready);
  //sem_wait(&sm_start);

  int batch_idx;
  for (batch_idx = 0;
       batch_idx < numbatches;
       batch_idx++)
  {
    embed_config_t *config = (embed_config_t*)result_ptr;
    config->attribute_size = 4;
    config->embedding_length = embed_length;
    config->result_embeddings = batchsize;
    config->input_embeddings = batchsize*embedperrequest;
    int list_idx;
    int id = 65;
    for (list_idx = 0;
         list_idx < 2*config->input_embeddings;
         list_idx+=2)
    {
      config->embedding_id_list[list_idx] = (list_idx/2)/embedperrequest;
      config->embedding_id_list[list_idx+1] = id;
      id += stride;
    }

    int nlb = (4 * embed_length * batchsize) / ns->blocksize;
    if ((4 * embed_length * batchsize) % ns->blocksize) nlb++;
    int config_nlb = (4*2*batchsize*embedperrequest + 16) / ns->blocksize;
    if ((4*2*batchsize*embedperrequest + 16) % ns->blocksize) config_nlb++;

    int err = unvme_translate_region(ns, q,
        result_ptr,
        slba + q,
        nlb,
        config_nlb);
    if (err) errx(1, "translate");
    int size = 4 * embed_length * batchsize;
    if (size%4096) size = ((size/4096)+1)*4096;
    result_ptr += size;
  }
  return 0;
}

void run_test_file(long numbatches, long batchsize, long embedperrequest)
{
    u64 tablesize = 4 *
                    embed_length *
                    table_length;
    int q;
    u64 ts, tsc;

    void *dram_table = malloc(tablesize);
    int size = (4*embed_length*batchsize) > (4*2*batchsize*embedperrequest+16) ?
      (4*embed_length*batchsize) : (4*2*batchsize*embedperrequest+16);
    if (size % 4096) size = ((size/4096)+1)*4096;
    void *results = malloc(size * numbatches * qcount);

    // Timing configuration
    u64 tsec = rdtsc_second();
    // For breakpoint debugging on board, huge timeout
    //timeout = UNVME_TIMEOUT * tsec;
    timeout = 1000 * tsec;

    // Fill in test table
    // Leaving it as garbage for now, will do functional validation in another test

    // Write test table to Flash
    FILE *table = fopen("/media/openssd/fileiotest", "w");
    if (!table) errx(1, "cannot open fileiotest for write");
    fwrite(dram_table, tablesize, 1, table);
    fclose(table);
    table = fopen("/media/openssd/fileiotest", "r");
    if (!table) errx(1, "cannot open fileiotest for read");

    sleep(1);

    printf("Starting fileio test for %ld, %ld, %ld.\n",
           numbatches, batchsize, embedperrequest);

    tsc = rdtsc();

    for (q = 0; q < qcount; q++) {
        u64 arg[6] = {q,
                      embedperrequest,
                      batchsize,
                      numbatches,
                      (u64)table,
                      (u64)results};
        lookup_thread_file(arg);
    }

    ts = rdtsc_elapse(tsc);

    printf("FileIO: %lf seconds\n",
            ((double)ts / (double)tsec));
    free(dram_table);
    free(results);
}

/**
 * Run test to spawn one thread for each queue.
 */
void run_test(long numbatches, long batchsize, long embedperrequest)
{
    u64 tablesize = 4 *
                    embed_length *
                    table_length;
    u64 slba = 5000;
    int q;
    u64 ts, tsc;

    void *dram_table = unvme_alloc(ns, tablesize);
    int size = (4*embed_length*batchsize) > (4*2*batchsize*embedperrequest+16) ?
      (4*embed_length*batchsize) : (4*2*batchsize*embedperrequest+16);
    if (size % 4096) size = ((size/4096)+1)*4096;
    void *results = unvme_alloc(ns, size * numbatches * qcount);

    // Timing configuration
    u64 tsec = rdtsc_second();
    // For breakpoint debugging on board, huge timeout
    //timeout = UNVME_TIMEOUT * tsec;
    timeout = 1000 * tsec;

    // Fill in test table
    // Leaving it as garbage for now, will do functional validation in another test

    // Write test table to Flash
    rw_region(dram_table, slba, tablesize / ns->blocksize, 1, 0);
    // Write fence before translation
    unvme_flush(ns, 0);

    //-----------------------------------
    // Conventional
    // ----------------------------------
    sleep(1);

    printf("Starting conventional test for %ld, %ld, %ld.\n",
           numbatches, batchsize, embedperrequest);

    tsc = rdtsc();

    for (q = 0; q < qcount; q++) {
        u64 arg[6] = {q,
                      embedperrequest,
                      batchsize,
                      numbatches,
                      (u64)dram_table,
                      (u64)results};
        lookup_thread_conventional(arg);
    }

    ts = rdtsc_elapse(tsc);

    printf("Conventional: %lf seconds\n",
            ((double)ts / (double)tsec));

    //-----------------------------------
    // NDP
    // ----------------------------------

    //sem_init(&sm_ready, 0, 0);
    //sem_init(&sm_start, 0, 0);

    //for (q = 0; q < qcount; q++) {
    //    u64 arg[6] = {q,
    //                  embedperrequest,
    //                  batchsize,
    //                  numbatches,
    //                  (u64)slba,
    //                  (u64)results};
    //    pthread_create(&ses[q], 0, lookup_thread_ndp, (void*)arg);
    //    sem_wait(&sm_ready);
    //}

    sleep(1);

    printf("Starting ndp test for %ld, %ld, %ld.\n",
           numbatches, batchsize, embedperrequest);

    tsc = rdtsc();

    //for (q = 0; q < qcount; q++) sem_post(&sm_start);
    //for (q = 0; q < qcount; q++) pthread_join(ses[q], 0);
    u64 arg[6] = {0,
                  embedperrequest,
                  batchsize,
                  numbatches,
                  (u64)slba,
                  (u64)results};
    lookup_thread_ndp(arg);

    ts = rdtsc_elapse(tsc);

    sleep(1);

    printf("NDP: %lf seconds\n",
            ((double)ts / (double)tsec));

    //-----------------------------------
    // Unvme IO
    // ----------------------------------

    sleep(1);

    printf("Starting Unvme IO test for %ld, %ld, %ld.\n",
           numbatches, batchsize, embedperrequest);

    tsc = rdtsc();

    //for (q = 0; q < qcount; q++) sem_post(&sm_start);
    //for (q = 0; q < qcount; q++) pthread_join(ses[q], 0);
    u64 arg2[6] = {0,
                  embedperrequest,
                  batchsize,
                  numbatches,
                  (u64)slba,
                  (u64)results};
    lookup_thread_io(arg2);

    ts = rdtsc_elapse(tsc);

    sleep(1);

    printf("Unvme IO: %lf seconds\n",
            ((double)ts / (double)tsec));

    // Print/reset on board stats
    unvme_flush(ns, 0);


    unvme_free(ns, dram_table);
    unvme_free(ns, results);
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

    long numbatches = 1;
    long batchsize = 1; // 1 4K page for results from 1 batch
    long embedperrequest = 80;

    int fileiotest = 0;

    int opt;
    while ((opt = getopt(argc, argv, "v:q:d:b:s:e:f:r:")) != -1) {
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
        case 'b':
            batchsize = strtol(optarg, 0, 0);
            break;
        case 's':
            stride = 64;
            break;
        case 'e':
            numbatches = strtol(optarg, 0, 0);
            break;
        case 'f':
            fileiotest = 1;
            break;
        case 'r':
            embedperrequest = strtol(optarg, 0, 0);
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

    printf("EMBEDDING BANDWIDTH TEST BEGIN\n");
    time_t tstart = time(0);

    if (fileiotest)
    {
      run_test_file(numbatches, batchsize, embedperrequest);
    }
    else
    {
      if (!(ns = unvme_open(pciname))) exit(1);
      if (qcount <= 0 || qcount > ns->qcount) errx(1, "qcount limit %d", ns->qcount);
      if (qsize <= 1 || qsize > ns->qsize) errx(1, "qsize limit %d", ns->qsize);

      if (!qcount) qcount = ns->qcount;
      if (!qsize) qsize = ns->qsize;

      printf("%s qc=%d/%d qs=%d/%d bc=%#lx bs=%d mbio=%d\n",
              ns->device, qcount, ns->qcount, qsize, ns->qsize,
              ns->blockcount, ns->blocksize, ns->maxbpio);

      ses = calloc(qcount, sizeof(pthread_t));

      run_test(numbatches, batchsize, embedperrequest);

      free(ses);
      unvme_close(ns);
    }

    printf("EMBEDDING BANDWIDTH TEST COMPLETE (%ld secs)\n", time(0) - tstart);
    return 0;
}

