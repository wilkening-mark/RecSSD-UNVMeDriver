/**
 * @file
 * @brief UNVMe RL embedding table lookup library for use from python.
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
  u32 embedding_length;
  u32 result_embeddings;
  u32 input_embeddings;
  u32 table_id;

  u32 embedding_id_list[];
} embed_config_t;

// Global variables
static const unvme_ns_t* ns;           ///< unvme namespace pointer
static int qcount = 8;                 ///< queue count
static int qsize = 256;                ///< queue size
static u64 timeout = 0;                ///< tsc elapsed timeout
static char* pciname = "01:00.0";      ///< PCIe identifier for OpenSSD
static int slba = 5000;                ///< Block address to store embedding table
// Lets put tables 10GB apart -- stride is in logical blocks (4KB)
static int table_stride = 2500000;

static void* fromPageAlloc;

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
  timeout = 30 * rdtsc_second();

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

void open_unvme()
{
  if (!(ns = unvme_open(pciname))) exit(1);
  fromPageAlloc = unvme_alloc(ns, 4096);
}

void close_unvme()
{
  unvme_free(ns, fromPageAlloc);
  unvme_close(ns);
}

void flush_unvme()
{
  unvme_flush(ns, 0);
}

void unvme_write_table(float* table, int vector_length,
        int table_length, int table_id)
{
  void* buf = unvme_alloc(ns, 4 * vector_length * table_length);

  float* bufp = (float*)buf;
  float* tablep = table;
  int i;
  for (i = 0; i < vector_length * table_length; i++)
  {
    *bufp = *tablep;
    bufp++;
    tablep++;
  }

  rw_region(buf, slba + (table_id * table_stride),
          (4 * vector_length * table_length) / ns->blocksize, 1, 0);
}

float* unvme_sparse_length_sum(
    int* flatInd, int vector_length, int batchsize, int embed_per_result,
    int table_id, int qid, int input_embeddings)
{
  int buffersize = (4*vector_length*batchsize > (4*2*input_embeddings + 20)) ?
    4*vector_length*batchsize:(4*2*input_embeddings + 20);
  void* result_ptr;
  result_ptr = unvme_alloc(ns, buffersize);
  embed_config_t *config = (embed_config_t*)result_ptr;
  config->attribute_size = 4;
  config->embedding_length = vector_length;
  config->result_embeddings = batchsize;
  config->input_embeddings = input_embeddings;
  config->table_id = table_id;
  int i;
  for(i = 0; i < 2*input_embeddings; i++)
    config->embedding_id_list[i] = flatInd[i];
  int nlb = (4 * vector_length * batchsize) / ns->blocksize;
  if ((4 * vector_length * batchsize) % ns->blocksize) nlb++;
  int config_nlb = (4*2*input_embeddings + 20) / ns->blocksize;
  if ((4*2*input_embeddings + 20) % ns->blocksize) config_nlb++;

  u64 tstart = rdtsc();
  int err = unvme_translate_region(ns, qid,
      result_ptr,
      slba + (table_id * table_stride) + qid,
      nlb,
      config_nlb);
  if (err) errx(1, "translate");
  u64 telapse = rdtsc_elapse(tstart);

  float* time_ptr = ((float*)result_ptr) + (vector_length*batchsize);
  *time_ptr = ((float)telapse / (float)rdtsc_second());

  return (float*)result_ptr;
}

float* unvme_read_embedding(int embedidx, int vector_length, int table_id, int qid)
{
  int attribute_size = 4;
  void* fromPage = fromPageAlloc;
  void* embedding = fromPage +
      ((embedidx % (4096 / (attribute_size * vector_length))) * (attribute_size * vector_length));

  u64 tstart = rdtsc();
  unvme_read(ns, qid, fromPage,
      slba + (table_id * table_stride) + ((attribute_size * vector_length * embedidx) / 4096),
      1);
  u64 telapse = rdtsc_elapse(tstart);

  float* time_ptr = ((float*)embedding) + (vector_length);
  *time_ptr = ((float)telapse / (float)rdtsc_second());

  return (float*)embedding;
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

float* unvme_sparse_length_sum_baseline(
    int* flatInd, int vector_length, int batchsize, int embed_per_result,
    int table_id)
{
  int buffersize = (4*vector_length*batchsize > (4*2*batchsize*embed_per_result + 20)) ?
    4*vector_length*batchsize:(4*2*batchsize*embed_per_result + 20);
  void* result_ptr = unvme_alloc(ns, buffersize);
  embed_config_t *config = (embed_config_t*)malloc(
          4*2*batchsize*embed_per_result + 20);
  config->attribute_size = 4;
  config->embedding_length = vector_length;
  config->result_embeddings = batchsize;
  config->input_embeddings = batchsize*embed_per_result;
  config->table_id = table_id;
  int i;
  for(i = 0; i < 2*batchsize*embed_per_result; i++)
    config->embedding_id_list[i] = flatInd[i];

  int q = 0;
  embedding_lookup_io(q,
      slba + (table_id * table_stride),
      result_ptr,
      config);

  return (float*)result_ptr;
}
