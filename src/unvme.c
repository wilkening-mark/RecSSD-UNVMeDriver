/**
 * Copyright (c) 2015-2016, Micron Technology, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * @brief UNVMe client library interface functions.
 */

#include "unvme_core.h"

/**
 * Open a client session with specified number of IO queues and queue size.
 * @param   pciname     PCI device name (as %x:%x.%x[/NSID] format)
 * @param   qcount      number of io queues
 * @param   qsize       io queue size
 * @return  namespace pointer or NULL if error.
 */
const unvme_ns_t* unvme_openq(const char* pciname, int qcount, int qsize)
{
    if (qcount < 0 || qsize < 0 || qsize == 1) {
        ERROR("invalid qcount %d or qsize %d", qcount, qsize);
        return NULL;
    }

    int b, d, f, nsid = 1;
    if ((sscanf(pciname, "%x:%x.%x/%x", &b, &d, &f, &nsid) != 4) &&
        (sscanf(pciname, "%x:%x.%x", &b, &d, &f) != 3)) {
        ERROR("invalid PCI %s (expect %%x:%%x.%%x[/NSID] format)", pciname);
        return NULL;
    }
    int pci = (b << 16) + (d << 8) + f;

    return unvme_do_open(pci, nsid, qcount, qsize);
}

/**
 * Open a client session.
 * @param   pciname     PCI device name (as %x:%x.%x[/NSID] format)
 * @return  namespace pointer or NULL if error.
 */
const unvme_ns_t* unvme_open(const char* pciname)
{
    return unvme_openq(pciname, 0, 0);
}

/**
 * Close a client session and delete its contained io queues.
 * @param   ns          namespace handle
 * @return  0 if ok else error code.
 */
int unvme_close(const unvme_ns_t* ns)
{
    return unvme_do_close(ns);
}

/**
 * Allocate an I/O buffer associated with a session.
 * @param   ns          namespace handle
 * @param   size        buffer size
 * @return  the allocated buffer or NULL if failure.
 */
void* unvme_alloc(const unvme_ns_t* ns, u64 size)
{
    return unvme_do_alloc(ns, size);
}

/**
 * Map an I/O buffer associated with a session.
 * @param   ns          namespace handle
 * @param   size        buffer size
 */
void unvme_map(const unvme_ns_t* ns, u64 size, void* pmb)
{
    return unvme_do_map(ns, size, pmb);
}

/**
 * Free an I/O buffer associated with a session.
 * @param   ns          namespace handle
 * @param   buf         buffer pointer
 * @return  0 if ok else -1.
 */
int unvme_free(const unvme_ns_t* ns, void* buf)
{
    return unvme_do_free(ns, buf);
}

/**
 * Read data from specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  I/O descriptor or NULL if failed.
 */
unvme_iod_t unvme_aread(const unvme_ns_t* ns, int qid, void* buf, u64 slba, u32 nlb)
{
    return (unvme_iod_t)unvme_rw(ns, qid, NVME_CMD_READ, buf, slba, nlb);
}

/**
 * Write data to specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  I/O descriptor or NULL if failed.
 */
unvme_iod_t unvme_awrite(const unvme_ns_t* ns, int qid,
                         const void* buf, u64 slba, u32 nlb)
{
    return (unvme_iod_t)unvme_rw(ns, qid, NVME_CMD_WRITE, (void*)buf, slba, nlb);
}

/**
 * Write configuration data for translation.
 *
 * Operation configuration data is placed in the first page of the
 * data buffer.
 *
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  I/O descriptor or NULL if failed.
 */
unvme_iod_t unvme_atranslate(const unvme_ns_t* ns, int qid,
                         void* buf, u64 slba)
{
    return (unvme_iod_t)unvme_rw_extended(ns, qid, NVME_CMD_WRITE, (void*)buf, slba, 1, 1);
}

/**
 * Read translated data from specified logical blocks on device.
 *
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  I/O descriptor or NULL if failed.
 */
unvme_iod_t unvme_atranslate_read(const unvme_ns_t* ns, int qid,
                         void* buf, u64 slba, u32 nlb)
{
    return (unvme_iod_t)unvme_rw_extended(ns, qid, NVME_CMD_READ, (void*)buf, slba, nlb, 1);
}

