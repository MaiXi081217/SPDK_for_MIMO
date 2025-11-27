/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_EC_H
#define SPDK_BDEV_EC_H

#include "spdk/bdev_module.h"
#include "spdk/thread.h"
#include "spdk/uuid.h"
#include "spdk/env.h"
#include <isa-l/erasure_code.h>

#define EC_BDEV_MIN_DATA_OFFSET_SIZE	(1024*1024) /* 1 MiB (minimum metadata reservation) */

/* Maximum k and p values for EC */
#define EC_MAX_K 255
#define EC_MAX_P 255

/* Buffer pool configuration */
#define EC_BDEV_DEFAULT_BUF_ALIGNMENT	0x1000	/* 4KB default alignment */
#define EC_BDEV_PARITY_BUF_POOL_MAX	128	/* Maximum parity buffers in pool */
#define EC_BDEV_RMW_BUF_POOL_MAX	64	/* Maximum RMW stripe buffers in pool */
#define EC_BDEV_TEMP_DATA_BUF_POOL_MAX	32	/* Maximum temporary data buffers in pool */
#define EC_BDEV_BITMAP_SIZE_LIMIT	64	/* Bitmap size limit for fast lookup */
#define EC_MAX_ENCODE_WORKERS	2	/* Default number of dedicated encoding workers */

/* Invalid offset marker */
#define EC_OFFSET_BLOCKS_INVALID	UINT64_MAX

/* RPC configuration */
#define EC_RPC_MAX_BASE_BDEVS		255	/* Maximum base bdevs in RPC */
#define EC_MAX_BASE_BDEVS		255	/* Maximum base bdevs for device selection */
#define EC_ASSIGNMENT_CACHE_DEFAULT_CAPACITY	16384  /* Default stripe assignment cache size (~8.5MB, must be power of two) */

/* Forward declaration */
struct ec_bdev;
struct ec_assignment_cache_entry;

struct ec_assignment_cache {
	struct ec_assignment_cache_entry *entries;
	uint32_t capacity;
	uint32_t mask;
	bool initialized;
	struct spdk_spinlock lock;
};

/* Wear profile management */
#define EC_MAX_WEAR_PROFILES	256
#define EC_GROUP_PROFILE_INVALID	0
#define EC_GROUP_MAP_FLUSH_THRESHOLD	1024
#define EC_GROUP_MAP_FLUSH_MAX_RETRIES	3

struct ec_wear_profile_slot {
	uint16_t profile_id;
	uint16_t reserved;
	bool valid;
	uint32_t wear_levels[EC_MAX_BASE_BDEVS];
	uint32_t wear_weights[EC_MAX_BASE_BDEVS];
};

/* Device selection strategy configuration */
struct ec_device_selection_config {
	/* Wear leveling (static, read once at initialization) */
	bool wear_leveling_enabled;
	uint32_t wear_levels[EC_MAX_BASE_BDEVS];   /* Static wear values */
	uint32_t wear_weights[EC_MAX_BASE_BDEVS];  /* Computed weights */
	
	/* Fault tolerance for wear leveling (内置在磨损均衡算法中) */
	uint8_t stripe_group_size;  /* Stripe group size for fault tolerance (usually = num_base_bdevs) */
	
	/* Deterministic algorithm parameters */
	uint32_t selection_seed;  /* Seed for deterministic selection */
	
	/* Device selection function pointer */
	int (*select_fn)(struct ec_bdev *ec_bdev,
			uint64_t stripe_index,
			uint8_t *data_indices,
			uint8_t *parity_indices);

	/* Wear profile tracking */
	uint16_t active_profile_id;
	uint16_t next_profile_id;
	uint32_t wear_profile_count;
	struct ec_wear_profile_slot wear_profiles[EC_MAX_WEAR_PROFILES];

