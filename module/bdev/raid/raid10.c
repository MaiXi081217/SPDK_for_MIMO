/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#include "bdev_raid.h"

#include "spdk/likely.h"
#include "spdk/log.h"

struct raid10_info {
	/* The parent raid bdev */
	struct raid_bdev *raid_bdev;
	/* Number of mirror pairs (for RAID10, num_base_bdevs should be even) */
	uint8_t num_mirror_pairs;
};

struct raid10_io_channel {
	/* Array of per-base_bdev counters of outstanding read blocks on this channel */
	uint64_t read_blocks_outstanding[0];
};

/* Forward declarations */
static void raid10_get_mirror_pair_for_strip(struct raid_bdev *raid_bdev, uint64_t strip_idx,
		uint8_t *mirror_pair_idx, uint8_t *disk_in_pair);
static uint8_t raid10_get_base_bdev_idx(uint8_t mirror_pair_idx, uint8_t disk_in_pair);
static void raid10_process_read_completed(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status);

static void
raid10_channel_inc_read_counters(struct raid_bdev_io_channel *raid_ch, uint8_t idx,
				uint64_t num_blocks)
{
	struct raid10_io_channel *raid10_ch = raid_bdev_channel_get_module_ctx(raid_ch);

	assert(raid10_ch->read_blocks_outstanding[idx] <= UINT64_MAX - num_blocks);
	raid10_ch->read_blocks_outstanding[idx] += num_blocks;
}

static void
raid10_channel_dec_read_counters(struct raid_bdev_io_channel *raid_ch, uint8_t idx,
				uint64_t num_blocks)
{
	struct raid10_io_channel *raid10_ch = raid_bdev_channel_get_module_ctx(raid_ch);

	assert(raid10_ch->read_blocks_outstanding[idx] >= num_blocks);
	raid10_ch->read_blocks_outstanding[idx] -= num_blocks;
}

static void
raid10_init_ext_io_opts(struct spdk_bdev_ext_io_opts *opts, struct raid_bdev_io *raid_io)
{
	memset(opts, 0, sizeof(*opts));
	opts->size = sizeof(*opts);
	opts->memory_domain = raid_io->memory_domain;
	opts->memory_domain_ctx = raid_io->memory_domain_ctx;
	opts->metadata = raid_io->md_buf;
}

static void
raid10_write_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	if (!success) {
		struct raid_base_bdev_info *base_info;

		base_info = raid_bdev_channel_get_base_info(raid_io->raid_ch, bdev_io->bdev);
		if (base_info) {
			raid_bdev_fail_base_bdev(base_info);
		}
	}

	spdk_bdev_free_io(bdev_io);

	raid_bdev_io_complete_part(raid_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);
}

static struct raid_base_bdev_info *
raid10_get_read_io_base_bdev(struct raid_bdev_io *raid_io)
{
	assert(raid_io->type == SPDK_BDEV_IO_TYPE_READ);
	return &raid_io->raid_bdev->base_bdev_info[raid_io->base_bdev_io_submitted];
}

static void
raid10_correct_read_error_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		struct raid_base_bdev_info *base_info = raid10_get_read_io_base_bdev(raid_io);

		/* Writing to the bdev that had the read error failed so fail the base bdev
		 * but complete the raid_io successfully. */
		raid_bdev_fail_base_bdev(base_info);
	}

	raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void
raid10_correct_read_error(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid10_info *r10info = raid_bdev->module_private;
	struct spdk_bdev_ext_io_opts io_opts;
	struct raid_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	uint64_t start_strip;
	uint64_t pd_strip;
	uint32_t offset_in_strip;
	uint64_t pd_lba;
	uint8_t i;
	int ret;

	/* Calculate physical LBA for the failed disk */
	start_strip = raid_io->offset_blocks >> raid_bdev->strip_size_shift;
	pd_strip = start_strip / r10info->num_mirror_pairs;
	offset_in_strip = raid_io->offset_blocks & (raid_bdev->strip_size - 1);
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;

	i = raid_io->base_bdev_io_submitted;
	base_info = &raid_bdev->base_bdev_info[i];
	base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, i);
	assert(base_ch != NULL);

	raid10_init_ext_io_opts(&io_opts, raid_io);
	ret = raid_bdev_writev_blocks_ext(base_info, base_ch, raid_io->iovs, raid_io->iovcnt,
					  pd_lba, raid_io->num_blocks,
					  raid10_correct_read_error_completion, raid_io, &io_opts);
	if (spdk_unlikely(ret != 0)) {
		if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, raid10_correct_read_error);
		} else {
			raid_bdev_fail_base_bdev(base_info);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		}
	}
}

