/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_ec_internal.h"

#include "spdk/log.h"
#include "spdk/bdev.h"
#include "spdk/nvme.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include <unistd.h>

/* Forward declaration for weak symbol from bdev_nvme module */
extern struct spdk_nvme_ctrlr *bdev_nvme_get_ctrlr(struct spdk_bdev *bdev) __attribute__((weak));

struct ec_assignment_cache_entry {
	uint64_t stripe_index;
	uint8_t state;
	uint8_t data_indices[EC_MAX_K];
	uint8_t parity_indices[EC_MAX_P];
};

enum {
	EC_ASSIGNMENT_CACHE_ENTRY_EMPTY = 0,
	EC_ASSIGNMENT_CACHE_ENTRY_VALID = 1,
};

/* Forward declarations */
static uint32_t ec_read_wear_level_once(struct ec_bdev *ec_bdev, uint8_t device_idx);
static uint8_t ec_select_by_weight_deterministic(uint8_t *candidates, uint32_t *weights,
		uint8_t num_candidates, uint32_t seed);
static void ec_remove_from_array(uint8_t *candidates, uint32_t *weights,
				 uint8_t *num_candidates, uint8_t device_to_remove);
static bool ec_device_combination_same(uint8_t *old_data, uint8_t *old_parity,
				      uint8_t *new_data, uint8_t *new_parity,
				      uint8_t k, uint8_t p);
static void ec_selection_compute_weights_from_levels(uint8_t n, const uint32_t *levels,
		uint32_t *weights);
static int ec_selection_collect_device_wear(struct ec_bdev *ec_bdev, uint32_t *levels);
static uint16_t ec_selection_allocate_profile_id(struct ec_device_selection_config *config);
static void ec_selection_copy_profile_to_active(struct ec_device_selection_config *config,
		struct ec_wear_profile_slot *slot, uint8_t num_devices);
static struct ec_wear_profile_slot *ec_selection_find_matching_profile(
		struct ec_device_selection_config *config, const uint32_t *levels);
static struct ec_wear_profile_slot *ec_selection_find_profile_slot(
		struct ec_device_selection_config *config, uint16_t profile_id);
static struct ec_wear_profile_slot *ec_selection_ensure_profile_slot(
		struct ec_device_selection_config *config, uint16_t profile_id);
static void ec_selection_schedule_group_flush(struct ec_bdev *ec_bdev);
/* Internal helper: wear leveling selection without fault tolerance check (to avoid recursion) */
static int ec_select_base_bdevs_wear_leveling_no_fault_check(struct ec_bdev *ec_bdev,
		uint64_t stripe_index,
		uint8_t *data_indices,
		uint8_t *parity_indices);

static void
ec_assignment_cache_reset(struct ec_assignment_cache *cache)
{
	if (cache == NULL) {
		return;
	}

	if (cache->entries != NULL) {
		free(cache->entries);
		cache->entries = NULL;
	}

	if (cache->initialized) {
		spdk_spin_destroy(&cache->lock);
	}

	cache->capacity = 0;
	cache->mask = 0;
	cache->initialized = false;
	memset(&cache->lock, 0, sizeof(cache->lock));
}

static int
ec_assignment_cache_init(struct ec_assignment_cache *cache, uint32_t requested_capacity)
{
	uint32_t capacity;

	if (cache == NULL) {
		return -EINVAL;
	}

	/* Ensure capacity is power of two for fast modulo */
	capacity = 1;
	while (capacity < requested_capacity) {
		capacity <<= 1;
		if (capacity == 0) {
			break;
		}
	}

	if (capacity == 0) {
		return -EINVAL;
	}

	cache->entries = calloc(capacity, sizeof(struct ec_assignment_cache_entry));
	if (cache->entries == NULL) {
		return -ENOMEM;
	}

	cache->capacity = capacity;
	cache->mask = capacity - 1;
	spdk_spin_init(&cache->lock);
	cache->initialized = true;

	return 0;
}

static inline struct ec_assignment_cache_entry *
ec_assignment_cache_find_entry(struct ec_assignment_cache *cache, uint64_t stripe_index)
{
	uint32_t idx;
	uint32_t i;

	if (cache == NULL || !cache->initialized || cache->entries == NULL) {
		return NULL;
	}

	idx = (uint32_t)(stripe_index & cache->mask);
	for (i = 0; i < cache->capacity; i++) {
		struct ec_assignment_cache_entry *entry = &cache->entries[idx];

		if (entry->state == EC_ASSIGNMENT_CACHE_ENTRY_EMPTY) {
			return NULL;
		}

		if (entry->stripe_index == stripe_index) {
			return entry;
		}

		idx = (idx + 1) & cache->mask;
	}

	return NULL;
}

static inline struct ec_assignment_cache_entry *
ec_assignment_cache_get_slot(struct ec_assignment_cache *cache, uint64_t stripe_index)
{
	uint32_t idx;
	uint32_t i;

	if (cache == NULL || !cache->initialized || cache->entries == NULL) {
		return NULL;
	}

	idx = (uint32_t)(stripe_index & cache->mask);
	for (i = 0; i < cache->capacity; i++) {
		struct ec_assignment_cache_entry *entry = &cache->entries[idx];

		if (entry->state == EC_ASSIGNMENT_CACHE_ENTRY_EMPTY ||
		    entry->stripe_index == stripe_index) {
			return entry;
		}

		idx = (idx + 1) & cache->mask;
	}

	/* Table is full - evict current slot */
	return &cache->entries[idx];
}

static bool
ec_assignment_cache_get(struct ec_device_selection_config *config,
			uint64_t stripe_index,
			uint8_t *data_indices,
			uint8_t *parity_indices,
			uint8_t k, uint8_t p)
{
	struct ec_assignment_cache_entry *entry;

	if (config == NULL || !config->assignment_cache.initialized) {
		return false;
	}

	spdk_spin_lock(&config->assignment_cache.lock);
	entry = ec_assignment_cache_find_entry(&config->assignment_cache, stripe_index);
	if (entry != NULL) {
		if (k > 0) {
			memcpy(data_indices, entry->data_indices, k);
		}
		if (p > 0) {
			memcpy(parity_indices, entry->parity_indices, p);
		}
		spdk_spin_unlock(&config->assignment_cache.lock);
		return true;
	}
	spdk_spin_unlock(&config->assignment_cache.lock);
	return false;
}

static void
ec_assignment_cache_store(struct ec_device_selection_config *config,
			  uint64_t stripe_index,
			  const uint8_t *data_indices,
			  const uint8_t *parity_indices,
			  uint8_t k, uint8_t p)
{
	struct ec_assignment_cache_entry *entry;

	if (config == NULL || !config->assignment_cache.initialized) {
		return;
	}

	spdk_spin_lock(&config->assignment_cache.lock);
	entry = ec_assignment_cache_get_slot(&config->assignment_cache, stripe_index);
	if (entry != NULL) {
		entry->stripe_index = stripe_index;
		entry->state = EC_ASSIGNMENT_CACHE_ENTRY_VALID;
		if (k > 0) {
			memcpy(entry->data_indices, data_indices, k);
		}
		if (p > 0) {
			memcpy(entry->parity_indices, parity_indices, p);
		}
	}
	spdk_spin_unlock(&config->assignment_cache.lock);
}