/**
 * Poll for completion status of a previous IO submission.
 * If there's no error, the descriptor will be freed.
 * @param   iod         IO descriptor
 * @param   timeout     in seconds
 * @return  0 if ok else error status (-1 for timeout).
 */
int unvme_apoll(unvme_iod_t iod, int timeout)
{
    return unvme_do_poll((unvme_desc_t*)iod, timeout, NULL);
}

/**
 * Poll for completion status of a previous IO submission.
 * If there's no error, the descriptor will be freed.
 * @param   iod         IO descriptor
 * @param   timeout     in seconds
 * @param   cqe_cs      CQE command specific DW0 returned
 * @return  0 if ok else error status (-1 for timeout).
 */
int unvme_apoll_cs(unvme_iod_t iod, int timeout, u32* cqe_cs)
{
    return unvme_do_poll((unvme_desc_t*)iod, timeout, cqe_cs);
}

/**
 * Read data from specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  0 if ok else error status.
 */
int unvme_read(const unvme_ns_t* ns, int qid, void* buf, u64 slba, u32 nlb)
{
    unvme_desc_t* desc = unvme_rw(ns, qid, NVME_CMD_READ, buf, slba, nlb);
    if (desc) {
        sched_yield();
        return unvme_do_poll(desc, UNVME_TIMEOUT, NULL);
    }
    return -1;
}

int unvme_flush(const unvme_ns_t* ns, int qid)
{
    unvme_desc_t* desc = unvme_aflush(ns, qid);
    if (desc) {
        sched_yield();
        return unvme_do_poll(desc, UNVME_TIMEOUT, NULL);
    }
    return -1;
}

/**
 * Write data to specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  0 if ok else error status.
 */
int unvme_write(const unvme_ns_t* ns, int qid,
                const void* buf, u64 slba, u32 nlb)
{
    unvme_desc_t* desc = unvme_rw(ns, qid, NVME_CMD_WRITE, (void*)buf, slba, nlb);
    if (desc) {
        sched_yield();
        return unvme_do_poll(desc, UNVME_TIMEOUT, NULL);
    }
    return -1;
}

/**
 * Read column data in specified logical blocks on device.
 * @param   ns          namespace handle
 * @param   qid         client queue index
 * @param   buf         data buffer (from unvme_alloc)
 * @param   slba        starting logical block
 * @param   nlb         number of logical blocks
 * @return  0 if ok else error status.
 */
int unvme_translate_region(const unvme_ns_t* ns, int qid,
                    void* buf, u64 slba, u32 nlb, u32 config_nlb)
{
  unsigned int nrequests = (nlb / ns->maxbpio) + !!(nlb % ns->maxbpio) + 1;
  unvme_desc_t** desc = malloc(ns->maxiopq * sizeof(unvme_desc_t*));
  unsigned int readOffset = 0;

  int i = 0;
  desc[i] = unvme_rw_extended(ns, qid, NVME_CMD_WRITE, (void*)buf, slba, config_nlb, 1);
  for (i = 1; i < ns->maxiopq; i++)
  {
    if (i >= nrequests) break;
    desc[i] = unvme_rw_extended(ns, qid, NVME_CMD_READ,
        (void*)buf+readOffset, slba,
        (i == nrequests-1 && nlb % ns->maxbpio) ?
          nlb % ns->maxbpio : ns->maxbpio, 1);
    readOffset += ns->maxbpio * ns->blocksize;
  }

  int pending = i;
  int j = 0;
  int maxj = (ns->maxbpio < nrequests) ? ns->maxbpio : nrequests;
  do {
    if (desc[j])
    {
      sched_yield();
      if (unvme_do_poll(desc[j], UNVME_TIMEOUT, NULL) == 0)
      {
        if (i < nrequests)
        {
          desc[j] = unvme_rw_extended(ns, qid, NVME_CMD_READ,
              (void*)buf+readOffset, slba,
              (i == nrequests-1 && nlb % ns->maxbpio) ?
                nlb % ns->maxbpio : ns->maxbpio, 1);
          readOffset += ns->maxbpio * ns->blocksize;
          i++;
        }
        else
        {
          desc[j] = 0;
          pending--;
        }
      }
    }
    if (++j == maxj) j = 0;
  } while (pending > 0);

  free(desc);
  return 0;
}