static void raid10_read_other_base_bdev(void *_raid_io);

static void
raid10_read_other_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		assert(raid_io->base_bdev_io_remaining > 0);
		raid_io->base_bdev_io_remaining--;
		raid10_read_other_base_bdev(raid_io);
		return;
	}

	/* try to correct the read error by writing data read from the other base bdev */
	raid10_correct_read_error(raid_io);
}

static void
raid10_read_other_base_bdev(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid10_info *r10info = raid_bdev->module_private;
	struct spdk_bdev_ext_io_opts io_opts;
	struct raid_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	uint64_t start_strip;
	uint64_t pd_strip;
	uint32_t offset_in_strip;
	uint64_t pd_lba;
	uint8_t mirror_pair_idx;
	uint8_t disk_in_pair;
	uint8_t base_idx[2];
	uint8_t i;
	int ret;

	/* Calculate which strip this I/O belongs to */
	start_strip = raid_io->offset_blocks >> raid_bdev->strip_size_shift;
	pd_strip = start_strip / r10info->num_mirror_pairs;
	offset_in_strip = raid_io->offset_blocks & (raid_bdev->strip_size - 1);
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;

	raid10_get_mirror_pair_for_strip(raid_bdev, start_strip, &mirror_pair_idx, &disk_in_pair);
	base_idx[0] = raid10_get_base_bdev_idx(mirror_pair_idx, 0);
	base_idx[1] = raid10_get_base_bdev_idx(mirror_pair_idx, 1);

	/* Try the other disk in the mirror pair */
	for (i = 0; i < 2; i++) {
		uint8_t idx = base_idx[i];
		if (idx == raid_io->base_bdev_io_submitted || idx >= raid_bdev->num_base_bdevs) {
			continue;
		}

		base_info = &raid_bdev->base_bdev_info[idx];
		base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, idx);

		if (base_ch == NULL) {
			continue;
		}

		raid10_init_ext_io_opts(&io_opts, raid_io);
		ret = raid_bdev_readv_blocks_ext(base_info, base_ch, raid_io->iovs, raid_io->iovcnt,
						 pd_lba, raid_io->num_blocks,
						 raid10_read_other_completion, raid_io, &io_opts);
		if (spdk_unlikely(ret != 0)) {
			if (ret == -ENOMEM) {
				raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, raid10_read_other_base_bdev);
			} else {
				break;
			}
		}
		return;
	}

	base_info = raid10_get_read_io_base_bdev(raid_io);
	raid_bdev_fail_base_bdev(base_info);

	raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
}

static void
raid10_read_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid10_channel_dec_read_counters(raid_io->raid_ch, raid_io->base_bdev_io_submitted,
					raid_io->num_blocks);

	if (!success) {
		/* For RAID10, we only need to try the other disk in the same mirror pair */
		raid_io->base_bdev_io_remaining = 1;
		raid10_read_other_base_bdev(raid_io);
		return;
	}

	raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_SUCCESS);
}

static void raid10_submit_rw_request(struct raid_bdev_io *raid_io);

static void
_raid10_submit_rw_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid10_submit_rw_request(raid_io);
}

/*
 * Calculate which mirror pair and which disk in the pair to use for a given strip
 * RAID10: data is striped across mirror pairs, and each strip is mirrored within its pair
 */
