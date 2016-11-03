/*
 * utils - Helper functions and utitilies used by liblnvm
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
#define _GNU_SOURCE
#include <stdlib.h>

#include <liblightnvm.h>
#include <string.h>
#include <nvm_debug.h>
#include <nvm_util.h>

/*
 * Searches the udev 'subsystem' for device named 'dev_name' of type 'devtype'
 *
 * NOTE: Caller is responsible for calling `udev_device_unref` on the returned
 * udev_device
 *
 * @returns First device in 'subsystem' of given 'devtype' with given 'dev_name'
 */
struct udev_device* udev_dev_find(struct udev *udev, const char *subsystem,
				  const char *devtype, const char *dev_name)
{
	struct udev_device *dev = NULL;

	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;

	enumerate = udev_enumerate_new(udev);	/* Search 'subsystem' */
	udev_enumerate_add_match_subsystem(enumerate, subsystem);
	udev_enumerate_scan_devices(enumerate);
	devices = udev_enumerate_get_list_entry(enumerate);
	udev_list_entry_foreach(dev_list_entry, devices) {
		const char *path;
		int path_len;
		
		path = udev_list_entry_get_name(dev_list_entry);
		if (!path) {
			NVM_DEBUG("Failed retrieving path from entry\n");
			continue;
		}
		path_len = strlen(path);

		if (dev_name) {			/* Compare name */
			int dev_name_len = strlen(dev_name);
			int match = strcmp(dev_name,
					   path + path_len-dev_name_len);
			if (0 != match) {
				NVM_DEBUG("Name comparison failed\n");
				continue;
			}
		}
						/* Get the udev object */
		dev = udev_device_new_from_syspath(udev, path);
		if (!dev) {
			NVM_DEBUG("Failed retrieving device from path\n");
			continue;
		}

		if (devtype) {			/* Compare device type */
			const char* sys_devtype;
			int sys_devtype_match;
			
			sys_devtype = udev_device_get_devtype(dev);
			if (!sys_devtype) {
				NVM_DEBUG("sys_devtype(%s)", sys_devtype);
				udev_device_unref(dev);
				dev = NULL;
				continue;
			}

			sys_devtype_match = strcmp(devtype, sys_devtype);
			if (0 != sys_devtype_match) {
				NVM_DEBUG("%s != %s\n", devtype, sys_devtype);
				udev_device_unref(dev);
				dev = NULL;
				continue;
			}
		}

		break;
	}

	return dev;
}

struct udev_device* udev_nvmdev_find(struct udev *udev, const char *dev_name)
{
	struct udev_device* dev = udev_dev_find(udev, "gennvm", NULL, dev_name);

	if (!dev) {
		NVM_DEBUG("NOTHING FOUND\n");
		return dev;
	}

	return dev;
}

void* nvm_buf_alloc(NVM_GEO geo, size_t nbytes)
{
	char *buf;
	int ret;

	ret = posix_memalign((void**)&buf, geo.nbytes, nbytes);
	if (ret) {
		return NULL;
	}

	return buf;
}

void* nvm_vpg_buf_alloc(NVM_GEO geo)
{
	char *buf;
	int ret;

	ret = posix_memalign((void**)&buf, geo.nbytes, geo.vpg_nbytes);
	if (ret) {
		return NULL;
	}

	return buf;
}

void* nvm_vblk_buf_alloc(NVM_GEO geo)
{
	char *buf;
	int ret;

	ret = posix_memalign((void**)&buf, geo.nbytes, geo.vblk_nbytes);
	if (ret) {
		return NULL;
	}

	return buf;
}

void nvm_buf_pr(char *buf, size_t buf_len)
{
	const int width = 32;
	int i;
	printf("** NVM_BUF_PR - BEGIN **");
	for (i = 0; i < buf_len; i++) {
		if (!(i % width))
			printf("\ni[%d,%d]: ", i, i+(width-1));
		printf(" %c", buf[i]);
	}
	printf("\n** NVM_BUF_PR - END **\n");
}

void nvm_buf_fill(char *buf, size_t buf_len)
{
	#pragma omp parallel for schedule(static)
	for (size_t i = 0; i < buf_len; ++i)
		buf[i] = (i % 26) + 65;
}

