#ifndef TESTS_STUBS_LINUX_SPI_SPI_H_
#define TESTS_STUBS_LINUX_SPI_SPI_H_

#include <stddef.h>
#include <drm/drm_drv.h>

struct spi_device {
	struct device dev;
	unsigned int chip_select[1];
	size_t max_transfer_size;
	size_t max_message_size;
	unsigned int sync_calls;
	size_t sync_first_len[32];
	unsigned int sync_num_xfers[32];
};

struct spi_transfer {
	const void *tx_buf;
	size_t len;
};

struct spi_message {
	int unused;
};

static inline void spi_message_init(struct spi_message *msg)
{
	(void)msg;
}

static inline void spi_message_add_tail(struct spi_transfer *xfer,
	struct spi_message *msg)
{
	(void)xfer;
	(void)msg;
}

static inline int spi_sync_transfer(struct spi_device *spi,
	struct spi_transfer *xfers, unsigned int num_xfers)
{
	unsigned int call = spi->sync_calls++;

	if (call < 32) {
		spi->sync_first_len[call] = num_xfers ? xfers[0].len : 0;
		spi->sync_num_xfers[call] = num_xfers;
	}
	return 0;
}

static inline int spi_sync(struct spi_device *spi, struct spi_message *msg)
{
	(void)spi;
	(void)msg;
	return 0;
}

static inline size_t spi_max_transfer_size(struct spi_device *spi)
{
	return spi->max_transfer_size;
}

static inline size_t spi_max_message_size(struct spi_device *spi)
{
	return spi->max_message_size;
}

#endif