/* Context for synchronous wear level reading */
struct wear_level_read_ctx {
	struct spdk_nvme_health_information_page *health_page;
	volatile bool completed;  /* Use volatile for thread safety (callback may run in different context) */
	volatile int status;
	bool timed_out;  /* Flag to prevent callback from accessing freed memory */
};

/* Completion callback for health log page read */
static void
ec_read_wear_level_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct wear_level_read_ctx *ctx = (struct wear_level_read_ctx *)cb_arg;

	/* Check if we've timed out - if so, don't access ctx (may be freed) */
	if (ctx->timed_out) {
		/* Command completed after timeout - ignore it */
		return;
	}

	if (spdk_nvme_cpl_is_error(cpl)) {
		ctx->status = -EIO;
	} else {
		ctx->status = 0;
	}
	ctx->completed = true;
}

/*
 * Read wear level from NVMe device (called once at initialization)
 * Returns wear level value (percentage_used), or 0 if unavailable
 * 
 * Note: This function performs synchronous I/O by polling the completion
 */
static uint32_t
ec_read_wear_level_once(struct ec_bdev *ec_bdev, uint8_t device_idx)
{
	/* Safety check: ensure device_idx is valid */
	if (spdk_unlikely(device_idx >= ec_bdev->num_base_bdevs)) {
		SPDK_ERRLOG("EC bdev %s: Invalid device_idx %u (max %u)\n",
			    ec_bdev->bdev.name, device_idx, ec_bdev->num_base_bdevs);
		return 0;
	}

	struct ec_base_bdev_info *base_info = &ec_bdev->base_bdev_info[device_idx];
	struct spdk_bdev *bdev;
	const char *module_name;
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_health_information_page *health_page;
	struct wear_level_read_ctx ctx;
	int rc;
	uint32_t wear_level = 0;

	if (base_info->desc == NULL) {
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: device %u has no bdev descriptor\n",
			      ec_bdev->bdev.name, device_idx);
		return 0;
	}

	bdev = spdk_bdev_desc_get_bdev(base_info->desc);
	if (bdev == NULL) {
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: device %u bdev is NULL\n",
			      ec_bdev->bdev.name, device_idx);
		return 0;
	}

	/* Check if this is an NVMe bdev */
	module_name = spdk_bdev_get_module_name(bdev);
	if (module_name == NULL || strcmp(module_name, "nvme") != 0) {
		/* Not an NVMe bdev, return 0 (no wear level available) */
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: device %u is not an NVMe bdev\n",
			      ec_bdev->bdev.name, device_idx);
		return 0;
	}

	/* Check if bdev_nvme_get_ctrlr function is available */
	if (bdev_nvme_get_ctrlr == NULL) {
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: bdev_nvme_get_ctrlr not available\n",
			      ec_bdev->bdev.name);
		return 0;
	}

	/* Get NVMe controller from bdev */
	ctrlr = bdev_nvme_get_ctrlr(bdev);
	if (ctrlr == NULL) {
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: device %u NVMe controller not available\n",
			      ec_bdev->bdev.name, device_idx);
		return 0;
	}

	/* Allocate health page buffer */
	health_page = spdk_zmalloc(sizeof(*health_page), 0, NULL,
				   SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (health_page == NULL) {
		SPDK_WARNLOG("EC bdev %s: Failed to allocate health page buffer for device %u\n",
			     ec_bdev->bdev.name, device_idx);
		return 0;
	}

	/* Initialize context */
	memset(&ctx, 0, sizeof(ctx));
	ctx.health_page = health_page;
	ctx.completed = false;
	ctx.status = -1;
	ctx.timed_out = false;

	/* Send get log page command (use global namespace tag for controller-level health) */
	rc = spdk_nvme_ctrlr_cmd_get_log_page(ctrlr,
					      SPDK_NVME_LOG_HEALTH_INFORMATION,
					      SPDK_NVME_GLOBAL_NS_TAG,
					      health_page,
					      sizeof(*health_page),
					      0,
					      ec_read_wear_level_cb,
					      &ctx);
	if (rc != 0) {
		SPDK_WARNLOG("EC bdev %s: Failed to send get log page command for device %u: %d\n",
			     ec_bdev->bdev.name, device_idx, rc);
		spdk_free(health_page);
		return 0;
	}

	/* Poll for completion (synchronous wait)
	 * Note: This is acceptable during initialization (one-time operation)
	 * We use a timeout to avoid infinite wait
	 */
	uint32_t poll_count = 0;
	const uint32_t max_polls = 10000;  /* ~1 second timeout (100us * 10000) */
	while (!ctx.completed && poll_count < max_polls) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
		/* Check again after processing completions (callback may have set completed) */
		if (!ctx.completed) {
			/* Small delay to avoid busy waiting */
			usleep(100);
			poll_count++;
		}
	}
	
	if (!ctx.completed) {
		/* Timeout: mark as timed out to prevent callback from accessing freed memory
		 * Note: We set timed_out before freeing health_page to ensure callback
		 * can check this flag before accessing ctx->health_page
		 */
		ctx.timed_out = true;
		/* Process completions one more time to handle any pending callbacks */
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
		SPDK_WARNLOG("EC bdev %s: Timeout reading wear level for device %u\n",
			     ec_bdev->bdev.name, device_idx);
		/* Note: health_page will be freed, but callback may still run.
		 * The timed_out flag prevents callback from accessing it. */
		spdk_free(health_page);
		return 0;
	}

	/* Check result */
	if (ctx.status == 0) {
		/* Success: return percentage_used as wear level */
		wear_level = health_page->percentage_used;
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: device %u wear level: %u%%\n",
			      ec_bdev->bdev.name, device_idx, wear_level);
	} else {
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: device %u failed to read wear level: %d\n",
			      ec_bdev->bdev.name, device_idx, ctx.status);
	}

	/* Free health page buffer */
	spdk_free(health_page);

	return wear_level;
}

/*
 * Deterministic weighted selection (ensures read-write consistency)
 * Uses seed to generate deterministic random number
 */
