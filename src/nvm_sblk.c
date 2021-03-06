/*
 * sblock - Spanning block functions
 *
 * Copyright (C) 2015 Javier González <javier@cnexlabs.com>
 * Copyright (C) 2015 Matias Bjørling <matias@cnexlabs.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  - Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <linux/lightnvm.h>
#include <liblightnvm.h>
#include <nvm.h>
#include <nvm_debug.h>
#include <nvm_omp.h>

struct nvm_sblk *nvm_sblk_new(struct nvm_dev *dev,
			      int ch_bgn, int ch_end,
			      int lun_bgn, int lun_end,
			      int blk)
{
	struct nvm_sblk *sblk;
	struct nvm_geo dev_geo = nvm_dev_attr_geo(dev);

	if (ch_bgn < 0 || ch_bgn > ch_end || ch_end >= dev_geo.nchannels) {
		NVM_DEBUG("invalid channel span");
		return NULL;
	}
	if (lun_bgn < 0 || lun_bgn > lun_end || lun_end >= dev_geo.nluns) {
		NVM_DEBUG("invalid lun span");
		return NULL;
	}
	if (blk < 0 || blk >= dev_geo.nblocks) {
		NVM_DEBUG("invalid block");
		return NULL;
	}

	sblk = malloc(sizeof(*sblk));
	if (!sblk)
		return NULL;

	sblk->pos_write = 0;
	sblk->pos_read = 0;

	sblk->dev = dev;

	sblk->bgn.g.ch = ch_bgn;	/* Construct span */
	sblk->bgn.g.lun = lun_bgn;
	sblk->bgn.g.blk = blk;
	sblk->bgn.g.pl = 0;
	sblk->bgn.g.pg = 0;
	sblk->bgn.g.sec = 0;

	sblk->end.g.ch = ch_end;
	sblk->end.g.lun = lun_end;
	sblk->end.g.blk = blk;
	sblk->end.g.pl = dev_geo.nplanes - 1;
	sblk->end.g.pg = dev_geo.npages - 1;
	sblk->end.g.sec = dev_geo.nsectors - 1;

	sblk->geo = dev_geo;		/* Inherit geometry from device */

	/* Overwrite with channels and luns */
	sblk->geo.nchannels = (sblk->end.g.ch - sblk->bgn.g.ch) + 1;
	sblk->geo.nluns = (sblk->end.g.lun - sblk->bgn.g.lun) + 1;
	sblk->geo.nblocks = 1; // For each ch/lun there is only one block

	/* Derive total number of bytes in sblk */
	sblk->geo.tbytes = sblk->geo.nchannels * sblk->geo.nluns * \
			   sblk->geo.nplanes * sblk->geo.nblocks * \
			   sblk->geo.npages * sblk->geo.nsectors * \
			   sblk->geo.nbytes;

	return sblk;
}

void nvm_sblk_free(struct nvm_sblk *sblk)
{
	free(sblk);
}

ssize_t nvm_sblk_erase(struct nvm_sblk *sblk)
{
	const struct nvm_geo geo = nvm_sblk_attr_geo(sblk);

	const int nplanes = geo.nplanes;

	const struct nvm_addr bgn = sblk->bgn;
	const struct nvm_addr end = sblk->end;

	ssize_t nerr = 0;

	int PLANE_FLAG = 0x0;

	PLANE_FLAG = (geo.nplanes == 4) ? NVM_MAGIC_FLAG_QUAD : PLANE_FLAG;
	PLANE_FLAG = (geo.nplanes == 2) ? NVM_MAGIC_FLAG_DUAL : PLANE_FLAG;

	#pragma omp parallel for schedule(static) collapse(2) reduction(+:nerr)
	for (int ch = bgn.g.ch; ch <= end.g.ch; ++ch) {
		for (int lun = bgn.g.lun; lun <= end.g.lun; ++lun) {
			struct nvm_addr addrs[nplanes];
			ssize_t err;

			for (int i = 0; i < nplanes; ++i) {
				addrs[i].ppa = bgn.ppa;
				addrs[i].g.ch = ch;
				addrs[i].g.lun = lun;
				addrs[i].g.pl = i % nplanes;
				// blk is fixed and inherited from bgn
				// pg is fixed and inherited from bgn (0)
				// sec is fixed and inherited from bgn (0)
			}

			err = nvm_addr_erase(sblk->dev,
					     addrs,
					     nplanes,
					     PLANE_FLAG);
			if (err) {
				NVM_DEBUG("FAILED: nvm_addr_erase err(%ld)", err);
				++nerr;
			}
		}
	}

	return -nerr;
}