	/* Stripe group profile map */
	uint16_t *group_profile_map;
	uint32_t num_stripe_groups;
	uint32_t group_profile_capacity;
	bool group_map_dirty;
	uint32_t group_map_dirty_count;
	bool group_map_flush_in_progress;
	uint64_t group_map_dirty_version;
	uint64_t group_map_flush_version;
	uint32_t group_map_flush_retries;
	
	/* Debug flags */
	bool debug_enabled;  /* Enable debug logging */

	/* Deterministic stripe assignment cache (accelerates fault tolerance checks) */
	struct ec_assignment_cache assignment_cache;
};

/*
 * EC bdev state
 */
enum ec_bdev_state {
	/* EC bdev is ready and is seen by upper layers */
	EC_BDEV_STATE_ONLINE,

	/*
	 * EC bdev is configuring, not all underlying bdevs are present.
	 * And can't be seen by upper layers.
	 */
	EC_BDEV_STATE_CONFIGURING,

	/*
	 * In offline state, EC bdev layer will complete all incoming commands without
	 * submitting to underlying base bdevs
	 */
	EC_BDEV_STATE_OFFLINE,

	/* EC bdev state max */
	EC_BDEV_STATE_MAX
};

typedef void (*ec_base_bdev_cb)(void *ctx, int status);

/*
 * ec_base_bdev_info contains information for the base bdevs which are part of some
 * EC bdev. This structure contains the per base bdev information.
 */
struct ec_base_bdev_info {
	/* The EC bdev that this base bdev belongs to */
	struct ec_bdev	*ec_bdev;

	/* name of the bdev */
	char		*name;

	/* uuid of the bdev */
	struct spdk_uuid uuid;

	/*
	 * Pointer to base bdev descriptor opened by EC bdev. This is NULL when the bdev for
	 * this slot is missing.
	 */
	struct spdk_bdev_desc	*desc;

	/* offset in blocks from the start of the base bdev to the start of the data region */
	uint64_t		data_offset;

	/* size in blocks of the base bdev's data region */
	uint64_t		data_size;

	/*
	 * When underlying base device calls the hot plug function on drive removal,
	 * this flag will be set and later after doing some processing, base device
	 * descriptor will be closed
	 */
	bool			remove_scheduled;

	/* callback for base bdev removal */
	ec_base_bdev_cb		remove_cb;

	/* context of the callback */
	void			*remove_cb_ctx;

	/* Hold the number of blocks to know how large the base bdev is resized. */
	uint64_t		blockcnt;

	/* io channel for the app thread */
	struct spdk_io_channel	*app_thread_ch;

	/* Set to true when base bdev has completed the configuration process */
	bool			is_configured;

	/* Set to true if this base bdev is a data block (false means parity block) */
	bool			is_data_block;

	/* Set to true to indicate that the base bdev is being removed because of a failure */
	bool			is_failed;

	/* callback for base bdev configuration */
	ec_base_bdev_cb		configure_cb;

	/* context of the callback */
	void			*configure_cb_ctx;
};

struct ec_bdev_io;
typedef void (*ec_bdev_io_completion_cb)(struct ec_bdev_io *ec_io,
		enum spdk_bdev_io_status status);

/*
 * ec_bdev_io is the context part of bdev_io. It contains the information
 * related to bdev_io for an EC bdev
 */
struct ec_bdev_io {
	/* The EC bdev associated with this IO */
	struct ec_bdev *ec_bdev;

	uint64_t offset_blocks;
	uint64_t num_blocks;
	struct iovec *iovs;
	int iovcnt;
	enum spdk_bdev_io_type type;
	struct spdk_memory_domain *memory_domain;
	void *memory_domain_ctx;
	void *md_buf;

	/* WaitQ entry, used only in waitq logic */
	struct spdk_bdev_io_wait_entry	waitq_entry;

	/* Context of the original channel for this IO */
	struct ec_bdev_io_channel	*ec_ch;