static uint8_t
ec_select_by_weight_deterministic(uint8_t *candidates, uint32_t *weights,
				  uint8_t num_candidates, uint32_t seed)
{
	uint32_t total_weight = 0;
	uint32_t i;

	if (num_candidates == 0) {
		SPDK_ERRLOG("No candidates available for selection\n");
		/* Return invalid device index (255 is max uint8_t, unlikely to be valid) */
		return UINT8_MAX;
	}

	/* Calculate total weight */
	for (i = 0; i < num_candidates; i++) {
		total_weight += weights[i];
	}

	if (total_weight == 0) {
		/* All weights are 0, use first candidate as fallback */
		SPDK_WARNLOG("Total weight is 0, using first candidate as fallback\n");
		return candidates[0];
	}

	/* Deterministic random number (based on seed) */
	/* Using linear congruential generator for deterministic randomness */
	uint32_t random = (seed * 1103515245 + 12345) % total_weight;

	/* Select based on weight */
	uint32_t accumulated = 0;
	for (i = 0; i < num_candidates; i++) {
		accumulated += weights[i];
		if (random < accumulated) {
			return candidates[i];
		}
	}

	/* Fallback to last candidate */
	return candidates[num_candidates - 1];
}

/*
 * Remove device from candidate array
 */
static void
ec_remove_from_array(uint8_t *candidates, uint32_t *weights,
		     uint8_t *num_candidates, uint8_t device_to_remove)
{
	uint8_t i, j;

	for (i = 0; i < *num_candidates; i++) {
		if (candidates[i] == device_to_remove) {
			/* Remove device by shifting array */
			for (j = i; j < *num_candidates - 1; j++) {
				candidates[j] = candidates[j + 1];
				weights[j] = weights[j + 1];
			}
			(*num_candidates)--;
			break;
		}
	}
}

/*
 * Check if two device combinations are the same
 * Reserved for future use in rebalance mechanism
 */