ssize_t nvm_sblk_pwrite(struct nvm_sblk *sblk, const void *buf, size_t count,
			size_t offset)
{
	const struct nvm_addr bgn = sblk->bgn;

	const struct nvm_geo geo = nvm_sblk_attr_geo(sblk);

	const int nchannels = geo.nchannels;
	const int ch_off = bgn.g.ch;
	const int nluns = geo.nluns;
	const int lun_off = bgn.g.lun;

	const int npages = geo.npages;
	const int nplanes = geo.nplanes;
	const int nsectors = geo.nsectors;
	const int nbytes = geo.nbytes;

	const int alignment = (nplanes * nsectors * nbytes);

	const size_t spg_bgn = sblk->pos_write / alignment;
	const size_t spg_end = spg_bgn + (count / alignment);

	const int NVM_OP_NADDR = nplanes * nsectors;
	const int NVM_CMD_NADDR = NVM_OP_NADDR;

	const int nthreads = nchannels * nluns;

	ssize_t nerr = 0;

	int PLANE_FLAG = 0x0;

	const char *data;

	if (buf) {	// Use user-supplied buffer
		data = buf;
	} else {	// Allocate and use a padding buffer
		data = nvm_buf_alloc(geo, NVM_CMD_NADDR * nbytes);
		if (!data)
			return -count;
	}

	// Check alignment
	if ((count % alignment) || (sblk->pos_write % alignment))
		return -count;

	PLANE_FLAG = (geo.nplanes == 4) ? NVM_MAGIC_FLAG_QUAD : PLANE_FLAG;
	PLANE_FLAG = (geo.nplanes == 2) ? NVM_MAGIC_FLAG_DUAL : PLANE_FLAG;

	#pragma omp parallel num_threads(nthreads) reduction(+:nerr)
	{
		const int tid = omp_get_thread_num();

		#pragma omp barrier
		for (size_t spg = spg_bgn + tid; spg < spg_end; spg += nthreads) {
			struct nvm_addr addrs[NVM_CMD_NADDR];
			const char *data_off;

			if (buf)
				data_off = data + spg * nbytes * NVM_CMD_NADDR;
			else
				data_off = data;

			// channels X luns X pages
			int ch = (spg % nchannels) + ch_off;
			int lun = ((spg / nchannels) % nluns) + lun_off;
			int vpg = ((spg / nchannels) / nluns) % npages;

			// Unroll: nplane X nsector
			for (int i = 0; i < NVM_CMD_NADDR; ++i) {
				addrs[i].ppa = bgn.ppa;
				addrs[i].g.ch = ch;
				addrs[i].g.lun = lun;
				addrs[i].g.pg = vpg;
				addrs[i].g.pl = (i / nsectors) % nplanes;
				// blk is fixed and inherited from bgn
				addrs[i].g.sec = i % nsectors;
			}

			ssize_t err = nvm_addr_write(sblk->dev,
						     addrs,
						     NVM_CMD_NADDR,
						     data_off,
						     PLANE_FLAG);
			if (err) {
				NVM_DEBUG("FAILED: nvm_addr_write e(%ld)", err);
				++nerr;
			}
		}
	}

	return nerr;
}

ssize_t nvm_sblk_write(struct nvm_sblk *sblk, const void *buf, size_t count)
{
	ssize_t nerr = nvm_sblk_pwrite(sblk, buf, count, sblk->pos_write);

	if (!nerr)
		sblk->pos_write += count;

	return nerr;
}

ssize_t nvm_sblk_pad(NVM_SBLK sblk)
{
	return nvm_sblk_write(sblk, NULL, sblk->geo.tbytes - sblk->pos_write);
}