static void
raid10_get_mirror_pair_for_strip(struct raid_bdev *raid_bdev, uint64_t strip_idx,
				  uint8_t *mirror_pair_idx, uint8_t *disk_in_pair)
{
	struct raid10_info *r10info = raid_bdev->module_private;
	uint8_t num_pairs = r10info->num_mirror_pairs;

	/* Which mirror pair this strip belongs to */
	*mirror_pair_idx = strip_idx % num_pairs;

	/* Within the mirror pair, choose the disk with less outstanding I/O
	 * For now, we'll use the first disk (index 0) in the pair, but this
	 * could be optimized to load balance
	 */
	*disk_in_pair = 0;
}

/*
 * Get the base bdev index from mirror pair index and disk index within pair
 */
static uint8_t
raid10_get_base_bdev_idx(uint8_t mirror_pair_idx, uint8_t disk_in_pair)
{
	/* Each mirror pair has 2 disks, so base_bdev_idx = mirror_pair_idx * 2 + disk_in_pair */
	return mirror_pair_idx * 2 + disk_in_pair;
}

static uint8_t
raid10_channel_next_read_base_bdev(struct raid_bdev *raid_bdev, struct raid_bdev_io_channel *raid_ch,
				    uint64_t strip_idx, struct raid_base_bdev_info *exclude_base_info)
{
	struct raid10_io_channel *raid10_ch = raid_bdev_channel_get_module_ctx(raid_ch);
	uint8_t mirror_pair_idx, disk_in_pair;
	uint8_t base_idx[2];  /* Two disks in the mirror pair */
	uint64_t read_blocks_min = UINT64_MAX;
	uint8_t idx = UINT8_MAX;
	uint8_t i;

	raid10_get_mirror_pair_for_strip(raid_bdev, strip_idx, &mirror_pair_idx, &disk_in_pair);

	/* Get the two disk indices in this mirror pair */
	base_idx[0] = raid10_get_base_bdev_idx(mirror_pair_idx, 0);
	base_idx[1] = raid10_get_base_bdev_idx(mirror_pair_idx, 1);

	/* Choose the disk with less outstanding read blocks, excluding the target if specified */
	for (i = 0; i < 2; i++) {
		uint8_t disk = base_idx[i];
		struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[disk];
		
		/* Skip if this is the excluded disk (e.g., target disk during rebuild) */
		if (exclude_base_info != NULL && base_info == exclude_base_info) {
			continue;
		}
		
		if (disk < raid_bdev->num_base_bdevs &&
		    raid_bdev_channel_get_base_channel(raid_ch, disk) != NULL &&
		    raid10_ch->read_blocks_outstanding[disk] < read_blocks_min) {
			read_blocks_min = raid10_ch->read_blocks_outstanding[disk];
			idx = disk;
		}
	}

	return idx;
}

static int
raid10_submit_read_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid_bdev_io_channel *raid_ch = raid_io->raid_ch;
	struct spdk_bdev_ext_io_opts io_opts;
	struct raid_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	uint64_t start_strip;
	uint64_t end_strip;
	uint64_t pd_strip;  /* Physical disk strip */
	uint32_t offset_in_strip;
	uint64_t pd_lba;    /* Physical disk LBA */
	uint8_t idx;
	int ret;

	/* Calculate which strip this I/O belongs to */
	start_strip = raid_io->offset_blocks >> raid_bdev->strip_size_shift;
	end_strip = (raid_io->offset_blocks + raid_io->num_blocks - 1) >>
		    raid_bdev->strip_size_shift;

	/* Check if I/O spans multiple strips - this requires splitting which is handled by
	 * the framework via optimal_io_boundary, but we should still validate */
	if (start_strip != end_strip && raid_bdev->num_base_bdevs > 2) {
		/* For RAID10 with multiple mirror pairs, I/O spanning strips should be split
		 * by the framework. If we get here, it's an error. */
		SPDK_ERRLOG("I/O spans strip boundary! start_strip=%" PRIu64 ", end_strip=%" PRIu64 "\n",
			    start_strip, end_strip);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return 0;
	}

	/* Calculate the physical disk strip and offset within the mirror pair */
	struct raid10_info *r10info = raid_bdev->module_private;
	uint8_t num_pairs = r10info->num_mirror_pairs;
	uint8_t mirror_pair_idx;
	uint8_t disk_in_pair;

	pd_strip = start_strip / num_pairs;
	raid10_get_mirror_pair_for_strip(raid_bdev, start_strip, &mirror_pair_idx, &disk_in_pair);

	/* Calculate offset within strip and physical LBA */
	offset_in_strip = raid_io->offset_blocks & (raid_bdev->strip_size - 1);
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;

	/* Choose which disk in the mirror pair to read from */
	idx = raid10_channel_next_read_base_bdev(raid_bdev, raid_ch, start_strip, NULL);
	if (spdk_unlikely(idx == UINT8_MAX)) {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return 0;
	}

	base_info = &raid_bdev->base_bdev_info[idx];
	base_ch = raid_bdev_channel_get_base_channel(raid_ch, idx);

	raid10_init_ext_io_opts(&io_opts, raid_io);
	ret = raid_bdev_readv_blocks_ext(base_info, base_ch, raid_io->iovs, raid_io->iovcnt,
					 pd_lba, raid_io->num_blocks,
					 raid10_read_bdev_io_completion, raid_io, &io_opts);

	if (spdk_likely(ret == 0)) {
		raid10_channel_inc_read_counters(raid_ch, idx, raid_io->num_blocks);
		raid_io->base_bdev_io_submitted = idx;
	} else if (spdk_unlikely(ret == -ENOMEM)) {
		raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid10_submit_rw_request);
		return 0;
	}

	return ret;
}