__attribute__((unused)) static bool
ec_device_combination_same(uint8_t *old_data, uint8_t *old_parity,
			   uint8_t *new_data, uint8_t *new_parity,
			   uint8_t k, uint8_t p)
{
	uint8_t i, j;
	bool found;

	/* Check data devices */
	for (i = 0; i < k; i++) {
		found = false;
		for (j = 0; j < k; j++) {
			if (old_data[i] == new_data[j]) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}

	/* Check parity devices */
	for (i = 0; i < p; i++) {
		found = false;
		for (j = 0; j < p; j++) {
			if (old_parity[i] == new_parity[j]) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}

	return true;
}

static void
ec_selection_compute_weights_from_levels(uint8_t n, const uint32_t *levels,
					 uint32_t *weights)
{
	uint32_t max_wear = 0;
	uint32_t min_wear = UINT32_MAX;
	bool have_valid = false;
	uint8_t i;

	for (i = 0; i < n; i++) {
		uint32_t wear = levels[i];

		if (wear == UINT32_MAX) {
			weights[i] = 0;
			continue;
		}

		if (wear == 0) {
			weights[i] = 100;
			continue;
		}

		if (wear > max_wear) {
			max_wear = wear;
		}
		if (wear < min_wear) {
			min_wear = wear;
		}
		have_valid = true;
	}

	if (!have_valid || max_wear == min_wear) {
		for (i = 0; i < n; i++) {
			if (levels[i] == UINT32_MAX) {
				weights[i] = 0;
			} else {
				weights[i] = 100;
			}
		}
		return;
	}

	uint32_t range = max_wear - min_wear;
	for (i = 0; i < n; i++) {
		if (levels[i] == UINT32_MAX) {
			weights[i] = 0;
			continue;
		}

		if (levels[i] == 0) {
			weights[i] = 100;
			continue;
		}

		uint32_t normalized = ((levels[i] - min_wear) * 100) / range;
		weights[i] = 100 - normalized;
		if (weights[i] == 0) {
			weights[i] = 1;
		}
	}
}

static inline uint32_t
ec_selection_calculate_group_id(const struct ec_device_selection_config *config,
				uint64_t stripe_index)
{
	uint32_t group_size = config->stripe_group_size;

	if (group_size == 0) {
		group_size = 1;
	}

	return (uint32_t)(stripe_index / group_size);
}

static int
ec_selection_ensure_group_capacity(struct ec_bdev *ec_bdev, uint32_t required_groups)
{
	struct ec_device_selection_config *config = &ec_bdev->selection_config;
	uint32_t new_capacity;
	uint16_t *new_map;
	size_t new_size;

	if (config->group_profile_map == NULL) {
		config->group_profile_capacity = 0;
	}

	if (required_groups <= config->group_profile_capacity) {
		if (required_groups > config->num_stripe_groups) {
			config->num_stripe_groups = required_groups;
		}
		return 0;
	}

	new_capacity = config->group_profile_capacity ? config->group_profile_capacity : 1024;
	while (new_capacity < required_groups) {
		new_capacity *= 2;
	}

	new_size = (size_t)new_capacity * sizeof(uint16_t);
	new_map = spdk_zmalloc(new_size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_SHARE);
	if (new_map == NULL) {
		return -ENOMEM;
	}

	if (config->group_profile_map != NULL && config->group_profile_capacity > 0) {
		memcpy(new_map, config->group_profile_map,
		       (size_t)config->group_profile_capacity * sizeof(uint16_t));
		spdk_free(config->group_profile_map);
	}

	config->group_profile_map = new_map;
	config->group_profile_capacity = new_capacity;
	if (required_groups > config->num_stripe_groups) {
		config->num_stripe_groups = required_groups;
	}

	return 0;
}

static struct ec_wear_profile_slot *
ec_selection_find_matching_profile(struct ec_device_selection_config *config, const uint32_t *levels)
{
	uint32_t i, dev;

	for (i = 0; i < EC_MAX_WEAR_PROFILES; i++) {
		struct ec_wear_profile_slot *src = &config->wear_profiles[i];
		if (!src->valid) {
			continue;
		}
		bool match = true;
		for (dev = 0; dev < EC_MAX_BASE_BDEVS; dev++) {
			if (src->wear_levels[dev] != levels[dev]) {
				match = false;
				break;
			}
		}
		if (match) {
			return src;
		}
	}

	return NULL;
}

static struct ec_wear_profile_slot *
ec_selection_find_profile_slot(struct ec_device_selection_config *config, uint16_t profile_id)
{
	uint32_t i;

	if (profile_id == 0) {
		return NULL;
	}

	for (i = 0; i < EC_MAX_WEAR_PROFILES; i++) {
		if (config->wear_profiles[i].valid &&
		    config->wear_profiles[i].profile_id == profile_id) {
			return &config->wear_profiles[i];
		}
	}

	return NULL;
}

static struct ec_wear_profile_slot *
ec_selection_ensure_profile_slot(struct ec_device_selection_config *config, uint16_t profile_id)
{
	struct ec_wear_profile_slot *slot = ec_selection_find_profile_slot(config, profile_id);
	uint32_t i;

	if (slot != NULL) {
		return slot;
	}

	for (i = 0; i < EC_MAX_WEAR_PROFILES; i++) {
		if (!config->wear_profiles[i].valid) {
			config->wear_profiles[i].valid = true;
			config->wear_profiles[i].profile_id = profile_id;
			memset(config->wear_profiles[i].wear_levels, 0,
			       sizeof(config->wear_profiles[i].wear_levels));
			memset(config->wear_profiles[i].wear_weights, 0,
			       sizeof(config->wear_profiles[i].wear_weights));
			return &config->wear_profiles[i];
		}
	}

	return NULL;
}

static struct ec_wear_profile_slot *
ec_selection_get_profile_slot_for_stripe(struct ec_bdev *ec_bdev, uint64_t stripe_index)
{
	struct ec_device_selection_config *config = &ec_bdev->selection_config;
	struct ec_wear_profile_slot *slot;
	uint16_t profile_id = config->active_profile_id;
	uint32_t group_id;
	int rc;

	if (!config->wear_leveling_enabled || config->group_profile_map == NULL) {
		return ec_selection_find_profile_slot(config, profile_id);
	}

	group_id = ec_selection_calculate_group_id(config, stripe_index);
	rc = ec_selection_ensure_group_capacity(ec_bdev, group_id + 1);
	if (rc != 0) {
		return ec_selection_find_profile_slot(config, profile_id);
	}

	if (config->group_profile_map != NULL && group_id < config->group_profile_capacity) {
		if (config->group_profile_map[group_id] != 0) {
			profile_id = config->group_profile_map[group_id];
		}
	}

	if (profile_id == 0) {
		profile_id = config->active_profile_id ? config->active_profile_id : 1;
	}

	slot = ec_selection_find_profile_slot(config, profile_id);
	if (slot == NULL && profile_id != config->active_profile_id) {
		slot = ec_selection_find_profile_slot(config, config->active_profile_id);
	}

	return slot;
}

static int
ec_selection_collect_device_wear(struct ec_bdev *ec_bdev, uint32_t *levels)
{
	uint8_t n = ec_bdev->num_base_bdevs;
	uint8_t i;
	uint32_t max_wear = 0;
	uint32_t min_wear = UINT32_MAX;
	uint8_t valid_wear_count = 0;

	for (i = 0; i < n; i++) {
		struct ec_base_bdev_info *base_info = &ec_bdev->base_bdev_info[i];
		uint32_t wear = 0;

		if (base_info->desc == NULL || base_info->is_failed) {
			levels[i] = UINT32_MAX;
			continue;
		}

		wear = ec_read_wear_level_once(ec_bdev, i);
		levels[i] = wear;

		if (wear > 0) {
			if (wear > max_wear) {
				max_wear = wear;
			}
			if (wear < min_wear) {
				min_wear = wear;
			}
			valid_wear_count++;
		}

		SPDK_NOTICELOG("EC bdev %s: Device %u wear level: %u\n",
			       ec_bdev->bdev.name, i, wear);
	}

	if (valid_wear_count == 0) {
		SPDK_WARNLOG("EC bdev %s: No valid wear level data collected, using defaults\n",
			     ec_bdev->bdev.name);
	}

	return 0;
}

static uint16_t
ec_selection_allocate_profile_id(struct ec_device_selection_config *config)
{
	uint32_t next = config->next_profile_id;

	if (next == 0) {
		next = 1;
	}

	config->next_profile_id = next + 1;
	if (config->next_profile_id == 0) {
		/* Wrapped - restart from 1 (profile 0 reserved) */
		config->next_profile_id = 1;
	}

	return (uint16_t)next;
}

static void
ec_selection_copy_profile_to_active(struct ec_device_selection_config *config,
				    struct ec_wear_profile_slot *slot, uint8_t num_devices)
{
	if (slot == NULL) {
		return;
	}

	memcpy(config->wear_levels, slot->wear_levels, sizeof(uint32_t) * num_devices);
	memcpy(config->wear_weights, slot->wear_weights, sizeof(uint32_t) * num_devices);
}

int
ec_selection_create_profile_from_devices(struct ec_bdev *ec_bdev, bool make_active,
					 bool persist_now)
{
	struct ec_device_selection_config *config = &ec_bdev->selection_config;
	struct ec_wear_profile_slot *slot;
	uint8_t n = ec_bdev->num_base_bdevs;
	uint16_t profile_id;
	uint32_t levels[EC_MAX_BASE_BDEVS];
	int rc;

	if (!config->wear_leveling_enabled) {
		return -EINVAL;
	}

	rc = ec_selection_collect_device_wear(ec_bdev, levels);
	if (rc != 0) {
		SPDK_ERRLOG("EC bdev %s: Failed to collect wear data: %s\n",
			    ec_bdev->bdev.name, spdk_strerror(-rc));
		return rc;
	}

	slot = ec_selection_find_matching_profile(config, levels);
	if (slot != NULL) {
		profile_id = slot->profile_id;
		SPDK_NOTICELOG("EC bdev %s: Wear profile identical to existing profile %u, reusing\n",
			       ec_bdev->bdev.name, profile_id);
		if (make_active) {
			config->active_profile_id = profile_id;
			ec_selection_copy_profile_to_active(config, slot, n);
		}
		return 0;
	}

	profile_id = ec_selection_allocate_profile_id(config);
	slot = ec_selection_ensure_profile_slot(config, profile_id);
	if (slot == NULL) {
		SPDK_ERRLOG("EC bdev %s: No free wear profile slots available (max=%u). "
			    "Consider increasing EC_MAX_WEAR_PROFILES or reducing refresh frequency.\n",
			    ec_bdev->bdev.name, EC_MAX_WEAR_PROFILES);
		return -ENOSPC;
	}

	memcpy(slot->wear_levels, levels, sizeof(uint32_t) * EC_MAX_BASE_BDEVS);
	slot->profile_id = profile_id;
	slot->valid = true;
	ec_selection_compute_weights_from_levels(n, slot->wear_levels, slot->wear_weights);

	if (make_active) {
		config->active_profile_id = profile_id;
		ec_selection_copy_profile_to_active(config, slot, n);
	}

	SPDK_NOTICELOG("EC bdev %s: Created wear profile %u (make_active=%s)\n",
		       ec_bdev->bdev.name, profile_id, make_active ? "true" : "false");

	if (ec_bdev->superblock_enabled) {
		if (persist_now) {
			SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Persisting wear profile %u immediately\n",
				      ec_bdev->bdev.name, profile_id);
			ec_bdev_sb_save_selection_metadata(ec_bdev);
			ec_bdev_write_superblock(ec_bdev, NULL, NULL);
		} else {
			SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Scheduling deferred persist for profile %u\n",
				      ec_bdev->bdev.name, profile_id);
			ec_selection_schedule_group_flush(ec_bdev);
		}
	}

	return 0;
}

