// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2021-2022, Intel Corporation */

/*
 * mover.c -- default pmem2 data mover
 */

#include "libpmem2.h"
#include "mover.h"
#include "map.h"
#include "membuf.h"
#include "out.h"
#include "pmem2_utils.h"
#include "util.h"
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>

struct data_mover {
	struct vdm base; /* must be first */
	struct pmem2_map *map;
	struct membuf *membuf;
};

struct data_mover_op {
	struct vdm_operation op;
	int complete;
};

/*
 * sync_operation_check -- always returns COMPLETE because sync mover operations
 * are complete immediately after starting.
 */
static enum future_state
sync_operation_check(void *data, const struct vdm_operation *operation)
{
	LOG(3, "data %p", data);
	SUPPRESS_UNUSED(operation);

	struct data_mover_op *sync_op = data;

	int complete;
	util_atomic_load_explicit32(&sync_op->complete, &complete,
		memory_order_acquire);

	return complete ? FUTURE_STATE_COMPLETE : FUTURE_STATE_IDLE;
}

/*
 * sync_operation_new -- creates a new sync operation
 */
static void *
sync_operation_new(struct vdm *vdm, const enum vdm_operation_type type)
{
	LOG(3, "vdm %p", vdm);

	SUPPRESS_UNUSED(type);

	struct data_mover *vdm_sync = (struct data_mover *)vdm;
	struct data_mover_op *sync_op = membuf_alloc(vdm_sync->membuf,
		sizeof(struct data_mover_op));

	if (sync_op == NULL)
		return NULL;

	sync_op->complete = 0;

	return sync_op;
}

/*
 * sync_operation_delete -- deletes sync operation
 */
static void
sync_operation_delete(void *data, const struct vdm_operation *operation,
	struct vdm_operation_output *output)
{
	output->result = VDM_SUCCESS;

	switch (operation->type) {
	case VDM_OPERATION_MEMCPY:
		output->type = VDM_OPERATION_MEMCPY;
		output->output.memcpy.dest =
			operation->data.memcpy.dest;
		break;
	case VDM_OPERATION_MEMMOVE:
		output->type = VDM_OPERATION_MEMMOVE;
		output->output.memmove.dest =
			operation->data.memcpy.dest;
		break;
	default:
		FATAL("unsupported operation type");

	}
	membuf_free(data);
}

/*
 * sync_operation_start -- start (and perform) a synchronous memory operation
 */
static int
sync_operation_start(void *data, const struct vdm_operation *operation,
	struct future_notifier *n)
{
	LOG(3, "data %p op %p, notifier %p", data, operation, n);
	struct data_mover_op *sync_data =
		(struct data_mover_op *)data;
	struct data_mover *mover = membuf_ptr_user_data(data);
	if (n)
		n->notifier_used = FUTURE_NOTIFIER_NONE;

	switch (operation->type) {
		case VDM_OPERATION_MEMCPY: {
			pmem2_memcpy_fn memcpy_fn;
			memcpy_fn = pmem2_get_memcpy_fn(mover->map);

			memcpy_fn(operation->data.memcpy.dest,
				operation->data.memcpy.src,
				operation->data.memcpy.n,
				PMEM2_F_MEM_NONTEMPORAL);
			break;
		}
		case VDM_OPERATION_MEMMOVE: {
			pmem2_memmove_fn memmove_fn;
			memmove_fn = pmem2_get_memmove_fn(mover->map);

			memmove_fn(operation->data.memcpy.dest,
				operation->data.memcpy.src,
				operation->data.memcpy.n,
				PMEM2_F_MEM_NONTEMPORAL);
			break;
		}
		default:
			FATAL("unsupported operation type");
	}
	util_atomic_store_explicit32(&sync_data->complete,
		1, memory_order_release);

	return 0;
}

static struct vdm data_mover_vdm = {
	.op_new = sync_operation_new,
	.op_delete = sync_operation_delete,
	.op_check = sync_operation_check,
	.op_start = sync_operation_start,
};

/*
 * mover_new -- creates a new synchronous data mover
 */
int
mover_new(struct pmem2_map *map, struct vdm **vdm)
{
	LOG(3, "map %p, vdm %p", map, vdm);
	int ret;
	struct data_mover *dms = pmem2_malloc(sizeof(*dms), &ret);
	if (dms == NULL)
		return ret;

	dms->base = data_mover_vdm;
	dms->map = map;
	*vdm = (struct vdm *)dms;

	dms->membuf = membuf_new(dms);
	if (dms->membuf == NULL) {
		ret = PMEM2_E_ERRNO;
		goto membuf_failed;
	}

	return 0;

membuf_failed:
	free(dms);
	return ret;
}

/*
 * mover_delete -- deletes a synchronous data mover
 */
void
mover_delete(struct vdm *dms)
{
	membuf_delete(((struct data_mover *)dms)->membuf);
	free((struct data_mover *)dms);
}

/*
 * pmem2_memcpy_async -- returns a memcpy future
 */
struct vdm_operation_future
pmem2_memcpy_async(struct pmem2_map *map,
	void *pmemdest,	const void *src, size_t len, unsigned flags)
{
	LOG(3, "map %p, pmemdest %p, src %p, len %" PRIu64 ", flags %u",
		map, pmemdest, src, len, flags);
	SUPPRESS_UNUSED(flags);
	return vdm_memcpy(map->vdm, pmemdest, (void *)src, len, 0);
}