static int
raid10_submit_write_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid10_info *r10info = raid_bdev->module_private;
	struct spdk_bdev_ext_io_opts io_opts;
	struct raid_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	uint64_t start_strip;
	uint64_t end_strip;
	uint64_t pd_strip;  /* Physical disk strip */
	uint32_t offset_in_strip;
	uint64_t pd_lba;    /* Physical disk LBA */
	uint8_t mirror_pair_idx;
	uint8_t disk_in_pair;
	uint8_t base_idx[2];  /* Two disks in the mirror pair */
	uint8_t idx;
	uint64_t base_bdev_io_not_submitted;
	int ret = 0;

	/* Calculate which strip this I/O belongs to */
	start_strip = raid_io->offset_blocks >> raid_bdev->strip_size_shift;
	end_strip = (raid_io->offset_blocks + raid_io->num_blocks - 1) >>
		    raid_bdev->strip_size_shift;

	/* Check if I/O spans multiple strips */
	if (start_strip != end_strip && raid_bdev->num_base_bdevs > 2) {
		SPDK_ERRLOG("I/O spans strip boundary! start_strip=%" PRIu64 ", end_strip=%" PRIu64 "\n",
			    start_strip, end_strip);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return 0;
	}

	/* Calculate the physical disk strip and which mirror pair */
	uint8_t num_pairs = r10info->num_mirror_pairs;
	pd_strip = start_strip / num_pairs;
	raid10_get_mirror_pair_for_strip(raid_bdev, start_strip, &mirror_pair_idx, &disk_in_pair);

	/* Calculate offset within strip and physical LBA */
	offset_in_strip = raid_io->offset_blocks & (raid_bdev->strip_size - 1);
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;

	/* Get the two disk indices in this mirror pair */
	base_idx[0] = raid10_get_base_bdev_idx(mirror_pair_idx, 0);
	base_idx[1] = raid10_get_base_bdev_idx(mirror_pair_idx, 1);

	if (raid_io->base_bdev_io_submitted == 0) {
		/* We need to write to both disks in the mirror pair */
		raid_io->base_bdev_io_remaining = 2;
		raid_bdev_io_set_default_status(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

	raid10_init_ext_io_opts(&io_opts, raid_io);
	/* Write to both disks in the mirror pair */
	for (disk_in_pair = 0; disk_in_pair < 2; disk_in_pair++) {
		idx = base_idx[disk_in_pair];
		if (idx >= raid_bdev->num_base_bdevs) {
			raid_io->base_bdev_io_submitted++;
			raid_bdev_io_complete_part(raid_io, 1, SPDK_BDEV_IO_STATUS_FAILED);
			continue;
		}

		base_info = &raid_bdev->base_bdev_info[idx];
		base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, idx);

		if (base_ch == NULL) {
			/* skip a missing base bdev's slot */
			raid_io->base_bdev_io_submitted++;
			raid_bdev_io_complete_part(raid_io, 1, SPDK_BDEV_IO_STATUS_FAILED);
			continue;
		}

		ret = raid_bdev_writev_blocks_ext(base_info, base_ch, raid_io->iovs, raid_io->iovcnt,
						  pd_lba, raid_io->num_blocks,
						  raid10_write_bdev_io_completion, raid_io, &io_opts);
		if (spdk_unlikely(ret != 0)) {
			if (spdk_unlikely(ret == -ENOMEM)) {
				raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid10_submit_rw_request);
				return 0;
			}

			base_bdev_io_not_submitted = 2 - raid_io->base_bdev_io_submitted;
			raid_bdev_io_complete_part(raid_io, base_bdev_io_not_submitted,
						   SPDK_BDEV_IO_STATUS_FAILED);
			return 0;
		}

		raid_io->base_bdev_io_submitted++;
	}

	if (raid_io->base_bdev_io_submitted == 0) {
		ret = -ENODEV;
	}

	return ret;
}