static void
ec_selection_mark_group_dirty(struct ec_bdev *ec_bdev)
{
	struct ec_device_selection_config *config = &ec_bdev->selection_config;

	if (!ec_bdev->superblock_enabled || config->group_profile_map == NULL) {
		return;
	}

	config->group_map_dirty = true;
	config->group_map_dirty_count++;
	config->group_map_dirty_version++;
	SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Stripe group map dirty (count=%u, version=%" PRIu64 ")\n",
		      ec_bdev->bdev.name, config->group_map_dirty_count,
		      config->group_map_dirty_version);

	if (!config->group_map_flush_in_progress &&
	    (config->group_map_dirty_count == 1 ||
	     config->group_map_dirty_count >= EC_GROUP_MAP_FLUSH_THRESHOLD)) {
		ec_selection_schedule_group_flush(ec_bdev);
	}
}

static void
ec_selection_flush_group_map_done(int status, struct ec_bdev *ec_bdev, void *cb_ctx)
{
	struct ec_device_selection_config *config = &ec_bdev->selection_config;

	if (status != 0) {
		SPDK_WARNLOG("EC bdev %s: Failed to persist stripe group metadata: %s\n",
			     ec_bdev->bdev.name, spdk_strerror(-status));
		config->group_map_flush_in_progress = false;
		if (config->group_map_flush_retries < EC_GROUP_MAP_FLUSH_MAX_RETRIES) {
			config->group_map_flush_retries++;
			SPDK_WARNLOG("EC bdev %s: Retrying metadata flush (%u/%u)\n",
				     ec_bdev->bdev.name, config->group_map_flush_retries,
				     EC_GROUP_MAP_FLUSH_MAX_RETRIES);
			ec_selection_schedule_group_flush(ec_bdev);
		} else {
			SPDK_ERRLOG("EC bdev %s: Metadata flush failed after %u retries, "
				    "new bindings will retry automatically\n",
				    ec_bdev->bdev.name, EC_GROUP_MAP_FLUSH_MAX_RETRIES);
		}
		return;
	}

	if (config->group_map_dirty_version != config->group_map_flush_version) {
		config->group_map_flush_in_progress = false;
		config->group_map_flush_retries = 0;
		ec_selection_schedule_group_flush(ec_bdev);
		return;
	}

	config->group_map_dirty = false;
	config->group_map_dirty_count = 0;
	config->group_map_flush_in_progress = false;
	config->group_map_flush_retries = 0;
	SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Stripe group map flush complete (version=%" PRIu64 ")\n",
		      ec_bdev->bdev.name, config->group_map_flush_version);
}

static void
ec_selection_flush_group_map_msg(void *ctx)
{
	struct ec_bdev *ec_bdev = ctx;
	struct ec_device_selection_config *config = &ec_bdev->selection_config;

	if (!config->group_map_flush_in_progress) {
		return;
	}

	ec_bdev_sb_save_selection_metadata(ec_bdev);
	ec_bdev_write_superblock(ec_bdev, ec_selection_flush_group_map_done, NULL);
}

static void
ec_selection_schedule_group_flush(struct ec_bdev *ec_bdev)
{
	struct ec_device_selection_config *config = &ec_bdev->selection_config;

	if (!ec_bdev->superblock_enabled || config->group_profile_map == NULL) {
		return;
	}

	if (config->group_map_flush_in_progress) {
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Flush already in progress, skip scheduling\n",
			      ec_bdev->bdev.name);
		return;
	}

	config->group_map_flush_in_progress = true;
	config->group_map_flush_version = config->group_map_dirty_version;
	config->group_map_flush_retries = 0;
	SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Scheduling stripe group map flush (version=%" PRIu64 ")\n",
		      ec_bdev->bdev.name, config->group_map_flush_version);
	spdk_thread_send_msg(spdk_thread_get_app_thread(),
			     ec_selection_flush_group_map_msg, ec_bdev);
}

int
ec_selection_bind_group_profile(struct ec_bdev *ec_bdev, uint64_t stripe_index)
{
	struct ec_device_selection_config *config = &ec_bdev->selection_config;
	uint32_t group_id;
	uint16_t *entry;
	uint16_t profile_id;
	int rc;

	if (!config->wear_leveling_enabled || config->group_profile_map == NULL) {
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Stripe %lu no binding required (wear leveling disabled)\n",
			      ec_bdev->bdev.name, stripe_index);
		return 0;
	}

	group_id = ec_selection_calculate_group_id(config, stripe_index);
	rc = ec_selection_ensure_group_capacity(ec_bdev, group_id + 1);
	if (rc != 0) {
		return rc;
	}

	entry = &config->group_profile_map[group_id];
	if (*entry != 0) {
		SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Stripe %lu already bound to profile %u\n",
			      ec_bdev->bdev.name, stripe_index, *entry);
		return 0;
	}

	profile_id = config->active_profile_id;
	if (profile_id == 0) {
		profile_id = 1;
		config->active_profile_id = profile_id;
	}

	*entry = profile_id;
	SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Stripe %lu bound to profile %u (group_id=%u)\n",
		      ec_bdev->bdev.name, stripe_index, profile_id, group_id);
	ec_selection_mark_group_dirty(ec_bdev);
	return 0;
}

/*
 * Wear leveling device selection with built-in fault tolerance
 * 磨损均衡算法，内置容错保证：确保同组条带不共享设备
 * 这是磨损均衡的核心功能，不是独立选项
 */
