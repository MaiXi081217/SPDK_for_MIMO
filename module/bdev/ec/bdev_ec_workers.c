/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_ec_internal.h"

#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/log.h"

struct ec_thread_enum_ctx {
	struct spdk_cpuset used_cores;
	bool completed;
};

static void
ec_bdev_collect_thread_cpuset(void *ctx)
{
	struct ec_thread_enum_ctx *state = ctx;
	const struct spdk_cpuset *mask = spdk_thread_get_cpumask(spdk_get_thread());

	if (mask != NULL) {
		spdk_cpuset_or(&state->used_cores, mask);
	}
}

static void
ec_bdev_collect_thread_cpuset_done(void *ctx)
{
	struct ec_thread_enum_ctx *state = ctx;
	state->completed = true;
}

static void
ec_bdev_reset_encode_workers(struct ec_bdev_module_private *mp)
{
	uint32_t i;

	if (mp == NULL) {
		return;
	}

	mp->encode_workers.count = 0;
	mp->encode_workers.next_rr = 0;
	mp->encode_workers.active_tasks = 0;
	mp->encode_workers.enabled = false;

	for (i = 0; i < EC_MAX_ENCODE_WORKERS; i++) {
		mp->encode_workers.threads[i] = NULL;
	}
}

static void
ec_bdev_encode_worker_exit(void *ctx)
{
	(void)ctx;
	spdk_thread_exit(spdk_get_thread());
}

int
ec_bdev_init_encode_workers(struct ec_bdev *ec_bdev)
{
	struct ec_bdev_module_private *mp = ec_bdev->module_private;
	struct spdk_thread *caller;
	struct ec_thread_enum_ctx enum_ctx = {};
	uint32_t cores[EC_MAX_ENCODE_WORKERS] = {};
	uint32_t core_count = 0;
	uint32_t lcore;
	int rc = 0;

	if (mp == NULL) {
		return -EINVAL;
	}

	ec_bdev_reset_encode_workers(mp);

	caller = spdk_get_thread();
	if (caller == NULL) {
		SPDK_WARNLOG("EC bdev %s: cannot create encode workers without current SPDK thread context\n",
			     ec_bdev->bdev.name);
		return 0;
	}

	spdk_cpuset_zero(&enum_ctx.used_cores);
	enum_ctx.completed = false;
	spdk_for_each_thread(ec_bdev_collect_thread_cpuset, &enum_ctx, ec_bdev_collect_thread_cpuset_done);
	while (!enum_ctx.completed) {
		if (spdk_thread_poll(caller, 0, 0) == 0) {
			spdk_delay_us(100);
		}
	}

	SPDK_ENV_FOREACH_CORE(lcore) {
		if (!spdk_cpuset_get_cpu(&enum_ctx.used_cores, lcore)) {
			cores[core_count++] = lcore;
			spdk_cpuset_set_cpu(&enum_ctx.used_cores, lcore, true);
			if (core_count == EC_MAX_ENCODE_WORKERS) {
				break;
			}
		}
	}

	/* TODO: Provide RPC/config options that allow specifying encode worker cores explicitly. */

	if (core_count == 0) {
		return 0;
	}

	for (uint32_t i = 0; i < core_count; i++) {
		struct spdk_cpuset mask;
		char thread_name[32];
		struct spdk_thread *worker_thread;

		spdk_cpuset_zero(&mask);
		spdk_cpuset_set_cpu(&mask, cores[i], true);
		snprintf(thread_name, sizeof(thread_name), "ec_enc_%u", cores[i]);

		worker_thread = spdk_thread_create(thread_name, &mask);
		if (worker_thread == NULL) {
			SPDK_ERRLOG("Failed to create encode worker on core %u for EC bdev %s\n",
				    cores[i], ec_bdev->bdev.name);
			rc = -ENOMEM;
			break;
		}

		mp->encode_workers.threads[mp->encode_workers.count++] = worker_thread;
	}

	if (rc != 0) {
		ec_bdev_cleanup_encode_workers(ec_bdev);
		return rc;
	}

	if (mp->encode_workers.count > 0) {
		mp->encode_workers.enabled = true;
	}

	return 0;
}

void
ec_bdev_cleanup_encode_workers(struct ec_bdev *ec_bdev)
{
	struct ec_bdev_module_private *mp;
	uint32_t i;

	if (ec_bdev == NULL) {
		return;
	}

	mp = ec_bdev->module_private;
	if (mp == NULL || mp->encode_workers.count == 0) {
		return;
	}

	mp->encode_workers.enabled = false;

	while (__atomic_load_n(&mp->encode_workers.active_tasks, __ATOMIC_ACQUIRE) != 0) {
		spdk_delay_us(100);
	}

	for (i = 0; i < mp->encode_workers.count; i++) {
		struct spdk_thread *thread = mp->encode_workers.threads[i];

		if (thread == NULL) {
			continue;
		}

		if (spdk_thread_send_msg(thread, ec_bdev_encode_worker_exit, NULL) != 0) {
			SPDK_ERRLOG("Failed to signal encode worker shutdown for EC bdev %s\n",
				    ec_bdev->bdev.name);
		}
	}

	for (i = 0; i < mp->encode_workers.count; i++) {
		struct spdk_thread *thread = mp->encode_workers.threads[i];

		if (thread == NULL) {
			continue;
		}

		while (!spdk_thread_is_exited(thread)) {
			spdk_delay_us(1000);
		}
		spdk_thread_destroy(thread);
		mp->encode_workers.threads[i] = NULL;
	}

	mp->encode_workers.count = 0;
	mp->encode_workers.next_rr = 0;
	mp->encode_workers.active_tasks = 0;
	mp->encode_workers.enabled = false;
}

struct spdk_thread *
ec_bdev_get_encode_worker_thread(struct ec_bdev *ec_bdev, bool *is_dedicated)
{
	struct ec_bdev_module_private *mp;
	struct spdk_thread *app_thread = spdk_thread_get_app_thread();

	if (is_dedicated != NULL) {
		*is_dedicated = false;
	}

	if (ec_bdev == NULL) {
		return app_thread;
	}

	mp = ec_bdev->module_private;
	if (mp == NULL || !mp->encode_workers.enabled || mp->encode_workers.count == 0) {
		return app_thread;
	}

	uint32_t idx = __atomic_fetch_add(&mp->encode_workers.next_rr, 1u, __ATOMIC_RELAXED);

	if (is_dedicated != NULL) {
		*is_dedicated = true;
	}

	return mp->encode_workers.threads[idx % mp->encode_workers.count];
}