	/* Used for tracking progress on io requests sent to member disks. */
	uint64_t			base_bdev_io_remaining;
	uint8_t				base_bdev_io_submitted;
	enum spdk_bdev_io_status	base_bdev_io_status;
	enum spdk_bdev_io_status	base_bdev_io_status_default;
	
	/* Optimized: Map base bdev I/O to index for O(1) lookup in completion callback
	 * This array stores the base_bdev_idx for each submitted I/O
	 * Index in array corresponds to submission order (0 to base_bdev_io_submitted-1)
	 */
	uint8_t				base_bdev_idx_map[EC_MAX_K + EC_MAX_P];
	
	/* Optimized: Direct base_info pointer map for true O(1) lookup in completion callback
	 * This eliminates the need for bdev pointer comparison in hot path
	 * Index corresponds to submission order, matches base_bdev_idx_map
	 */
	struct ec_base_bdev_info	*base_info_map[EC_MAX_K + EC_MAX_P];

	/* Private data for the EC module */
	void				*module_private;

	/* Custom completion callback. Overrides bdev_io completion if set. */
	ec_bdev_io_completion_cb	completion_cb;
};

typedef void (*ec_bdev_configure_cb)(void *cb_ctx, int rc);
typedef void (*ec_bdev_destruct_cb)(void *cb_ctx, int rc);

/*
 * ec_bdev is the single entity structure which contains SPDK block device
 * and the information related to any EC bdev either configured or
 * in configuring list.
 */
struct ec_bdev {
	/* EC bdev device, this will get registered in bdev layer */
	struct spdk_bdev		bdev;

	/* the EC bdev descriptor, opened for internal use */
	struct spdk_bdev_desc		*self_desc;

	/* link of EC bdev to link it to global EC bdev list */
	TAILQ_ENTRY(ec_bdev)		global_link;

	/* array of base bdev info */
	struct ec_base_bdev_info	*base_bdev_info;

	/* strip size of EC bdev in blocks */
	uint32_t			strip_size;

	/* strip size of EC bdev in KB */
	uint32_t			strip_size_kb;

	/* strip size bit shift for optimized calculation */
	uint32_t			strip_size_shift;

	/* state of EC bdev */
	enum ec_bdev_state		state;

	/* number of base bdevs comprising EC bdev */
	uint8_t				num_base_bdevs;

	/* number of base bdevs discovered */
	uint8_t				num_base_bdevs_discovered;

	/* number of operational base bdevs */
	uint8_t				num_base_bdevs_operational;

	/* minimum number of viable base bdevs that are required by array to operate */
	uint8_t				min_base_bdevs_operational;

	/* Number of data blocks (k) */
	uint8_t				k;

	/* Number of parity blocks (p) */
	uint8_t				p;

	/* Set to true if destroy of this EC bdev is started. */
	bool				destroy_started;

	/* Superblock */
	bool				superblock_enabled;
	struct ec_bdev_superblock	*sb;
	size_t				sb_buffer_capacity;

	/* Superblock buffer used for I/O */
	void				*sb_io_buf;
	uint32_t			sb_io_buf_size;

	/* EC module private data */
	struct ec_bdev_module_private	*module_private;

	/* Callback and context for ec_bdev configuration */
	ec_bdev_configure_cb		configure_cb;
	void				*configure_cb_ctx;

	/* Callback and context for ec_bdev deconfiguration (during deletion) */
	ec_bdev_destruct_cb		deconfigure_cb_fn;
	void				*deconfigure_cb_arg;

	/* Buffer alignment requirement for memory allocations */
	size_t				buf_alignment;

	/* Flag to track if alignment warning has been logged (to avoid log spam) */
	bool				alignment_warned;

	/* Rebuild state and context (legacy, kept for compatibility) */
	struct ec_rebuild_context	*rebuild_ctx;
	
	/* EC bdev background process, e.g. rebuild (new process framework) */
	struct ec_bdev_process		*process;
	
	/* Device selection strategy configuration */
	struct ec_device_selection_config selection_config;
};