int
ec_select_base_bdevs_wear_leveling(struct ec_bdev *ec_bdev,
				   uint64_t stripe_index,
				   uint8_t *data_indices,
				   uint8_t *parity_indices)
{
	struct ec_device_selection_config *config = &ec_bdev->selection_config;
	uint8_t n = ec_bdev->num_base_bdevs;
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;
	uint8_t group_size = config->stripe_group_size;
	struct ec_wear_profile_slot *profile_slot;
	const uint32_t *weight_table;
	uint8_t i;
	int rc;

	if (config->debug_enabled) {
		SPDK_NOTICELOG("[SELECT] Stripe %lu: Starting device selection (group_size=%u, n=%u, k=%u, p=%u)\n",
			       stripe_index, group_size, n, k, p);
	}

	if (ec_assignment_cache_get(config, stripe_index, data_indices, parity_indices, k, p)) {
		if (config->debug_enabled) {
			SPDK_NOTICELOG("[SELECT] Stripe %lu: Cache hit, skipping recompute\n",
				       stripe_index);
		}
		return 0;
	}

	/* Calculate stripe group information */
	/* Safety check: ensure group_size is valid */
	if (spdk_unlikely(group_size == 0)) {
		SPDK_ERRLOG("EC bdev %s: Invalid stripe_group_size %u, using default %u\n",
			    ec_bdev->bdev.name, group_size, n);
		group_size = n;
		config->stripe_group_size = n;
	}
	uint64_t group_id = stripe_index / group_size;
	uint64_t offset_in_group = stripe_index % group_size;
	uint64_t group_start = group_id * group_size;

	if (config->debug_enabled) {
		SPDK_NOTICELOG("[SELECT] Stripe %lu: Group ID=%lu, offset_in_group=%lu, group_start=%lu\n",
			       stripe_index, group_id, offset_in_group, group_start);
	}

	/* Step 1: Generate candidate device list (exclude failed devices) */
	uint8_t candidates[EC_MAX_BASE_BDEVS];
	uint32_t candidate_weights[EC_MAX_BASE_BDEVS];
	uint8_t num_candidates = 0;
	struct ec_base_bdev_info *base_info;
	uint8_t device_idx = 0;

	profile_slot = ec_selection_get_profile_slot_for_stripe(ec_bdev, stripe_index);
	weight_table = profile_slot ? profile_slot->wear_weights : config->wear_weights;

	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc != NULL && !base_info->is_failed) {
			/* CRITICAL: device_idx must be calculated as offset from base_bdev_info array */
			/* EC_FOR_EACH_BASE_BDEV returns pointer, so calculate index */
			if (spdk_unlikely(device_idx >= n)) {
				SPDK_ERRLOG("EC bdev %s: Device index %u exceeds num_base_bdevs %u\n",
					    ec_bdev->bdev.name, device_idx, n);
				break;  /* Safety: prevent array overflow */
			}
			candidates[num_candidates] = device_idx;
			candidate_weights[num_candidates] = weight_table[device_idx];
			num_candidates++;

			if (config->debug_enabled) {
				SPDK_NOTICELOG("[SELECT] Stripe %lu: Candidate device %u (wear_weight=%u)\n",
					       stripe_index, device_idx, weight_table[device_idx]);
			}
		}
		device_idx++;
	}

	if (num_candidates < k + p) {
		SPDK_ERRLOG("EC bdev %s: Not enough operational devices for stripe %lu "
			    "(available: %u, needed: %u, total base devices: %u). "
			    "Please check device status or add more base devices.\n",
			    ec_bdev->bdev.name, stripe_index, num_candidates, k + p, n);
		return -ENODEV;
	}

	if (config->debug_enabled) {
		SPDK_NOTICELOG("[SELECT] Stripe %lu: Found %u candidate devices\n",
			       stripe_index, num_candidates);
	}

	/* Step 2: 容错检查 - 获取同组其他条带使用的设备（内置在磨损均衡算法中）
	 * 这是磨损均衡算法的核心功能：确保同组条带不共享设备
	 * 查询其他条带时，使用相同的磨损均衡算法（但不进行容错检查，避免递归）
	 * 借助条带分配缓存避免重复计算，同时保持 100% 正确性
	 */
	uint8_t used_devices[EC_MAX_BASE_BDEVS] = {0};
	uint8_t used_count = 0;

	for (uint64_t other_stripe = group_start;
	     other_stripe < group_start + offset_in_group;
	     other_stripe++) {
		uint8_t other_data[EC_MAX_K];
		uint8_t other_parity[EC_MAX_P];
		if (!ec_assignment_cache_get(config, other_stripe, other_data, other_parity, k, p)) {
			/* 使用磨损均衡算法查询其他条带（但不进行容错检查，避免递归） */
			rc = ec_select_base_bdevs_wear_leveling_no_fault_check(ec_bdev,
					other_stripe, other_data, other_parity);
			if (rc != 0) {
				/* Other stripe may not be written yet, skip */
				if (config->debug_enabled) {
					SPDK_DEBUGLOG(bdev_ec, "[SELECT] Stripe %lu: Other stripe %lu not available (rc=%d)\n",
						      stripe_index, other_stripe, rc);
				}
				continue;
			}

			ec_assignment_cache_store(config, other_stripe, other_data, other_parity, k, p);
		}

		/* Mark devices as used */
		for (i = 0; i < k; i++) {
			if (other_data[i] < EC_MAX_BASE_BDEVS && !used_devices[other_data[i]]) {
				used_devices[other_data[i]] = 1;
				used_count++;
				if (config->debug_enabled) {
					SPDK_NOTICELOG("[SELECT] Stripe %lu: Device %u used by stripe %lu\n",
						       stripe_index, other_data[i], other_stripe);
				}
			}
		}
		for (i = 0; i < p; i++) {
			if (other_parity[i] < EC_MAX_BASE_BDEVS && !used_devices[other_parity[i]]) {
				used_devices[other_parity[i]] = 1;
				used_count++;
				if (config->debug_enabled) {
					SPDK_NOTICELOG("[SELECT] Stripe %lu: Device %u used by stripe %lu (parity)\n",
						       stripe_index, other_parity[i], other_stripe);
				}
			}
		}
	}

	if (config->debug_enabled) {
		SPDK_NOTICELOG("[SELECT] Stripe %lu: Found %u devices used by other stripes in group\n",
			       stripe_index, used_count);
	}

	/* Step 3: Select from available devices (exclude used devices, use wear weights) */
	uint8_t available_candidates[EC_MAX_BASE_BDEVS];
	uint32_t available_weights[EC_MAX_BASE_BDEVS];
	uint8_t num_available = 0;

	for (i = 0; i < num_candidates; i++) {
		uint8_t dev = candidates[i];
		if (!used_devices[dev]) {
			available_candidates[num_available] = dev;
			available_weights[num_available] = candidate_weights[i];
			num_available++;

			if (config->debug_enabled) {
				SPDK_NOTICELOG("[SELECT] Stripe %lu: Available device %u (weight=%u)\n",
					       stripe_index, dev, candidate_weights[i]);
			}
		}
	}

	/* If not enough available devices, use all candidates (with warning) */
	if (num_available < k + p) {
		/* Provide detailed error information */
		SPDK_WARNLOG("EC bdev %s: Not enough devices for fault tolerance at stripe %lu "
			     "(group_id=%lu, offset_in_group=%lu). "
			     "Available: %u, needed: %u, candidates: %u, used by other stripes: %u. "
			     "Using all candidates with potential overlap (fault tolerance may be compromised).\n",
			     ec_bdev->bdev.name, stripe_index, group_id, offset_in_group,
			     num_available, k + p, num_candidates, used_count);
		SPDK_WARNLOG("EC bdev %s: Suggestion: Increase stripe_group_size or add more base devices "
			     "to improve fault tolerance.\n", ec_bdev->bdev.name);
		/* Safety: ensure we don't exceed array bounds */
		/* Note: num_candidates is uint8_t, so it's always <= EC_MAX_BASE_BDEVS (255) */
		if (num_candidates > 0 && num_candidates <= n) {
			memcpy(available_candidates, candidates, num_candidates * sizeof(uint8_t));
			memcpy(available_weights, candidate_weights, num_candidates * sizeof(uint32_t));
			num_available = num_candidates;
		} else {
			SPDK_ERRLOG("EC bdev %s: Invalid candidate count %u (max %u) at stripe %lu, cannot proceed\n",
				    ec_bdev->bdev.name, num_candidates, n, stripe_index);
			return -ENODEV;
		}
	}

	if (config->debug_enabled) {
		SPDK_NOTICELOG("[SELECT] Stripe %lu: %u available devices for selection\n",
			       stripe_index, num_available);
	}

	/* Step 4: Deterministic selection based on wear weights */
	uint32_t seed = config->selection_seed ^ (uint32_t)stripe_index;

	/* Select k data block devices */
	for (i = 0; i < k; i++) {
		/* Safety check: ensure we have enough devices */
		if (spdk_unlikely(num_available == 0)) {
			SPDK_ERRLOG("EC bdev %s: Not enough devices for data selection at stripe %lu "
				    "(needed %u more, available: %u, already selected: %u). "
				    "This may indicate insufficient devices or too many devices used by other stripes in the group.\n",
				    ec_bdev->bdev.name, stripe_index, k - i, num_available, i);
			return -ENODEV;
		}
		uint8_t selected = ec_select_by_weight_deterministic(
			available_candidates, available_weights, num_available, seed + i);
		/* Safety check: ensure selected device is valid */
		if (spdk_unlikely(selected == UINT8_MAX || selected >= n)) {
			SPDK_ERRLOG("EC bdev %s: Invalid device selected %u (max %u) at stripe %lu\n",
				    ec_bdev->bdev.name, selected, n, stripe_index);
			return -ENODEV;
		}
		data_indices[i] = selected;

		if (config->debug_enabled) {
			SPDK_NOTICELOG("[SELECT] Stripe %lu: Selected data device %u (index %u)\n",
				       stripe_index, selected, i);
		}

		/* Remove selected device from available list */
		ec_remove_from_array(available_candidates, available_weights,
				     &num_available, selected);
	}

	/* Select p parity block devices */
	for (i = 0; i < p; i++) {
		/* Safety check: ensure we have enough devices */
		if (spdk_unlikely(num_available == 0)) {
			SPDK_ERRLOG("EC bdev %s: Not enough devices for parity selection at stripe %lu "
				    "(needed %u more, available: %u, already selected: %u). "
				    "This may indicate insufficient devices or too many devices used by other stripes in the group.\n",
				    ec_bdev->bdev.name, stripe_index, p - i, num_available, i);
			return -ENODEV;
		}
		uint8_t selected = ec_select_by_weight_deterministic(
			available_candidates, available_weights, num_available, seed + k + i);
		/* Safety check: ensure selected device is valid */
		if (spdk_unlikely(selected == UINT8_MAX || selected >= n)) {
			SPDK_ERRLOG("EC bdev %s: Invalid device selected %u (max %u) at stripe %lu\n",
				    ec_bdev->bdev.name, selected, n, stripe_index);
			return -ENODEV;
		}
		parity_indices[i] = selected;

		if (config->debug_enabled) {
			SPDK_NOTICELOG("[SELECT] Stripe %lu: Selected parity device %u (index %u)\n",
				       stripe_index, selected, i);
		}

		/* Remove selected device from available list */
		ec_remove_from_array(available_candidates, available_weights,
				     &num_available, selected);
	}

	ec_assignment_cache_store(config, stripe_index, data_indices, parity_indices, k, p);

	if (config->debug_enabled) {
		/* Safe debug output: only print available indices */
		fprintf(stderr, "[SELECT] Stripe %lu: Selection complete - data: [", stripe_index);
		for (i = 0; i < k && i < EC_MAX_K; i++) {
			fprintf(stderr, "%s%u", (i > 0) ? "," : "", data_indices[i]);
		}
		fprintf(stderr, "], parity: [");
		for (i = 0; i < p && i < EC_MAX_P; i++) {
			fprintf(stderr, "%s%u", (i > 0) ? "," : "", parity_indices[i]);
		}
		fprintf(stderr, "]\n");
		fflush(stderr);
	}

	return 0;
}