static void
raid10_submit_rw_request(struct raid_bdev_io *raid_io)
{
	int ret;

	switch (raid_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		ret = raid10_submit_read_request(raid_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		ret = raid10_submit_write_request(raid_io);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (spdk_unlikely(ret != 0)) {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void raid10_submit_null_payload_request(struct raid_bdev_io *raid_io);

static void
_raid10_submit_null_payload_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid10_submit_null_payload_request(raid_io);
}

static inline void
raid10_null_payload_request_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	raid10_write_bdev_io_completion(bdev_io, success, cb_arg);
}

static void
raid10_submit_null_payload_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid10_info *r10info = raid_bdev->module_private;
	struct spdk_bdev_ext_io_opts io_opts;
	struct raid_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	uint64_t start_strip;
	uint64_t pd_strip;  /* Physical disk strip */
	uint32_t offset_in_strip;
	uint64_t pd_lba;    /* Physical disk LBA */
	uint8_t mirror_pair_idx;
	uint8_t disk_in_pair;
	uint8_t base_idx[2];  /* Two disks in the mirror pair */
	uint8_t idx;
	uint64_t base_bdev_io_not_submitted;
	int ret = 0;

	/* Calculate which strip this I/O belongs to */
	start_strip = raid_io->offset_blocks >> raid_bdev->strip_size_shift;

	/* Calculate the physical disk strip and which mirror pair */
	uint8_t num_pairs = r10info->num_mirror_pairs;
	pd_strip = start_strip / num_pairs;
	raid10_get_mirror_pair_for_strip(raid_bdev, start_strip, &mirror_pair_idx, &disk_in_pair);

	/* Calculate offset within strip and physical LBA */
	offset_in_strip = raid_io->offset_blocks & (raid_bdev->strip_size - 1);
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;

	/* Get the two disk indices in this mirror pair */
	base_idx[0] = raid10_get_base_bdev_idx(mirror_pair_idx, 0);
	base_idx[1] = raid10_get_base_bdev_idx(mirror_pair_idx, 1);

	if (raid_io->base_bdev_io_submitted == 0) {
		/* We need to write to both disks in the mirror pair */
		raid_io->base_bdev_io_remaining = 2;
		raid_bdev_io_set_default_status(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
	}

	raid10_init_ext_io_opts(&io_opts, raid_io);
	/* Write to both disks in the mirror pair */
	for (disk_in_pair = 0; disk_in_pair < 2; disk_in_pair++) {
		idx = base_idx[disk_in_pair];
		if (idx >= raid_bdev->num_base_bdevs) {
			raid_io->base_bdev_io_submitted++;
			raid_bdev_io_complete_part(raid_io, 1, SPDK_BDEV_IO_STATUS_FAILED);
			continue;
		}

		base_info = &raid_bdev->base_bdev_info[idx];
		base_ch = raid_bdev_channel_get_base_channel(raid_io->raid_ch, idx);

		if (base_ch == NULL) {
			/* skip a missing base bdev's slot */
			raid_io->base_bdev_io_submitted++;
			raid_bdev_io_complete_part(raid_io, 1, SPDK_BDEV_IO_STATUS_FAILED);
			continue;
		}

		switch (raid_io->type) {
		case SPDK_BDEV_IO_TYPE_UNMAP:
			ret = raid_bdev_unmap_blocks(base_info, base_ch,
						     pd_lba, raid_io->num_blocks,
						     raid10_null_payload_request_io_completion, raid_io);
			break;

		case SPDK_BDEV_IO_TYPE_FLUSH:
			ret = raid_bdev_flush_blocks(base_info, base_ch,
						     pd_lba, raid_io->num_blocks,
						     raid10_null_payload_request_io_completion, raid_io);
			break;

		default:
			SPDK_ERRLOG("submit request, invalid io type with null payload %u\n", raid_io->type);
			assert(false);
			ret = -EIO;
		}

		if (spdk_unlikely(ret != 0)) {
			if (spdk_unlikely(ret == -ENOMEM)) {
				raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
							base_ch, _raid10_submit_null_payload_request);
				return;
			}

			base_bdev_io_not_submitted = 2 - raid_io->base_bdev_io_submitted;
			raid_bdev_io_complete_part(raid_io, base_bdev_io_not_submitted,
						   SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}

		raid_io->base_bdev_io_submitted++;
	}

	assert(raid_io->base_bdev_io_submitted != 0);
}

static void
raid10_ioch_destroy(void *io_device, void *ctx_buf)
{
}

static int
raid10_ioch_create(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
raid10_io_device_unregister_done(void *io_device)
{
	struct raid10_info *r10info = io_device;

	raid_bdev_module_stop_done(r10info->raid_bdev);

	free(r10info);
}

static int
raid10_start(struct raid_bdev *raid_bdev)
{
	uint64_t min_blockcnt = UINT64_MAX;
	uint64_t base_bdev_data_size;
	struct raid_base_bdev_info *base_info;
	struct raid10_info *r10info;
	char name[256];

	/* RAID10 requires even number of base bdevs (for mirror pairs) */
	if (raid_bdev->num_base_bdevs % 2 != 0) {
		SPDK_ERRLOG("RAID10 requires even number of base bdevs, got %u\n",
			    raid_bdev->num_base_bdevs);
		return -EINVAL;
	}

	/* RAID10 requires strip_size */
	if (raid_bdev->strip_size == 0) {
		SPDK_ERRLOG("RAID10 requires strip_size to be set\n");
		return -EINVAL;
	}

	r10info = calloc(1, sizeof(*r10info));
	if (!r10info) {
		SPDK_ERRLOG("Failed to allocate RAID10 info device structure\n");
		return -ENOMEM;
	}
	r10info->raid_bdev = raid_bdev;
	r10info->num_mirror_pairs = raid_bdev->num_base_bdevs / 2;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		min_blockcnt = spdk_min(min_blockcnt, base_info->data_size);
	}

	/* Align to strip size */
	base_bdev_data_size = (min_blockcnt >> raid_bdev->strip_size_shift) << raid_bdev->strip_size_shift;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->data_size = base_bdev_data_size;
	}

	/* RAID10 capacity = base_bdev_data_size * num_mirror_pairs (striped across pairs) */
	raid_bdev->bdev.blockcnt = base_bdev_data_size * r10info->num_mirror_pairs;
	raid_bdev->module_private = r10info;

	if (raid_bdev->num_base_bdevs > 2) {
		raid_bdev->bdev.optimal_io_boundary = raid_bdev->strip_size;
		raid_bdev->bdev.split_on_optimal_io_boundary = true;
	} else {
		/* Single mirror pair, no striping */
		raid_bdev->bdev.optimal_io_boundary = 0;
		raid_bdev->bdev.split_on_optimal_io_boundary = false;
	}

	snprintf(name, sizeof(name), "raid10_%s", raid_bdev->bdev.name);
	spdk_io_device_register(r10info, raid10_ioch_create, raid10_ioch_destroy,
				sizeof(struct raid10_io_channel) + raid_bdev->num_base_bdevs * sizeof(uint64_t),
				name);

	return 0;
}

static bool
raid10_stop(struct raid_bdev *raid_bdev)
{
	struct raid10_info *r10info = raid_bdev->module_private;

	spdk_io_device_unregister(r10info, raid10_io_device_unregister_done);

	return false;
}

static struct spdk_io_channel *
raid10_get_io_channel(struct raid_bdev *raid_bdev)
{
	struct raid10_info *r10info = raid_bdev->module_private;

	return spdk_get_io_channel(r10info);
}

static void
raid10_process_write_completed(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_process_request *process_req = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid_bdev_process_request_complete(process_req, success ? 0 : -EIO);
}

static void raid10_process_submit_write(struct raid_bdev_process_request *process_req);

static void
_raid10_process_submit_write(void *ctx)
{
	struct raid_bdev_process_request *process_req = ctx;

	raid10_process_submit_write(process_req);
}

static void
raid10_process_submit_write(struct raid_bdev_process_request *process_req)
{
	struct raid_bdev_io *raid_io = &process_req->raid_io;
	struct raid_bdev *raid_bdev = raid_io->raid_bdev;
	struct raid10_info *r10info = raid_bdev->module_private;
	struct spdk_bdev_ext_io_opts io_opts;
	uint64_t start_strip;
	uint64_t pd_strip;
	uint32_t offset_in_strip;
	uint64_t pd_lba;
	int ret;

	/* Calculate physical LBA for process request (rebuild/resync) */
	start_strip = raid_io->offset_blocks >> raid_bdev->strip_size_shift;
	pd_strip = start_strip / r10info->num_mirror_pairs;
	offset_in_strip = raid_io->offset_blocks & (raid_bdev->strip_size - 1);
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;

	raid10_init_ext_io_opts(&io_opts, raid_io);
	ret = raid_bdev_writev_blocks_ext(process_req->target, process_req->target_ch,
					  raid_io->iovs, raid_io->iovcnt,
					  pd_lba, raid_io->num_blocks,
					  raid10_process_write_completed, process_req, &io_opts);
	if (spdk_unlikely(ret != 0)) {
		if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(process_req->target->desc),
						process_req->target_ch, _raid10_process_submit_write);
		} else {
			raid_bdev_process_request_complete(process_req, ret);
		}
	}
}

