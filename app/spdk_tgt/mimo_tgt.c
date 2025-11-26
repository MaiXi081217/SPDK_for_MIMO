/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/log.h"
#if defined(SPDK_CONFIG_VHOST)
#include "spdk/vhost.h"
#endif
#if defined(SPDK_CONFIG_VFIO_USER)
#include "spdk/vfu_target.h"
#endif
#include "spdk_go_notify.h"

/* Forward declaration: EC module's global configuration variable
 * Defined in module/bdev/ec/bdev_ec.c
 */
extern bool g_ec_encode_workers_enabled;

#if defined(SPDK_CONFIG_VHOST) || defined(SPDK_CONFIG_VFIO_USER)
#define SPDK_SOCK_PATH "S:"
#else
#define SPDK_SOCK_PATH
#endif

static const char *g_pid_path = NULL;
static bool g_disable_ec_encode_workers = false;  /* Default: enabled */
static const char g_spdk_tgt_get_opts_string[] = "f:E" SPDK_SOCK_PATH;

static void
spdk_tgt_usage(void)
{
	printf(" -f <file>                 pidfile save pid to file under given path\n");
	printf(" -E                        disable EC encoding dedicated worker threads (default: enabled)\n");
#if defined(SPDK_CONFIG_VHOST) || defined(SPDK_CONFIG_VFIO_USER)
	printf(" -S <path>                 directory where to create vhost/vfio-user sockets (default: pwd)\n");
#endif
}

static void
spdk_tgt_save_pid(const char *pid_path)
{
	FILE *pid_file;

	pid_file = fopen(pid_path, "w");
	if (pid_file == NULL) {
		fprintf(stderr, "Couldn't create pid file '%s': %s\n", pid_path, strerror(errno));
		exit(EXIT_FAILURE);
	}

	fprintf(pid_file, "%d\n", getpid());
	fclose(pid_file);
}


static int
spdk_tgt_parse_arg(int ch, char *arg)
{
	switch (ch) {
	case 'f':
		g_pid_path = arg;
		break;
	case 'E':
		g_disable_ec_encode_workers = true;
		break;
#if defined(SPDK_CONFIG_VHOST) || defined(SPDK_CONFIG_VFIO_USER)
	case 'S':
#ifdef SPDK_CONFIG_VHOST
		spdk_vhost_set_socket_path(arg);
#endif
#ifdef SPDK_CONFIG_VFIO_USER
		spdk_vfu_set_socket_path(arg);
#endif
		break;
#endif
	default:
		return -EINVAL;
	}
	return 0;
}

static void
spdk_tgt_started(void *arg1)
{
	int notify_rc;
	char payload[128];

	if (g_pid_path) {
		spdk_tgt_save_pid(g_pid_path);
	}

	if (getenv("MEMZONE_DUMP") != NULL) {
		spdk_memzone_dump(stdout);
		fflush(stdout);
	}

	/* Send startup notification */
	snprintf(payload, sizeof(payload), "{\"pid\": %d, \"name\": \"mimo_tgt\"}", getpid());
	notify_rc = NotifyEvent("mimo_tgt_started", payload);
	if (notify_rc == 0) {
		SPDK_NOTICELOG("Startup notification sent: event=mimo_tgt_started, payload=%s\n", payload);
	} else {
		SPDK_WARNLOG("Failed to send startup notification (rc=%d)\n", notify_rc);
	}

	/* Physical device removal notifications are handled in bdev_nvme module's remove_cb */
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc;

	spdk_app_opts_init(&opts, sizeof(opts));
	opts.name = "mimo_tgt";
	if ((rc = spdk_app_parse_args(argc, argv, &opts, g_spdk_tgt_get_opts_string,
				      NULL, spdk_tgt_parse_arg, spdk_tgt_usage)) !=
	    SPDK_APP_PARSE_ARGS_SUCCESS) {
		return rc;
	}

	/* Set EC encoding workers configuration based on command line argument */
	if (g_disable_ec_encode_workers) {
		g_ec_encode_workers_enabled = false;
		SPDK_NOTICELOG("EC encoding dedicated worker threads: DISABLED (via -E)\n"
			       "  EC encoding will use app_thread instead of dedicated cores\n");
	} else {
		SPDK_NOTICELOG("EC encoding dedicated worker threads: ENABLED (default)\n");
	}

	rc = spdk_app_start(&opts, spdk_tgt_started, NULL);
	spdk_app_fini();

	return rc;
}