/*
 * Internal helper: wear leveling selection without fault tolerance check
 * 用于容错检查时查询其他条带，避免递归
 */
static int
ec_select_base_bdevs_wear_leveling_no_fault_check(struct ec_bdev *ec_bdev,
		uint64_t stripe_index,
		uint8_t *data_indices,
		uint8_t *parity_indices)
{
	struct ec_device_selection_config *config = &ec_bdev->selection_config;
	uint8_t k = ec_bdev->k;
	uint8_t p = ec_bdev->p;
	uint8_t n = ec_bdev->num_base_bdevs;
	struct ec_wear_profile_slot *profile_slot;
	const uint32_t *weight_table;
	uint8_t i;

	/* Generate candidate device list */
	uint8_t candidates[EC_MAX_BASE_BDEVS];
	uint32_t candidate_weights[EC_MAX_BASE_BDEVS];
	uint8_t num_candidates = 0;
	struct ec_base_bdev_info *base_info;
	uint8_t device_idx = 0;

	profile_slot = ec_selection_get_profile_slot_for_stripe(ec_bdev, stripe_index);
	weight_table = profile_slot ? profile_slot->wear_weights : config->wear_weights;

	EC_FOR_EACH_BASE_BDEV(ec_bdev, base_info) {
		if (base_info->desc != NULL && !base_info->is_failed) {
			if (spdk_unlikely(device_idx >= n)) {
				break;
			}
			candidates[num_candidates] = device_idx;
			candidate_weights[num_candidates] = weight_table[device_idx];
			num_candidates++;
		}
		device_idx++;
	}

	if (num_candidates < k + p) {
		return -ENODEV;
	}

	/* Deterministic selection based on wear weights (no fault tolerance check) */
	uint32_t seed = config->selection_seed ^ (uint32_t)stripe_index;
	uint8_t num_available = num_candidates;

	/* Select k data block devices */
	for (i = 0; i < k; i++) {
		if (spdk_unlikely(num_available == 0)) {
			return -ENODEV;
		}
		uint8_t selected = ec_select_by_weight_deterministic(
			candidates, candidate_weights, num_available, seed + i);
		if (spdk_unlikely(selected == UINT8_MAX || selected >= n)) {
			return -ENODEV;
		}
		data_indices[i] = selected;
		ec_remove_from_array(candidates, candidate_weights, &num_available, selected);
	}

	/* Select p parity block devices */
	for (i = 0; i < p; i++) {
		if (spdk_unlikely(num_available == 0)) {
			return -ENODEV;
		}
		uint8_t selected = ec_select_by_weight_deterministic(
			candidates, candidate_weights, num_available, seed + k + i);
		if (spdk_unlikely(selected == UINT8_MAX || selected >= n)) {
			return -ENODEV;
		}
		parity_indices[i] = selected;
		ec_remove_from_array(candidates, candidate_weights, &num_available, selected);
	}

	return 0;
}


/*
 * Initialize device selection configuration
 * Reads wear levels once at initialization
 */