/* Wrapper function to convert spdk_bdev_io_completion_cb to raid10_process_read_completed */
static void
raid10_process_read_bdev_io_completion(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;
	enum spdk_bdev_io_status status;

	spdk_bdev_free_io(bdev_io);

	status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;
	raid10_process_read_completed(raid_io, status);
}

static void
raid10_process_read_completed(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	struct raid_bdev_process_request *process_req = SPDK_CONTAINEROF(raid_io,
			struct raid_bdev_process_request, raid_io);

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		raid_bdev_process_request_complete(process_req, -EIO);
		return;
	}

	raid10_process_submit_write(process_req);
}

static int
raid10_submit_process_request(struct raid_bdev_process_request *process_req,
			     struct raid_bdev_io_channel *raid_ch)
{
	struct raid_bdev_io *raid_io = &process_req->raid_io;
	/* Process context always owns the raid_bdev pointer (target may be NULL during setup). */
	struct raid_bdev *raid_bdev = process_req->target->raid_bdev;
	struct raid_bdev_io_channel *process_raid_ch = raid_ch;
	struct spdk_bdev_ext_io_opts io_opts;
	struct raid_base_bdev_info *base_info;
	struct spdk_io_channel *base_ch;
	uint64_t start_strip;
	uint64_t end_strip;
	uint64_t pd_strip;
	uint32_t offset_in_strip;
	uint64_t pd_lba;
	uint8_t idx;
	int ret;

