/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_ec_internal.h"

#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/log.h"

#include <unistd.h>

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

	/* Check if encoding workers are enabled via global configuration */
	if (!g_ec_encode_workers_enabled) {
		SPDK_NOTICELOG("EC bdev %s: Encoding dedicated workers disabled by user configuration\n",
			       ec_bdev->bdev.name);
		return 0;
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

	/* Strategy: EC encoding threads should use cores OUTSIDE SPDK's configured set
	 * to avoid interfering with Reactor threads. This ensures:
	 * 1. User's configured cores are fully dedicated to Reactors (as expected)
	 * 2. EC encoding gets dedicated cores for optimal performance
	 * 3. No need to "unbind" Reactors (which could cause issues)
	 * 
	 * Note: Reactor threads bind to ALL cores specified in SPDK's -m parameter,
	 * so we should only look for cores OUTSIDE the SPDK configuration.
	 */
	
	uint32_t spdk_core_count = spdk_env_get_core_count();
	long sys_cores = sysconf(_SC_NPROCESSORS_ONLN);
	uint32_t system_core_count = (sys_cores > 0) ? (uint32_t)sys_cores : spdk_core_count;
	
	/* Look for system cores OUTSIDE SPDK's configuration
	 * These cores are not bound to Reactors and can be used for EC encoding
	 */
	for (uint32_t sys_core = 0; sys_core < system_core_count && core_count < EC_MAX_ENCODE_WORKERS; sys_core++) {
		/* Check if this core is outside SPDK's configured set */
		bool in_spdk_set = false;
		SPDK_ENV_FOREACH_CORE(lcore) {
			if (lcore == sys_core) {
				in_spdk_set = true;
				break;
			}
		}
		
		/* Use system core if it's outside SPDK config and not already used by other threads */
		if (!in_spdk_set && !spdk_cpuset_get_cpu(&enum_ctx.used_cores, sys_core)) {
			/* Verify array bounds before assignment */
			if (spdk_unlikely(core_count >= EC_MAX_ENCODE_WORKERS)) {
				break;
			}
			cores[core_count++] = sys_core;
			spdk_cpuset_set_cpu(&enum_ctx.used_cores, sys_core, true);
			SPDK_NOTICELOG("EC bdev %s: Using system core %u (outside SPDK config) for encoding\n",
				       ec_bdev->bdev.name, sys_core);
		}
	}
	
	/* Provide clear guidance to users */
	if (core_count == 0) {
		SPDK_WARNLOG("\n"
			     "===========================================================\n"
			     "EC bdev %s: No dedicated cores available for encoding workers.\n"
			     "\n"
			     "RECOMMENDATION: Configure %u additional cores for EC encoding.\n"
			     "  - Current SPDK cores: %u\n"
			     "  - System total cores: %u\n"
			     "  - EC encoding needs: %u dedicated cores\n"
			     "\n"
			     "Example: If you want %u Reactors, start SPDK with %u cores:\n"
			     "  ./mimo_tgt -m 0x%X  # %u cores: %u for Reactors + %u for EC\n"
			     "\n"
			     "EC encoding will fall back to app_thread (may impact performance).\n"
			     "===========================================================\n",
			     ec_bdev->bdev.name,
			     EC_MAX_ENCODE_WORKERS,
			     spdk_core_count,
			     system_core_count,
			     EC_MAX_ENCODE_WORKERS,
			     spdk_core_count,
			     spdk_core_count + EC_MAX_ENCODE_WORKERS,
			     (1 << (spdk_core_count + EC_MAX_ENCODE_WORKERS)) - 1,
			     spdk_core_count + EC_MAX_ENCODE_WORKERS,
			     spdk_core_count,
			     EC_MAX_ENCODE_WORKERS);
		return 0;
	} else if (core_count < EC_MAX_ENCODE_WORKERS) {
		SPDK_WARNLOG("EC bdev %s: Only %u/%u encode worker cores available. "
			     "Consider configuring %u more cores for optimal performance.\n",
			     ec_bdev->bdev.name, core_count, EC_MAX_ENCODE_WORKERS,
			     EC_MAX_ENCODE_WORKERS - core_count);
	} else {
		SPDK_NOTICELOG("EC bdev %s: Successfully allocated %u dedicated cores for encoding workers\n",
			       ec_bdev->bdev.name, core_count);
	}
	
	/* TODO: Provide RPC/config options that allow specifying encode worker cores explicitly. */

	/* Create worker threads for each allocated core */
	for (uint32_t i = 0; i < core_count; i++) {
		struct spdk_cpuset mask;
		char thread_name[32];
		struct spdk_thread *worker_thread;
		bool is_system_core = false;

		/* Check if this core is outside SPDK's configured set */
		is_system_core = true;
		SPDK_ENV_FOREACH_CORE(lcore) {
			if (lcore == cores[i]) {
				is_system_core = false;
				break;
			}
		}

		spdk_cpuset_zero(&mask);
		spdk_cpuset_set_cpu(&mask, cores[i], true);
		snprintf(thread_name, sizeof(thread_name), "ec_enc_%u", cores[i]);

		worker_thread = spdk_thread_create(thread_name, &mask);
		if (worker_thread == NULL) {
			if (is_system_core) {
				/* System core binding may fail if DPDK doesn't manage it.
				 * This is expected - DPDK only manages cores in its configuration.
				 * Skip this core and continue with others.
				 */
				SPDK_WARNLOG("EC bdev %s: Cannot bind to system core %u "
					     "(not managed by DPDK), skipping\n",
					     ec_bdev->bdev.name, cores[i]);
				continue;
			} else {
				/* SPDK-managed core should work - this is a real error */
				SPDK_ERRLOG("Failed to create encode worker on SPDK core %u for EC bdev %s\n",
					    cores[i], ec_bdev->bdev.name);
				rc = -ENOMEM;
				break;
			}
		}

		/* Verify we don't exceed array bounds */
		if (spdk_unlikely(mp->encode_workers.count >= EC_MAX_ENCODE_WORKERS)) {
			SPDK_ERRLOG("EC bdev %s: Worker thread count exceeds maximum (%u)\n",
				    ec_bdev->bdev.name, EC_MAX_ENCODE_WORKERS);
			spdk_thread_destroy(worker_thread);
			rc = -E2BIG;
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
		SPDK_NOTICELOG("EC bdev %s: Created %u encode worker thread(s)\n",
			       ec_bdev->bdev.name, mp->encode_workers.count);
	} else {
		SPDK_WARNLOG("EC bdev %s: No encode worker threads created, will use app_thread\n",
			     ec_bdev->bdev.name);
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

	/* Wait for all active encoding tasks to complete, with timeout protection */
	uint32_t wait_count = 0;
	const uint32_t max_wait_iterations = 10000; /* 1 second max wait */
	while (__atomic_load_n(&mp->encode_workers.active_tasks, __ATOMIC_ACQUIRE) != 0) {
		if (spdk_unlikely(++wait_count > max_wait_iterations)) {
			SPDK_WARNLOG("EC bdev %s: Timeout waiting for encoding tasks to complete "
				     "(active_tasks=%d). Proceeding with cleanup.\n",
				     ec_bdev->bdev.name,
				     __atomic_load_n(&mp->encode_workers.active_tasks, __ATOMIC_ACQUIRE));
			break;
		}
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

		/* Wait for thread to exit, with timeout protection */
		uint32_t exit_wait_count = 0;
		const uint32_t max_exit_wait_iterations = 5000; /* 5 seconds max wait */
		while (!spdk_thread_is_exited(thread)) {
			if (spdk_unlikely(++exit_wait_count > max_exit_wait_iterations)) {
				SPDK_WARNLOG("EC bdev %s: Timeout waiting for worker thread %u to exit. "
					     "Proceeding with destroy.\n",
					     ec_bdev->bdev.name, i);
				break;
			}
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

	/* Use round-robin to distribute encoding tasks across worker threads */
	uint32_t idx = __atomic_fetch_add(&mp->encode_workers.next_rr, 1u, __ATOMIC_RELAXED);
	idx = idx % mp->encode_workers.count;

	/* Verify thread pointer is valid */
	if (spdk_unlikely(mp->encode_workers.threads[idx] == NULL)) {
		SPDK_ERRLOG("EC bdev %s: Worker thread at index %u is NULL, falling back to app_thread\n",
			    ec_bdev->bdev.name, idx);
		return app_thread;
	}

	if (is_dedicated != NULL) {
		*is_dedicated = true;
	}

	return mp->encode_workers.threads[idx];
}

