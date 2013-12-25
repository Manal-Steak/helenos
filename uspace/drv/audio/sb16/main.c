/*
 * Copyright (c) 2011 Jan Vesely
 * Copyright (c) 2011 Vojtech Horky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/** @addtogroup drvaudiosb16
 * @{
 */
/** @file
 * Main routines of Creative Labs SoundBlaster 16 driver
 */
#include <ddf/driver.h>
#include <ddf/interrupt.h>
#include <ddf/log.h>
#include <device/hw_res_parsed.h>
#include <devman.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <str_error.h>

#include "ddf_log.h"
#include "sb16.h"

#define NAME "sb16"

static int sb_add_device(ddf_dev_t *device);
static int sb_get_res(ddf_dev_t *device, addr_range_t **pp_sb_regs,
    addr_range_t **pp_mpu_regs, int *irq, int *dma8, int *dma16);
static int sb_enable_interrupts(ddf_dev_t *device);

static driver_ops_t sb_driver_ops = {
	.dev_add = sb_add_device,
};

static driver_t sb_driver = {
	.name = NAME,
	.driver_ops = &sb_driver_ops
};

/** Initialize global driver structures (NONE).
 *
 * @param[in] argc Nmber of arguments in argv vector (ignored).
 * @param[in] argv Cmdline argument vector (ignored).
 * @return Error code.
 *
 * Driver debug level is set here.
 */
int main(int argc, char *argv[])
{
	printf(NAME": HelenOS SB16 audio driver.\n");
	ddf_log_init(NAME);
	return ddf_driver_main(&sb_driver);
}

static void irq_handler(ddf_dev_t *dev, ipc_callid_t iid, ipc_call_t *call)
{
	sb16_t *sb16_dev = ddf_dev_data_get(dev);
	sb16_interrupt(sb16_dev);
}

/** Initialize new SB16 driver instance.
 *
 * @param[in] device DDF instance of the device to initialize.
 * @return Error code.
 */
static int sb_add_device(ddf_dev_t *device)
{
	bool handler_regd = false;
	const size_t irq_cmd_count = sb16_irq_code_size();
	irq_cmd_t irq_cmds[irq_cmd_count];
	irq_pio_range_t irq_ranges[1];

	sb16_t *soft_state = ddf_dev_data_alloc(device, sizeof(sb16_t));
	int rc = soft_state ? EOK : ENOMEM;
	if (rc != EOK) {
		ddf_log_error("Failed to allocate sb16 structure.");
		goto error;
	}

	addr_range_t sb_regs;
	addr_range_t *p_sb_regs = &sb_regs;
	addr_range_t mpu_regs;
	addr_range_t *p_mpu_regs = &mpu_regs;
	int irq = 0, dma8 = 0, dma16 = 0;

	rc = sb_get_res(device, &p_sb_regs, &p_mpu_regs, &irq, &dma8, &dma16);
	if (rc != EOK) {
		ddf_log_error("Failed to get resources: %s.", str_error(rc));
		goto error;
	}

	sb16_irq_code(p_sb_regs, dma8, dma16, irq_cmds, irq_ranges);

	irq_code_t irq_code = {
		.cmdcount = irq_cmd_count,
		.cmds = irq_cmds,
		.rangecount = 1,
		.ranges = irq_ranges
	};

	rc = register_interrupt_handler(device, irq, irq_handler, &irq_code);
	if (rc != EOK) {
		ddf_log_error("Failed to register irq handler: %s.",
		    str_error(rc));
		goto error;
	}

	handler_regd = true;

	rc = sb_enable_interrupts(device);
	if (rc != EOK) {
		ddf_log_error("Failed to enable interrupts: %s.",
		    str_error(rc));
		goto error;
	}

	rc = sb16_init_sb16(soft_state, p_sb_regs, device, dma8, dma16);
	if (rc != EOK) {
		ddf_log_error("Failed to init sb16 driver: %s.",
		    str_error(rc));
		goto error;
	}

	rc = sb16_init_mpu(soft_state, p_mpu_regs);
	if (rc == EOK) {
		ddf_fun_t *mpu_fun =
		    ddf_fun_create(device, fun_exposed, "midi");
		if (mpu_fun) {
			rc = ddf_fun_bind(mpu_fun);
			if (rc != EOK)
				ddf_log_error(
				    "Failed to bind midi function: %s.",
				    str_error(rc));
		} else {
			ddf_log_error("Failed to create midi function.");
		}
	} else {
		ddf_log_warning("Failed to init mpu driver: %s.",
		    str_error(rc));
	}

	/* MPU state does not matter */
	return EOK;
error:
	if (handler_regd)
		unregister_interrupt_handler(device, irq);
	return rc;
}