ssize_t nvm_sblk_pread(struct nvm_sblk *sblk, void *buf, size_t count,
		       size_t offset)
{
	const struct nvm_addr bgn = sblk->bgn;

	const struct nvm_geo geo = nvm_sblk_attr_geo(sblk);

	const int nchannels = geo.nchannels;
	const int ch_off = bgn.g.ch;
	const int nluns = geo.nluns;
	const int lun_off = bgn.g.lun;

	const int npages = geo.npages;
	const int nplanes = geo.nplanes;
	const int nsectors = geo.nsectors;
	const int nbytes = geo.nbytes;

	const int alignment = (nplanes * nsectors * nbytes);

	const size_t spg_bgn = offset / alignment;
	const size_t spg_end = spg_bgn + (count / alignment);

	const int NVM_OP_NADDR = nplanes * nsectors;
	const int NVM_CMD_NADDR = NVM_OP_NADDR;

	const int nthreads = nchannels * nluns;

	ssize_t nerr = 0;

	int PLANE_FLAG = 0x0;

	if ((count % alignment) || (offset % alignment)) {	// Check align
		return -count;
	}

	PLANE_FLAG = (geo.nplanes == 4) ? NVM_MAGIC_FLAG_QUAD : PLANE_FLAG;
	PLANE_FLAG = (geo.nplanes == 2) ? NVM_MAGIC_FLAG_DUAL : PLANE_FLAG;

	#pragma omp parallel num_threads(nthreads) reduction(+:nerr)
	{
		const int tid = omp_get_thread_num();

		#pragma omp barrier
		for (size_t spg = spg_bgn + tid; spg < spg_end; spg += nthreads) {
			struct nvm_addr addrs[NVM_CMD_NADDR];

			char *buf_off = buf + spg * nbytes * NVM_CMD_NADDR;

			// channels X luns X pages
			int ch = (spg % nchannels) + ch_off;
			int lun = ((spg / nchannels) % nluns) + lun_off;
			int vpg = ((spg / nchannels) / nluns) % npages;

			// Unroll: nplane X nsector
			for (int i = 0; i < NVM_CMD_NADDR; ++i) {
				addrs[i].ppa = bgn.ppa;
				addrs[i].g.ch = ch;
				addrs[i].g.lun = lun;
				addrs[i].g.pg = vpg;
				addrs[i].g.pl = (i / nsectors) % nplanes;
				// blk is fixed and inherited from bgn
				addrs[i].g.sec = i % nsectors;
			}

			ssize_t err = nvm_addr_read(sblk->dev,
						    addrs,
						    NVM_CMD_NADDR,
						    buf_off,
						    PLANE_FLAG);
			if (err) {
				NVM_DEBUG("FAILED: nvm_addr_read e(%ld)", err);
				++nerr;
			}
		}
	}

	return -nerr;
}

ssize_t nvm_sblk_read(struct nvm_sblk *sblk, void *buf, size_t count)
{
	ssize_t nerr = nvm_sblk_pread(sblk, buf, count, sblk->pos_read);

	if (!nerr)
		sblk->pos_read += count;

	return nerr;
}

struct nvm_addr nvm_sblk_attr_end(struct nvm_sblk *sblk)
{
	return sblk->end;
}

struct nvm_geo nvm_sblk_attr_geo(struct nvm_sblk *sblk)
{
	return sblk->geo;
}

size_t nvm_sblk_attr_pos_write(struct nvm_sblk *sblk)
{
	return sblk->pos_write;
}

size_t nvm_sblk_attr_pos_read(struct nvm_sblk *sblk)
{
	return sblk->pos_read;
}

void nvm_sblk_pr(struct nvm_sblk *sblk)
{
	printf("sblk {\n");
	printf(" "); nvm_dev_pr(sblk->dev);
	printf(" bgn "); nvm_addr_pr(sblk->bgn);
	printf(" end "); nvm_addr_pr(sblk->end);
	printf(" "); nvm_geo_pr(sblk->geo);
	printf("}\n");
}