	/* For rebuild, we need to read from the mirror pair but exclude the target disk */
	raid_bdev_io_init(raid_io, process_raid_ch, SPDK_BDEV_IO_TYPE_READ,
			  process_req->offset_blocks, process_req->num_blocks,
			  &process_req->iov, 1, process_req->md_buf, NULL, NULL);
	raid_io->raid_bdev = raid_bdev;
	raid_io->completion_cb = raid10_process_read_completed;

	/* Calculate which strip this I/O belongs to */
	start_strip = raid_io->offset_blocks >> raid_bdev->strip_size_shift;
	end_strip = (raid_io->offset_blocks + raid_io->num_blocks - 1) >>
		    raid_bdev->strip_size_shift;

	/* Check if I/O spans multiple strips */
	if (start_strip != end_strip && raid_bdev->num_base_bdevs > 2) {
		SPDK_ERRLOG("Process I/O spans strip boundary! start_strip=%" PRIu64 ", end_strip=%" PRIu64 "\n",
			    start_strip, end_strip);
		return -EINVAL;
	}

	/* Choose which disk in the mirror pair to read from, excluding the target disk */
	idx = raid10_channel_next_read_base_bdev(raid_bdev, process_raid_ch, start_strip,
						 process_req->target);
	if (spdk_unlikely(idx == UINT8_MAX)) {
		SPDK_ERRLOG("No available disk in mirror pair for rebuild at strip %" PRIu64 "\n",
			    start_strip);
		return -ENODEV;
	}