static int sb_get_res(ddf_dev_t *device, addr_range_t **pp_sb_regs,
    addr_range_t **pp_mpu_regs, int *irq, int *dma8, int *dma16)
{
	assert(device);

	async_sess_t *parent_sess = devman_parent_device_connect(
	    EXCHANGE_SERIALIZE, ddf_dev_get_handle(device), IPC_FLAG_BLOCKING);
	if (!parent_sess)
		return ENOMEM;

	hw_res_list_parsed_t hw_res;
	hw_res_list_parsed_init(&hw_res);
	const int ret = hw_res_get_list_parsed(parent_sess, &hw_res, 0);
	async_hangup(parent_sess);
	if (ret != EOK) {
		return ret;
	}

	/* 1x IRQ, 1-2x DMA(8,16), 1-2x IO (MPU is separate). */
	if (hw_res.irqs.count != 1 ||
	   (hw_res.io_ranges.count != 1 && hw_res.io_ranges.count != 2) ||
	   (hw_res.dma_channels.count != 1 && hw_res.dma_channels.count != 2)) {
		hw_res_list_parsed_clean(&hw_res);
		return EINVAL;
	}

	if (irq)
		*irq = hw_res.irqs.irqs[0];

	if (dma8) {
		if (hw_res.dma_channels.channels[0] < 4) {
			*dma8 = hw_res.dma_channels.channels[0];
		} else {
			if (hw_res.dma_channels.count == 2 &&
			    hw_res.dma_channels.channels[1] < 4) {
				*dma8 = hw_res.dma_channels.channels[1];
			}
		}
	}

	if (dma16) {
		if (hw_res.dma_channels.channels[0] > 4) {
			*dma16 = hw_res.dma_channels.channels[0];
		} else {
			if (hw_res.dma_channels.count == 2 &&
			    hw_res.dma_channels.channels[1] > 4) {
				*dma16 = hw_res.dma_channels.channels[1];
			}
		}
	}

	if (hw_res.io_ranges.count == 1) {
		if (pp_sb_regs && *pp_sb_regs)
			**pp_sb_regs = hw_res.io_ranges.ranges[0];
		if (pp_mpu_regs)
			*pp_mpu_regs = NULL;
	} else {
		const int sb =
		    (hw_res.io_ranges.ranges[0].size >= sizeof(sb16_regs_t))
		        ? 0 : 1;
		const int mpu = 1 - sb;
		if (pp_sb_regs && *pp_sb_regs)
			**pp_sb_regs = hw_res.io_ranges.ranges[sb];
		if (pp_mpu_regs && *pp_mpu_regs)
			**pp_mpu_regs = hw_res.io_ranges.ranges[mpu];
	}

	return EOK;
}

int sb_enable_interrupts(ddf_dev_t *device)
{
	async_sess_t *parent_sess = devman_parent_device_connect(
	    EXCHANGE_SERIALIZE, ddf_dev_get_handle(device), IPC_FLAG_BLOCKING);
	if (!parent_sess)
		return ENOMEM;

	bool enabled = hw_res_enable_interrupt(parent_sess);
	async_hangup(parent_sess);

	return enabled ? EOK : EIO;
}

/**
 * @}
 */
