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
 * @brief UNVMe multiple concurrent devices test.
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

#include "unvme.h"

/// device thread session
typedef struct {
    char            pciname[16];    ///< PCI name
    int             pci;            ///< PCI device id
    int             ins;            ///< instance (start with 0)
    int             inscount;       ///< instance count
    pthread_t       thread;         ///< session thread
} ses_t;

// Global variables
static sem_t        sm_ready;       ///< semaphore for ready
static sem_t        sm_start;       ///< semaphore for start
static int          error;          ///< error flag


/**
 * Thread session per device test.
 */
void* test_session(void* arg)
{
    ses_t* ses = arg;
    const unvme_ns_t* ns;

    printf("Test device %s started\n", ses->pciname);
    sem_post(&sm_ready);
    sem_wait(&sm_start);

    if (!(ns = unvme_open(ses->pciname))) exit(1);
    printf("%s qc=%d/%d qs=%d/%d bc=%#lx bs=%d mbio=%d\n",
            ns->device, ns->qcount, ns->maxqcount, ns->qsize, ns->maxqsize,
            ns->blockcount, ns->blocksize, ns->maxbpio);

    u64 datasize = 256*1024*1024;
    u64 nlb = datasize >> ns->blockshift;
    u64 slba = nlb * ns->id;
    u64* wbuf = unvme_alloc(ns, datasize);
    u64* rbuf = unvme_alloc(ns, datasize);
    if (!wbuf || !rbuf) errx(1, "unvme_alloc %#lx failed", datasize);

    u64 w, wsize = datasize / sizeof(u64);
    for (w = 0; w < wsize; w++) {
        u64 pat = ses->thread + w;
        wbuf[w] = (pat << 32) | (~pat & 0xffffffff);
    }

    // for multiple namespace instances, different queues must be used
    // so divide up the queues for those instances
    int qcount = ns->qcount / ses->inscount;
    int q = qcount * ses->ins;
    while (!error && qcount--) {
        u64 lba = slba + q;
        u64 nb = nlb - q;
        printf("Test %s q%d lba %#lx nlb %#lx\n", ses->pciname, q, lba, nb);
        if (unvme_write(ns, q, wbuf, lba, nb)) {
            printf("ERROR: unvme_write %s q%d lba %#lx nlb %#lx\n",
                   ses->pciname, q, lba, nb);
            error = ses->pci;
            break;
        }
        memset(rbuf, 0, datasize);
        if (unvme_read(ns, q, rbuf, lba, nb)) {
            printf("ERROR: unvme_read %s q%d lba %#lx nlb %#lx\n",
                   ses->pciname, q, lba, nb);
            error = ses->pci;
            break;
        }
        if (memcmp(wbuf, rbuf, nb << ns->blockshift)) {
            printf("ERROR: data mismatch %s q%d lba %#lx nlb %#lx\n",
                   ses->pciname, q, lba, nb);
            error = ses->pci;
            break;
        }
        q++;
    }

    unvme_free(ns, rbuf);
    unvme_free(ns, wbuf);
    unvme_close(ns);

    if (!error)
        printf("Test device %s completed\n", ses->pciname);
    else if (error == ses->pci)
        printf("Test device %s failed\n", ses->pciname);
    return 0;
}

/**
 * Main program.
 */
int main(int argc, char* argv[])
{
    char* prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    char usage[256];
    sprintf(usage, "Usage: %s PCINAME PCINAME...\n\n\
       must specified 2 or more devices\n\
       (e.g.: %s 0a:00.0/1 0a:00.0/2 0b:00.0/1)", prog, prog);

    if (argc < 3) {
        warnx(usage, prog);
        exit(1);
    }

    int numses = argc - 1;
    ses_t* ses = malloc(numses * sizeof(ses_t));
    int i, k;
    for (i = 0; i < numses; i++) {
        int b, d, f, n = 1;
        if ((sscanf(argv[i+1], "%x:%x.%x/%x", &b, &d, &f, &n) != 4) &&
            (sscanf(argv[i+1], "%x:%x.%x", &b, &d, &f) != 3)) {
            warnx(usage, prog);
            exit(1);
        }
        sprintf(ses[i].pciname, "%02x:%02x.%x/%x", b, d, f, n);
        ses[i].pci = (b << 16) | (d << 8) | f;
        ses[i].ins = 0;
        ses[i].inscount = 1;
    }

    // update number of namespace instances in the device list
    // first pass to count number of instances
    for (i = 0; i < (numses - 1); i++) {
        for (k = i + 1; k < numses; k++) {
            if (ses[k].ins == 0 && ses[k].pci == ses[i].pci) {
                ses[k].ins++;
                ses[i].inscount++;
            }
        }
    }
    // second pass to update number of instances (applicable for 3+ instances)
    for (i = 0; i < (numses - 1); i++) {
        for (k = i + 1; k < numses; k++) {
            if (ses[k].pci == ses[i].pci) {
                ses[k].inscount = ses[i].inscount;
            }
        }
    }

    printf("MULTI-DEVICE TEST BEGIN\n");

    sem_init(&sm_ready, 0, 0);
    sem_init(&sm_start, 0, 0);
    time_t tstart = time(0);

    for (i = 0; i < numses; i++) {
        pthread_create(&ses[i].thread, 0, test_session, ses+i);
        sem_wait(&sm_ready);
    }
    for (i = 0; i < numses; i++) sem_post(&sm_start);
    for (i = 0; i < numses; i++) pthread_join(ses[i].thread, 0);

    sem_destroy(&sm_start);
    sem_destroy(&sm_ready);
    free(ses);

    printf("MULTI-DEVICE TEST COMPLETE (%ld secs)\n", time(0) - tstart);
    return 0;
}