	/* Calculate physical LBA */
	struct raid10_info *r10info = raid_bdev->module_private;
	pd_strip = start_strip / r10info->num_mirror_pairs;
	offset_in_strip = raid_io->offset_blocks & (raid_bdev->strip_size - 1);
	pd_lba = (pd_strip << raid_bdev->strip_size_shift) + offset_in_strip;

	base_info = &raid_bdev->base_bdev_info[idx];
	base_ch = raid_bdev_channel_get_base_channel(process_raid_ch, idx);

	raid10_init_ext_io_opts(&io_opts, raid_io);
	ret = raid_bdev_readv_blocks_ext(base_info, base_ch, raid_io->iovs, raid_io->iovcnt,
					 pd_lba, raid_io->num_blocks,
					 raid10_process_read_bdev_io_completion, raid_io, &io_opts);

	if (spdk_likely(ret == 0)) {
		raid10_channel_inc_read_counters(process_raid_ch, idx, raid_io->num_blocks);
		raid_io->base_bdev_io_submitted = idx;
		return process_req->num_blocks;
	} else if (spdk_unlikely(ret == -ENOMEM)) {
		raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
					base_ch, _raid10_submit_rw_request);
		return 0;
	} else {
		return ret;
	}
}

static bool
raid10_resize(struct raid_bdev *raid_bdev)
{
	int rc;
	uint64_t min_blockcnt = UINT64_MAX;
	uint64_t base_bdev_data_size;
	struct raid_base_bdev_info *base_info;
	struct raid10_info *r10info = raid_bdev->module_private;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		struct spdk_bdev *base_bdev;

		if (base_info->desc == NULL) {
			continue;
		}
		base_bdev = spdk_bdev_desc_get_bdev(base_info->desc);
		min_blockcnt = spdk_min(min_blockcnt, base_bdev->blockcnt - base_info->data_offset);
	}

	/* Align to strip size */
	base_bdev_data_size = (min_blockcnt >> raid_bdev->strip_size_shift) << raid_bdev->strip_size_shift;
	uint64_t new_blockcnt = base_bdev_data_size * r10info->num_mirror_pairs;

	if (new_blockcnt == raid_bdev->bdev.blockcnt) {
		return false;
	}

	rc = spdk_bdev_notify_blockcnt_change(&raid_bdev->bdev, new_blockcnt);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to notify blockcount change\n");
		return false;
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->data_size = base_bdev_data_size;
	}
	return true;
}

static struct raid_bdev_module g_raid10_module = {
	.level = RAID10,
	.base_bdevs_min = 2,
	.base_bdevs_constraint = {CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL, 1},
	.memory_domains_supported = true,
	.start = raid10_start,
	.stop = raid10_stop,
	.submit_rw_request = raid10_submit_rw_request,
	.submit_null_payload_request = raid10_submit_null_payload_request,
	.get_io_channel = raid10_get_io_channel,
	.submit_process_request = raid10_submit_process_request,
	.resize = raid10_resize,
};
RAID_MODULE_REGISTER(&g_raid10_module)

SPDK_LOG_REGISTER_COMPONENT(bdev_raid10)
