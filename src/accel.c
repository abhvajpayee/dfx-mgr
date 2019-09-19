/*
 * Copyright (c) 2019, Xilinx Inc. and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#include <acapd/accel.h>
#include <acapd/assert.h>
#include <acapd/print.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

acapd_accel_pkg_hd_t *acapd_alloc_pkg(size_t size)
{
	acapd_accel_pkg_hd_t *pkg;

	pkg = malloc(size);
	if (pkg == NULL) {
		return pkg;
	}
	memset(pkg, 0, size);
	return pkg;
}

int acapd_config_pkg(acapd_accel_pkg_hd_t *pkg, uint32_t type, char *name,
		     size_t size, void *data, int is_end)
{
	acapd_accel_pkg_hd_t *tmppkg;
	char *pkgdata;

	acapd_assert(pkg != NULL);
	if (type >= ACAPD_ACCEL_PKG_TYPE_LAST) {
		acapd_perror("Failed to config pkg, non supported type %u.\n",
			     type);
		return ACAPD_ACCEL_FAILURE;
	}
	tmppkg = pkg;
	while(tmppkg->type != 0) {
		tmppkg = (acapd_accel_pkg_hd_t *)((char *)tmppkg +
						   sizeof(*tmppkg) +
						   tmppkg->size);
	}
	tmppkg->type = type;
	tmppkg->size = (uint64_t)size;
	tmppkg->is_end = is_end;
	pkgdata = (char *)tmppkg + sizeof(*tmppkg);
	memset(tmppkg->name, 0, sizeof(tmppkg->name));
	if (name != NULL) {
		size_t nsize;

		nsize = sizeof(tmppkg->name) - 1;
		if (nsize < strlen(name)) {
			nsize = strlen(name);
		}
		strncpy(tmppkg->name, name, nsize);
	}

	memcpy(pkgdata, data, size);
	return ACAPD_ACCEL_SUCCESS;
}

void init_accel(acapd_accel_t *accel, acapd_accel_pkg_hd_t *pkg)
{
	acapd_assert(accel != NULL);
	acapd_assert(pkg != NULL);
	memset(accel, 0, sizeof(*accel));
	accel->pkg = pkg;
	accel->status = ACAPD_ACCEL_STATUS_UNLOADED;
}

int load_accel(acapd_accel_t *accel, unsigned int async)
{
	int ret;

	acapd_assert(accel != NULL);
	acapd_assert(accel->pkg != NULL);
	/* TODO: Check if the accel is valid */
	/* For now, for now assume it is always PDI/DTB */
	ret = sys_load_accel(accel, async);
	if (ret == ACAPD_ACCEL_SUCCESS) {
		accel->status = ACAPD_ACCEL_STATUS_INUSE;
	} else if (ret == ACAPD_ACCEL_INPROGRESS) {
		accel->status = ACAPD_ACCEL_STATUS_LOADING;
	} else {
		accel->load_failure = ret;
	}
	return ret;
}

int accel_load_status(acapd_accel_t *accel)
{
	acapd_assert(accel != NULL);
	if (accel->load_failure != ACAPD_ACCEL_SUCCESS) {
		return accel->load_failure;
	} else if (accel->status != ACAPD_ACCEL_STATUS_INUSE) {
		return ACAPD_ACCEL_INVALID;
	} else {
		return ACAPD_ACCEL_SUCCESS;
	}
}

int remove_accel(acapd_accel_t *accel, unsigned int async)
{
	acapd_assert(accel != NULL);
	if (accel->status == ACAPD_ACCEL_STATUS_UNLOADED) {
		return ACAPD_ACCEL_SUCCESS;
	} else if (accel->status == ACAPD_ACCEL_STATUS_UNLOADING) {
		return ACAPD_ACCEL_INPROGRESS;
	} else {
		int ret;

		ret = sys_remove_accel(accel, async);
		if (ret == ACAPD_ACCEL_SUCCESS) {
			accel->status = ACAPD_ACCEL_STATUS_UNLOADED;
		} else if (ret == ACAPD_ACCEL_INPROGRESS) {
			accel->status = ACAPD_ACCEL_STATUS_UNLOADING;
		} else {
			accel->status = ACAPD_ACCEL_STATUS_UNLOADING;
		}
		return ret;
	}
}