#define EC_FOR_EACH_BASE_BDEV(e, i) \
	for (i = e->base_bdev_info; i < e->base_bdev_info + e->num_base_bdevs; i++)

/*
 * ec_bdev_io_channel is the context of spdk_io_channel for EC bdev device. It
 * contains the relationship of EC bdev io channel with base bdev io channels.
 */
/* Buffer pool entry for parity buffers */
struct ec_parity_buf_entry {
	SLIST_ENTRY(ec_parity_buf_entry)	link;
	unsigned char			*buf;
};

struct ec_bdev_io_channel {
	/* Array of IO channels of base bdevs */
	struct spdk_io_channel	**base_channel;
	
	/* Buffer pool for parity buffers to avoid frequent allocation/deallocation */
	SLIST_HEAD(, ec_parity_buf_entry)	parity_buf_pool;
	
	/* Buffer pool for RMW stripe buffers */
	SLIST_HEAD(, ec_parity_buf_entry)	rmw_stripe_buf_pool;
	
	/* Optimized: Buffer pool for temporary data buffers (cross-iov data) */
	SLIST_HEAD(, ec_parity_buf_entry)	temp_data_buf_pool;
	
	/* Number of buffers in pool */
	uint32_t			parity_buf_count;
	uint32_t			rmw_buf_count;
	uint32_t			temp_data_buf_count;
	
	/* Buffer size for parity buffers */
	uint32_t			parity_buf_size;
	
	/* Buffer size for RMW stripe buffers */
	uint32_t			rmw_buf_size;
	
	/* Buffer size for temporary data buffers */
	uint32_t			temp_data_buf_size;
	
	/* Optimized: Cached alignment value to avoid repeated lookups from ec_bdev
	 * This reduces memory access overhead in hot path (buffer allocation)
	 */
	size_t				cached_alignment;
	
	/* RAID5F-style object pool for stripe_private structures
	 * Pre-allocated stripe_private objects to avoid malloc overhead
	 */
	TAILQ_HEAD(, ec_stripe_private)	free_stripe_privs;
	
	/* RAID5F-style: For async encoding retry queue (if encoding resources unavailable) */
	TAILQ_HEAD(, ec_stripe_private)	encode_retry_queue;
	
	/* Background process data */
	struct {
		uint64_t offset;
		struct spdk_io_channel *target_ch;
		struct ec_bdev_io_channel *ch_processed;
	} process;
};

/* TAIL head for EC bdev list */
TAILQ_HEAD(ec_all_tailq, ec_bdev);

extern struct ec_all_tailq		g_ec_bdev_list;

/*
 * EC bdev module private data
 */
struct ec_bdev_module_private {
	/* Encode matrix for EC encoding */
	unsigned char *encode_matrix;

	/* Decode tables for EC decoding */
	unsigned char *g_tbls;

	/* Decode matrix for EC decoding */
	unsigned char *decode_matrix;

	/* Temporary matrix for decode operations */
	unsigned char *temp_matrix;

	/* Invert matrix for decode operations */
	unsigned char *invert_matrix;

	/* Decode index array */
	unsigned char decode_index[EC_MAX_K + EC_MAX_P];

	/* Dedicated encoding worker threads (optional) */
	struct {
		struct spdk_thread *threads[EC_MAX_ENCODE_WORKERS];
		uint32_t count;
		uint32_t next_rr;
		uint32_t active_tasks;
		bool enabled;
	} encode_workers;
};

int ec_bdev_create(const char *name, uint32_t strip_size, uint8_t k, uint8_t p,
		   bool superblock, const struct spdk_uuid *uuid,
		   struct ec_bdev **ec_bdev_out);
void ec_bdev_delete(struct ec_bdev *ec_bdev, bool wipe_sb, ec_bdev_destruct_cb cb_fn, void *cb_ctx);
int ec_bdev_add_base_bdev(struct ec_bdev *ec_bdev, const char *name,
			  ec_base_bdev_cb cb_fn, void *cb_ctx);
