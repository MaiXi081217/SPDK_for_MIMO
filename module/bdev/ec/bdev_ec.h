/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_BDEV_EC_H
#define SPDK_BDEV_EC_H

#include "spdk/bdev_module.h"
#include "spdk/uuid.h"
#include <isa-l/erasure_code.h>

#define EC_BDEV_MIN_DATA_OFFSET_SIZE	(1024*1024) /* 1 MiB */

/* Maximum k and p values for EC */
#define EC_MAX_K 255
#define EC_MAX_P 255

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

	/* Extension interface for external modules (e.g., FTL for wear leveling) */
	struct ec_bdev_extension_if	*extension_if;

	/* Buffer alignment requirement for memory allocations */
	size_t				buf_alignment;

	/* Flag to track if alignment warning has been logged (to avoid log spam) */
	bool				alignment_warned;

	/* Rebuild state and context */
	struct ec_rebuild_context	*rebuild_ctx;
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
	
	/* Number of buffers in pool */
	uint32_t			parity_buf_count;
	uint32_t			rmw_buf_count;
	
	/* Buffer size for parity buffers */
	uint32_t			parity_buf_size;
	
	/* Buffer size for RMW stripe buffers */
	uint32_t			rmw_buf_size;
};

/* TAIL head for EC bdev list */
TAILQ_HEAD(ec_all_tailq, ec_bdev);

extern struct ec_all_tailq		g_ec_bdev_list;

/*
 * EC bdev extension interface for external modules (e.g., FTL for wear leveling)
 * This interface allows external modules to control I/O distribution based on
 * wear leveling or other policies.
 */
struct ec_bdev_extension_if;

/*
 * Callback to select base bdev indices for data blocks based on wear leveling
 * or other policies. This allows external modules (e.g., FTL) to control
 * which disks receive data based on wear statistics.
 * params:
 * ext_if - extension interface
 * ec_bdev - EC bdev
 * offset_blocks - logical offset in blocks
 * num_blocks - number of blocks
 * data_indices - output array of base bdev indices for data blocks (size k)
 * parity_indices - output array of base bdev indices for parity blocks (size p)
 * ctx - extension context
 * returns:
 * 0 on success, non-zero on failure
 */
typedef int (*ec_ext_select_base_bdevs_fn)(struct ec_bdev_extension_if *ext_if,
					    struct ec_bdev *ec_bdev,
					    uint64_t offset_blocks,
					    uint32_t num_blocks,
					    uint8_t *data_indices,
					    uint8_t *parity_indices,
					    void *ctx);

/*
 * Callback to get wear level information for a base bdev
 * This allows external modules to provide wear statistics
 * params:
 * ext_if - extension interface
 * ec_bdev - EC bdev
 * base_info - base bdev info
 * wear_level - output wear level (0-100, 0 = no wear, 100 = maximum wear)
 * ctx - extension context
 * returns:
 * 0 on success, non-zero on failure
 */
typedef int (*ec_ext_get_wear_level_fn)(struct ec_bdev_extension_if *ext_if,
					 struct ec_bdev *ec_bdev,
					 struct ec_base_bdev_info *base_info,
					 uint8_t *wear_level,
					 void *ctx);

/*
 * Callback to notify I/O completion for wear level tracking
 * This allows external modules to update wear statistics
 * params:
 * ext_if - extension interface
 * ec_bdev - EC bdev
 * base_info - base bdev info that handled the I/O
 * offset_blocks - logical offset in blocks
 * num_blocks - number of blocks
 * is_write - true if write operation, false if read
 * ctx - extension context
 * returns:
 * none
 */
typedef void (*ec_ext_notify_io_complete_fn)(struct ec_bdev_extension_if *ext_if,
					      struct ec_bdev *ec_bdev,
					      struct ec_base_bdev_info *base_info,
					      uint64_t offset_blocks,
					      uint32_t num_blocks,
					      bool is_write,
					      void *ctx);

/*
 * EC bdev extension interface
 * External modules (e.g., FTL) can register this interface to control
 * I/O distribution and wear leveling.
 */
struct ec_bdev_extension_if {
	/* Name of the extension module */
	const char *name;

	/* Context for extension module */
	void *ctx;

	/* Callback to select base bdevs for data/parity blocks */
	ec_ext_select_base_bdevs_fn select_base_bdevs;

	/* Callback to get wear level information */
	ec_ext_get_wear_level_fn get_wear_level;

	/* Callback to notify I/O completion */
	ec_ext_notify_io_complete_fn notify_io_complete;

	/* Optional: callback to initialize extension */
	int (*init)(struct ec_bdev_extension_if *ext_if, struct ec_bdev *ec_bdev);

	/* Optional: callback to cleanup extension */
	void (*fini)(struct ec_bdev_extension_if *ext_if, struct ec_bdev *ec_bdev);
};

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

/* Extension interface functions */
int ec_bdev_register_extension(struct ec_bdev *ec_bdev, struct ec_bdev_extension_if *ext_if);
void ec_bdev_unregister_extension(struct ec_bdev *ec_bdev);
struct ec_bdev_extension_if *ec_bdev_get_extension(struct ec_bdev *ec_bdev);

/*
 * Definitions related to EC bdev superblock
 */
#define EC_BDEV_SB_VERSION_MAJOR	1
#define EC_BDEV_SB_VERSION_MINOR	0
#define EC_BDEV_SB_NAME_SIZE		64

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

	uint8_t			reserved[115];

	/* size of the base bdevs array */
	uint8_t			base_bdevs_size;
	/* array of base bdev descriptors */
	struct ec_bdev_sb_base_bdev base_bdevs[];
};
SPDK_STATIC_ASSERT(sizeof(struct ec_bdev_superblock) == 256, "incorrect size");

#define EC_BDEV_SB_MAX_LENGTH (sizeof(struct ec_bdev_superblock) + UINT8_MAX * sizeof(struct ec_bdev_sb_base_bdev))

SPDK_STATIC_ASSERT(EC_BDEV_SB_MAX_LENGTH < EC_BDEV_MIN_DATA_OFFSET_SIZE,
		   "Incorrect min data offset");

typedef void (*ec_bdev_write_sb_cb)(int status, struct ec_bdev *ec_bdev, void *ctx);
typedef void (*ec_bdev_load_sb_cb)(const struct ec_bdev_superblock *sb, int status, void *ctx);

int ec_bdev_alloc_superblock(struct ec_bdev *ec_bdev, uint32_t block_size);
void ec_bdev_free_superblock(struct ec_bdev *ec_bdev);
void ec_bdev_init_superblock(struct ec_bdev *ec_bdev);
void ec_bdev_write_superblock(struct ec_bdev *ec_bdev, ec_bdev_write_sb_cb cb,
			       void *cb_ctx);
void ec_bdev_wipe_superblock(struct ec_bdev *ec_bdev, ec_bdev_write_sb_cb cb,
			     void *cb_ctx);
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

