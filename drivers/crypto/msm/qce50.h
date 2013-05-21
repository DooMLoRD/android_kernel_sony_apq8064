/* Copyright (c) 2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef _DRIVERS_CRYPTO_MSM_QCE50_H_
#define _DRIVERS_CRYPTO_MSM_QCE50_H_

#include <mach/sps.h>

/* MAX Data xfer block size between BAM and CE */
#define MAX_CE_BAM_BURST_SIZE   0x40
#define QCEBAM_BURST_SIZE	MAX_CE_BAM_BURST_SIZE
#define MAX_BAM_DESCRIPTORS	(0x40 - 1)

#define GET_VIRT_ADDR(x)  \
		((uint32_t)pce_dev->coh_vmem +			\
		((uint32_t)x - pce_dev->coh_pmem))
#define GET_PHYS_ADDR(x)  \
		(pce_dev->coh_pmem + (x - (uint32_t)pce_dev->coh_vmem))

#define CRYPTO_REG_SIZE 4
#define NUM_OF_CRYPTO_AUTH_IV_REG 16
#define NUM_OF_CRYPTO_CNTR_IV_REG 4
#define NUM_OF_CRYPTO_AUTH_BYTE_COUNT_REG 4
#define CRYPTO_TOTAL_REGISTERS_DUMPED   26
#define CRYPTO_RESULT_DUMP_SIZE   \
	ALIGN((CRYPTO_TOTAL_REGISTERS_DUMPED * CRYPTO_REG_SIZE), \
	QCEBAM_BURST_SIZE)

/* QCE max number of descriptor in a descriptor list */
#define QCE_MAX_NUM_DESC    128
#define SPS_MAX_PKT_SIZE  (64 * 1024  - 1)

/* State of consumer/producer Pipe */
enum qce_pipe_st_enum {
	QCE_PIPE_STATE_IDLE = 0,
	QCE_PIPE_STATE_IN_PROG = 1,
	QCE_PIPE_STATE_COMP = 2,
	QCE_PIPE_STATE_LAST
};

struct qce_sps_ep_conn_data {
	struct sps_pipe			*pipe;
	struct sps_connect		connect;
	struct sps_register_event	event;
};

/* CE Result DUMP format*/
struct ce_result_dump_format {
	uint32_t auth_iv[NUM_OF_CRYPTO_AUTH_IV_REG];
	uint32_t auth_byte_count[NUM_OF_CRYPTO_AUTH_BYTE_COUNT_REG];
	uint32_t encr_cntr_iv[NUM_OF_CRYPTO_CNTR_IV_REG];
	uint32_t status;
	uint32_t status2;
};

struct qce_cmdlist_info {

	uint32_t cmdlist;
	struct sps_command_element *crypto_cfg;
	struct sps_command_element *encr_seg_cfg;
	struct sps_command_element *encr_seg_size;
	struct sps_command_element *encr_seg_start;
	struct sps_command_element *encr_key;
	struct sps_command_element *encr_xts_key;
	struct sps_command_element *encr_cntr_iv;
	struct sps_command_element *encr_ccm_cntr_iv;
	struct sps_command_element *encr_mask;
	struct sps_command_element *encr_xts_du_size;

	struct sps_command_element *auth_seg_cfg;
	struct sps_command_element *auth_seg_size;
	struct sps_command_element *auth_seg_start;
	struct sps_command_element *auth_key;
	struct sps_command_element *auth_iv;
	struct sps_command_element *auth_nonce_info;
	struct sps_command_element *auth_bytecount;
	struct sps_command_element *seg_size;
	struct sps_command_element *go_proc;
	uint32_t size;
};

struct qce_cmdlistptr_ops {
	struct qce_cmdlist_info cipher_aes_128_cbc_ctr;
	struct qce_cmdlist_info cipher_aes_256_cbc_ctr;
	struct qce_cmdlist_info cipher_aes_128_ecb;
	struct qce_cmdlist_info cipher_aes_256_ecb;
	struct qce_cmdlist_info cipher_aes_128_xts;
	struct qce_cmdlist_info cipher_aes_256_xts;
	struct qce_cmdlist_info cipher_des_cbc;
	struct qce_cmdlist_info cipher_des_ecb;
	struct qce_cmdlist_info cipher_3des_cbc;
	struct qce_cmdlist_info cipher_3des_ecb;
	struct qce_cmdlist_info auth_sha1;
	struct qce_cmdlist_info auth_sha256;
	struct qce_cmdlist_info auth_sha1_hmac;
	struct qce_cmdlist_info auth_sha256_hmac;
	struct qce_cmdlist_info auth_aes_128_cmac;
	struct qce_cmdlist_info auth_aes_256_cmac;
	struct qce_cmdlist_info aead_sha1_hmac;
	struct qce_cmdlist_info aead_aes_128_ccm;
	struct qce_cmdlist_info aead_aes_256_ccm;
	struct qce_cmdlist_info unlock_all_pipes;
};


/* DM data structure with buffers, commandlists & commmand pointer lists */
struct ce_sps_data {

	uint32_t			bam_irq;
	uint32_t			bam_mem;
	void __iomem			*bam_iobase;

	struct qce_sps_ep_conn_data	producer;
	struct qce_sps_ep_conn_data	consumer;
	struct sps_event_notify		notify;
	struct scatterlist		*src;
	struct scatterlist		*dst;
	unsigned int			pipe_pair_index;
	unsigned int			src_pipe_index;
	unsigned int			dest_pipe_index;
	uint32_t			bam_handle;

	enum qce_pipe_st_enum consumer_state;	/* Consumer pipe state */
	enum qce_pipe_st_enum producer_state;	/* Producer pipe state */

	int consumer_status;		/* consumer pipe status */
	int producer_status;		/* producer pipe status */

	struct sps_transfer in_transfer;
	struct sps_transfer out_transfer;

	int ce_burst_size;

	struct qce_cmdlistptr_ops cmdlistptr;
	uint32_t result_dump;
	uint32_t ignore_buffer;
	struct ce_result_dump_format *result;
	uint32_t minor_version;
};
#endif /* _DRIVERS_CRYPTO_MSM_QCE50_H */