struct ec_bdev *ec_bdev_find_by_name(const char *name);
enum ec_bdev_state ec_bdev_str_to_state(const char *str);
const char *ec_bdev_state_to_str(enum ec_bdev_state state);
void ec_bdev_write_info_json(struct ec_bdev *ec_bdev, struct spdk_json_write_ctx *w);
void ec_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);
int ec_bdev_remove_base_bdev(struct spdk_bdev *base_bdev, ec_base_bdev_cb cb_fn, void *cb_ctx);
void ec_bdev_fail_base_bdev(struct ec_base_bdev_info *base_info);
int ec_bdev_gen_decode_matrix(struct ec_bdev *ec_bdev, uint8_t *frag_err_list, int nerrs);

/*
 * Definitions related to EC bdev superblock
 */
#define EC_BDEV_SB_VERSION_MAJOR	1
#define EC_BDEV_SB_VERSION_MINOR	1
#define EC_BDEV_SB_NAME_SIZE		64
#define EC_BDEV_SB_MAX_SUPPORTED_LENGTH	(64ULL * 1024 * 1024)

enum ec_bdev_sb_base_bdev_state {
	EC_SB_BASE_BDEV_MISSING	= 0,
	EC_SB_BASE_BDEV_CONFIGURED	= 1,
	EC_SB_BASE_BDEV_FAILED	= 2,
	EC_SB_BASE_BDEV_SPARE		= 3,
	EC_SB_BASE_BDEV_REBUILDING	= 4,	/* Rebuild in progress but not completed */
};

struct ec_bdev_sb_base_bdev {
	/* uuid of the base bdev */
	struct spdk_uuid	uuid;
	/* offset in blocks from base device start to the start of EC data area */
	uint64_t		data_offset;
	/* size in blocks of the base device EC data area */
	uint64_t		data_size;
	/* state of the base bdev */
	uint32_t		state;
	/* feature/status flags */
	uint32_t		flags;
	/* slot number of this base bdev in the EC */
	uint8_t			slot;
	/* whether this is a data block (true) or parity block (false) */
	bool			is_data_block;

	uint8_t			reserved[22];
};
SPDK_STATIC_ASSERT(sizeof(struct ec_bdev_sb_base_bdev) == 64, "incorrect size");

struct ec_bdev_superblock {
#define EC_BDEV_SB_SIG "SPDKEC  "
	uint8_t			signature[8];
	struct {
		/* incremented when a breaking change in the superblock structure is made */
		uint16_t	major;
		/* incremented for changes in the superblock that are backward compatible */
		uint16_t	minor;
	} version;
	/* length in bytes of the entire superblock */
	uint32_t		length;
	/* crc32c checksum of the entire superblock */
	uint32_t		crc;
	/* feature/status flags */
	uint32_t		flags;
	/* unique id of the EC bdev */
	struct spdk_uuid	uuid;
	/* name of the EC bdev */
	uint8_t			name[EC_BDEV_SB_NAME_SIZE];
	/* size of the EC bdev in blocks */
	uint64_t		ec_size;
	/* the EC bdev block size - must be the same for all base bdevs */
	uint32_t		block_size;
	/* strip (chunk) size in blocks */
	uint32_t		strip_size;
	/* state of the EC */
	uint32_t		state;
	/* sequence number, incremented on every superblock update */
	uint64_t		seq_number;
	/* number of EC base devices */
	uint8_t			num_base_bdevs;
	/* number of data blocks (k) */
	uint8_t			k;
	/* number of parity blocks (p) */
	uint8_t			p;

	struct {
		uint8_t			wear_leveling_enabled;
		uint8_t			stripe_group_size;
		uint16_t		reserved16;
		uint32_t		selection_seed;
	} wear_cfg;

	/* Extended metadata offsets (relative to start of superblock buffer) */
	uint32_t		profile_region_offset;
	uint32_t		profile_region_length;
	uint32_t		group_map_offset;
	uint32_t		group_map_length;
	uint32_t		latest_profile_id;
	uint32_t		total_profile_slots;

