/*
 * Copyright CERN 2013
 * Author: Federico Vaga <federico.vaga@gmail.com>
 */
#ifndef FMCADC_LIB_INT_H_
#define FMCADC_LIB_INT_H_

/*
 * offsetof and container_of come from kernel.h header file
 */
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#define container_of(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = ((void *)ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})
#define to_dev_zio(dev) (container_of(dev, struct __fmcadc_dev_zio, gid))

/* ->open takes different args than open(), so fa a fun to use tpyeof */
struct fmcadc_board_type;
struct fmcadc_dev *fmcadc_internal_open(const struct fmcadc_board_type *b,
					unsigned int dev_id,
					unsigned long totalsize,
					unsigned int nbuffer,
					unsigned long flags);

/*
 * The operations structure is the device-specific backend of the library
 */
struct fmcadc_operations {
	typeof(fmcadc_internal_open)	*open;
	typeof(fmcadc_close)		*close;

	/* Handle acquisition */
	int (*start_acquisition)(struct fmcadc_dev *dev,
				 unsigned int flags,
				 struct timeval *timeout);
	int (*stop_acquisition)(struct fmcadc_dev *dev,
				unsigned int flags);
	/* Handle configuration */
	int (*apply_config)(struct fmcadc_dev *dev,
			    unsigned int flags,
			    struct fmcadc_conf *conf);
	int (*retrieve_config)(struct fmcadc_dev *dev,
			       struct fmcadc_conf *conf);
	/* Handle buffers */
	struct fmcadc_buffer *(*request_buffer)(struct fmcadc_dev *dev,
					       int nsamples,
					       void *(*alloc_fn)(size_t),
					       unsigned int flags,
					       struct timeval *timeout);
	int (*release_buffer)(struct fmcadc_dev *dev,
			      struct fmcadc_buffer *buf,
			      void (*free_fn)(void *));
	char *(*strerror)(int errnum);
};
/*
 * This structure describes the board supported by the library
 * @name name of the board type, for example "fmc-adc-100MS"
 * @devname name of the device in Linux
 * @driver_type: the kind of driver that hanlde this kind of board (e.g. ZIO)
 * @capabilities bitmask of device capabilities for trigger, channel
 *               acquisition
 * @fa_op pointer to a set of operations
 */
struct fmcadc_board_type {
	char			*name;
	char			*devname;
	char			*driver_type;
	uint32_t		capabilities[__FMCADC_CONF_TYPE_LAST_INDEX];
	struct fmcadc_operations *fa_op;
};

/*
 * Generic Instance Descriptor
 */
struct fmcadc_gid {
	const struct fmcadc_board_type *board;
};

/* Definition of board types */
extern struct fmcadc_board_type fmcadc_100ms_4ch_14bit;

/* Internal structure (ZIO specific, for ZIO drivers only) */
struct __fmcadc_dev_zio {
	unsigned int cset;
	int fdc;
	int fdd;
	uint32_t dev_id;
	unsigned long flags;
	char *devbase;
	char *sysbase;
	/* Mandatory field */
	struct fmcadc_gid gid;
};
#define FMCADC_FLAG_VERBOSE 0x00000001

/* The board-specific functions are defined in fmc-adc-100m14b4cha.c */
struct fmcadc_dev *fmcadc_zio_open(const struct fmcadc_board_type *b,
				   unsigned int dev_id,
				   unsigned long totalsize,
				   unsigned int nbuffer,
				   unsigned long flags);
int fmcadc_zio_close(struct fmcadc_dev *dev);

int fmcadc_zio_start_acquisition(struct fmcadc_dev *dev,
				 unsigned int flags, struct timeval *timeout);
int fmcadc_zio_stop_acquisition(struct fmcadc_dev *dev,
				unsigned int flags);
struct fmcadc_buffer *fmcadc_zio_request_buffer(struct fmcadc_dev *dev,
						int nsamples,
						void *(*alloc)(size_t),
						unsigned int flags,
						struct timeval *timeout);
int fmcadc_zio_release_buffer(struct fmcadc_dev *dev,
			      struct fmcadc_buffer *buf,
			      void (*free_fn)(void *));

/* The following functions are in config-zio.c */
int fmcadc_zio_apply_config(struct fmcadc_dev *dev, unsigned int flags,
			    struct fmcadc_conf *conf);
int fmcadc_zio_retrieve_config(struct fmcadc_dev *dev,
			       struct fmcadc_conf *conf);
int fa_zio_sysfs_set(struct __fmcadc_dev_zio *fa, char *name,
		     uint32_t *value);

#endif /* FMCADC_LIB_INT_H_ */
