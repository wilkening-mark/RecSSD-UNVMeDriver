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
 * @brief Invoke NVMe set features command.
 */

#include "nvme_common.c"


/**
 * Main program.
 */
int main(int argc, char* argv[])
{
    const char* usage = "Usage: %s PCINAME NSID FEATURE_ID FEATURE_ARG";

    if (argc < 5) {
        warnx(usage, argv[0]);
        exit(1);
    }

    char* s = argv[2];
    int nsid = strtol(s, &s, 0);
    if (*s) {
        warnx(usage, argv[0]);
        exit(1);
    }
    s = argv[3];
    int fid = strtol(s, &s, 0);
    if (*s) {
        warnx(usage, argv[0]);
        exit(1);
    }
    s = argv[3];
    u32 res = strtol(s, &s, 0);
    if (*s) {
        warnx(usage, argv[0]);
        exit(1);
    }

    if (fid < NVME_FEATURE_ARBITRATION || fid > NVME_FEATURE_ASYNC_EVENT ||
        fid == NVME_FEATURE_LBA_RANGE)
        errx(1, "features_id %d not supported", fid);

    nvme_setup(argv[1], 8);
    vfio_dma_t* dma = vfio_dma_alloc(vfiodev, sizeof(nvme_feature_lba_data_t));
    if (!dma) errx(1, "vfio_dma_alloc");
    u64 prp1 = dma->addr;
    int err = nvme_acmd_get_features(nvmedev, nsid, fid, prp1, 0L, &res);
    if (err) errx(1, "set_features %d failed", fid);

    if (fid == NVME_FEATURE_ARBITRATION) {
        nvme_feature_arbitration_t* arb = (nvme_feature_arbitration_t*)&res;
        printf("1)  Arbitration:              hpw=%u mpw=%u lpw=%u ab=%u\n",
                arb->hpw, arb->mpw, arb->lpw, arb->ab);
    } else if (fid == NVME_FEATURE_POWER_MGMT) {
        nvme_feature_power_mgmt_t* pm = (nvme_feature_power_mgmt_t*)&res;
        printf("2)  Power Management:         ps=%u\n", pm->ps);
    } else if (fid == NVME_FEATURE_TEMP_THRESHOLD) {
        nvme_feature_temp_threshold_t* tt = (nvme_feature_temp_threshold_t*)&res;
        printf("4)  Temperature Threshold:    tmpth=%u\n", tt->tmpth);
    } else if (fid == NVME_FEATURE_ERROR_RECOVERY) {
        nvme_feature_error_recovery_t* er = (nvme_feature_error_recovery_t*)&res;
        printf("5)  Error Recovery:           tler=%u\n", er->tler);
    } else if (fid == NVME_FEATURE_WRITE_CACHE) {
        nvme_feature_write_cache_t* wc = (nvme_feature_write_cache_t*)&res;
        printf("6)  Volatile Write Cache:     wce=%u\n", wc->wce);
    } else if (fid == NVME_FEATURE_NUM_QUEUES) {
        nvme_feature_num_queues_t* nq = (nvme_feature_num_queues_t*)&res;
        printf("7)  Number of Queues:         nsq=%u ncq=%u\n", nq->nsq, nq->ncq);
    } else if (fid == NVME_FEATURE_INT_COALESCING) {
        nvme_feature_int_coalescing_t* intc = (nvme_feature_int_coalescing_t*)&res;
        printf("8)  Interrupt Coalescing:     time=%u thr=%u\n", intc->time, intc->thr);
    } else if (fid == NVME_FEATURE_INT_VECTOR) {
        nvme_feature_int_vector_t* intv = (nvme_feature_int_vector_t*)&res;
        printf("9)  Interrupt Vector Config:  iv=%u cd=%u\n", intv->iv, intv->cd);
    } else if (fid == NVME_FEATURE_WRITE_ATOMICITY) {
        nvme_feature_write_atomicity_t* wa = (nvme_feature_write_atomicity_t*)&res;
        printf("10) Write Atomicity:          dn=%u\n", wa->dn);
    } else if (fid == NVME_FEATURE_ASYNC_EVENT) {
        nvme_feature_async_event_t* aec = (nvme_feature_async_event_t*)&res;
        printf("11) Async Event Config:       smart=%u\n", aec->smart);
    }

    nvme_cleanup();
    return 0;
}