	uint8_t			reserved[83];

	/* size of the base bdevs array */
	uint8_t			base_bdevs_size;
	/* array of base bdev descriptors */
	struct ec_bdev_sb_base_bdev base_bdevs[];
};
SPDK_STATIC_ASSERT(sizeof(struct ec_bdev_superblock) == 256, "incorrect size");

#define EC_BDEV_SB_BASE_SECTION_LENGTH (sizeof(struct ec_bdev_superblock) + UINT8_MAX * sizeof(struct ec_bdev_sb_base_bdev))

typedef void (*ec_bdev_write_sb_cb)(int status, struct ec_bdev *ec_bdev, void *ctx);
typedef void (*ec_bdev_load_sb_cb)(const struct ec_bdev_superblock *sb, int status, void *ctx);

int ec_bdev_alloc_superblock(struct ec_bdev *ec_bdev, uint32_t block_size);
void ec_bdev_free_superblock(struct ec_bdev *ec_bdev);
void ec_bdev_init_superblock(struct ec_bdev *ec_bdev);
void ec_bdev_write_superblock(struct ec_bdev *ec_bdev, ec_bdev_write_sb_cb cb,
			       void *cb_ctx);
void ec_bdev_wipe_superblock(struct ec_bdev *ec_bdev, ec_bdev_write_sb_cb cb,
			     void *cb_ctx);
int ec_bdev_sb_reserve_buffer(struct ec_bdev *ec_bdev, size_t required_length);
void ec_bdev_sb_save_selection_metadata(struct ec_bdev *ec_bdev);
void ec_bdev_sb_load_selection_metadata(struct ec_bdev *ec_bdev,
					const struct ec_bdev_superblock *sb);
void ec_bdev_sb_load_wear_leveling_config(const struct ec_bdev_superblock *sb,
					  struct ec_device_selection_config *config);
int ec_bdev_wipe_single_base_bdev_superblock(struct ec_base_bdev_info *base_info,
					     ec_base_bdev_cb cb, void *cb_ctx);
int ec_bdev_load_base_bdev_superblock(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
				      ec_bdev_load_sb_cb cb, void *cb_ctx);
bool ec_bdev_sb_update_base_bdev_state(struct ec_bdev *ec_bdev, uint8_t slot,
					enum ec_bdev_sb_base_bdev_state new_state);
const struct ec_bdev_sb_base_bdev *ec_bdev_sb_find_base_bdev_by_slot(struct ec_bdev *ec_bdev, uint8_t slot);
const struct ec_bdev_sb_base_bdev *ec_bdev_sb_find_base_bdev_by_uuid(const struct ec_bdev_superblock *sb,
								      const struct spdk_uuid *uuid);

/* Lifecycle helpers */
void ec_bdev_free(struct ec_bdev *ec_bdev);

/* I/O completion and management functions */
void ec_bdev_io_complete(struct ec_bdev_io *ec_io, enum spdk_bdev_io_status status);
bool ec_bdev_io_complete_part(struct ec_bdev_io *ec_io, uint64_t completed,
			      enum spdk_bdev_io_status status);
void ec_bdev_io_set_default_status(struct ec_bdev_io *ec_io, enum spdk_bdev_io_status status);
void ec_bdev_queue_io_wait(struct ec_bdev_io *ec_io, struct spdk_bdev *bdev,
			  struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn);
void ec_bdev_io_init(struct ec_bdev_io *ec_io, struct ec_bdev_io_channel *ec_ch,
		    enum spdk_bdev_io_type type, uint64_t offset_blocks,
		    uint64_t num_blocks, struct iovec *iovs, int iovcnt, void *md_buf,
		    struct spdk_memory_domain *memory_domain, void *memory_domain_ctx);

#endif /* SPDK_BDEV_EC_H */