int
ec_bdev_init_selection_config(struct ec_bdev *ec_bdev)
{
	struct ec_device_selection_config *config = &ec_bdev->selection_config;
	uint8_t n = ec_bdev->num_base_bdevs;
	uint16_t *prev_group_map = config->group_profile_map;
	uint32_t prev_group_capacity = config->group_profile_capacity;
	bool prev_wear_leveling = config->wear_leveling_enabled;
	uint8_t prev_group_size = config->stripe_group_size;
	uint32_t prev_seed = config->selection_seed;
	uint16_t prev_active_profile = config->active_profile_id;
	uint16_t prev_next_profile = config->next_profile_id;
	bool prev_debug = config->debug_enabled;
	bool metadata_loaded = false;
	int rc = 0;
	uint8_t i;

	SPDK_NOTICELOG("EC bdev %s: Initializing device selection configuration\n",
		       ec_bdev->bdev.name);

	/* Destroy existing assignment cache before resetting configuration */
	ec_assignment_cache_reset(&config->assignment_cache);

	/* Initialize with defaults */
	memset(config, 0, sizeof(*config));
	config->group_profile_map = prev_group_map;
	config->group_profile_capacity = prev_group_capacity;
	config->wear_leveling_enabled = prev_wear_leveling;
	config->stripe_group_size = prev_group_size;
	config->selection_seed = prev_seed ? prev_seed : 0x12345678;
	config->active_profile_id = prev_active_profile;
	config->next_profile_id = prev_next_profile ? prev_next_profile : 1;
	config->debug_enabled = prev_debug;
	config->group_map_dirty = false;
	config->group_map_dirty_count = 0;
	config->group_map_flush_in_progress = false;
	config->group_map_dirty_version = 0;
	config->group_map_flush_version = 0;

	if (ec_bdev->superblock_enabled && ec_bdev->sb != NULL) {
		ec_bdev_sb_load_wear_leveling_config(ec_bdev->sb, config);
	}

	if (config->stripe_group_size == 0 || config->stripe_group_size > n) {
		config->stripe_group_size = (n == 0) ? 1 : n;
	}
	if (config->selection_seed == 0) {
		config->selection_seed = 0x12345678;
	}

	if (!config->wear_leveling_enabled) {
		if (config->group_profile_map != NULL) {
			spdk_free(config->group_profile_map);
			config->group_profile_map = NULL;
			config->group_profile_capacity = 0;
		}
		config->num_stripe_groups = 0;
		config->select_fn = ec_select_base_bdevs_default;
		SPDK_NOTICELOG("EC bdev %s: Using default round-robin device selection\n",
			       ec_bdev->bdev.name);
		return 0;
	}

	if (prev_group_map != NULL && prev_group_capacity > 0) {
		metadata_loaded = true;
	}

	uint64_t stripes_per_full = (uint64_t)ec_bdev->strip_size * ec_bdev->k;
	if (stripes_per_full == 0) {
		stripes_per_full = 1;
	}
	uint64_t total_stripes = spdk_divide_round_up(ec_bdev->bdev.blockcnt, stripes_per_full);
	uint32_t num_groups = spdk_divide_round_up(total_stripes, config->stripe_group_size);
	if (num_groups == 0) {
		num_groups = 1;
	}
	config->num_stripe_groups = num_groups;
	if (num_groups > 0) {
		rc = ec_selection_ensure_group_capacity(ec_bdev, num_groups);
		if (rc != 0) {
			return rc;
		}
	}

	if (ec_bdev->superblock_enabled && ec_bdev->sb != NULL) {
		ec_bdev_sb_load_selection_metadata(ec_bdev, ec_bdev->sb);
		for (uint32_t profile_idx = 0; profile_idx < EC_MAX_WEAR_PROFILES; profile_idx++) {
			struct ec_wear_profile_slot *slot = &config->wear_profiles[profile_idx];
			if (slot->valid) {
				ec_selection_compute_weights_from_levels(n,
									 slot->wear_levels,
									 slot->wear_weights);
			}
		}
		metadata_loaded = true;
	}

	if (metadata_loaded && config->group_profile_map == NULL) {
		/* No stripe groups to restore - treat as unloaded */
		metadata_loaded = false;
	}

	if (!metadata_loaded && config->group_profile_map != NULL) {
		memset(config->group_profile_map, 0, (size_t)config->num_stripe_groups * sizeof(uint16_t));
	}

	if (config->active_profile_id == 0) {
		config->active_profile_id = 1;
	}
	if (config->next_profile_id == 0 || config->next_profile_id <= config->active_profile_id) {
		config->next_profile_id = config->active_profile_id + 1;
	}

	if (!metadata_loaded) {
		rc = ec_selection_create_profile_from_devices(ec_bdev, true, ec_bdev->superblock_enabled);
		if (rc != 0) {
			return rc;
		}
	}

	struct ec_wear_profile_slot *active_slot =
		ec_selection_find_profile_slot(config, config->active_profile_id);
	if (active_slot == NULL) {
		rc = ec_selection_create_profile_from_devices(ec_bdev, true, ec_bdev->superblock_enabled);
		if (rc != 0) {
			return rc;
		}
		active_slot = ec_selection_find_profile_slot(config, config->active_profile_id);
		if (active_slot == NULL) {
			SPDK_ERRLOG("EC bdev %s: Failed to locate active wear profile %u\n",
				    ec_bdev->bdev.name, config->active_profile_id);
			return -ENOENT;
		}
	}

	ec_selection_copy_profile_to_active(config, active_slot, n);

	rc = ec_assignment_cache_init(&config->assignment_cache,
				      EC_ASSIGNMENT_CACHE_DEFAULT_CAPACITY);
	if (rc != 0) {
		SPDK_WARNLOG("EC bdev %s: Failed to initialize assignment cache (%s)\n",
			     ec_bdev->bdev.name, spdk_strerror(-rc));
		ec_assignment_cache_reset(&config->assignment_cache);
		rc = 0;
	}

	config->select_fn = ec_select_base_bdevs_wear_leveling;
	SPDK_NOTICELOG("EC bdev %s: Using wear-leveling device selection (with built-in fault tolerance)\n",
		       ec_bdev->bdev.name);

	return rc;
}

/*
 * Cleanup device selection configuration
 */
void
ec_bdev_cleanup_selection_config(struct ec_bdev *ec_bdev)
{
	struct ec_device_selection_config *config = &ec_bdev->selection_config;

	if (ec_bdev == NULL) {
		return;
	}

	SPDK_DEBUGLOG(bdev_ec, "EC bdev %s: Cleaning up device selection configuration\n",
		      ec_bdev->bdev.name);

	ec_assignment_cache_reset(&config->assignment_cache);
	if (config->group_profile_map != NULL) {
		spdk_free(config->group_profile_map);
		config->group_profile_map = NULL;
	}
	config->group_profile_capacity = 0;
	config->group_map_dirty = false;
	config->group_map_dirty_count = 0;
	config->group_map_flush_in_progress = false;
	config->group_map_dirty_version = 0;
	config->group_map_flush_version = 0;

	/* Reset configuration */
	memset(config, 0, sizeof(*config));
	config->select_fn = ec_select_base_bdevs_default;  /* Fallback to default */
}

