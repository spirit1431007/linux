/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * H/W layer of ISHTP provider device (ISH)
 *
 * Copyright (c) 2014-2016, Intel Corporation.
 */

#ifndef _ISHTP_HW_ISH_H_
#define _ISHTP_HW_ISH_H_

#include <linux/pci.h>
#include <linux/interrupt.h>
#include "hw-ish-regs.h"
#include "ishtp-dev.h"

#define PCI_DEVICE_ID_INTEL_ISH_CHV		0x22D8
#define PCI_DEVICE_ID_INTEL_ISH_BXT_Ax		0x0AA2
#define PCI_DEVICE_ID_INTEL_ISH_BXT_Bx		0x1AA2
#define PCI_DEVICE_ID_INTEL_ISH_APL_Ax		0x5AA2
#define PCI_DEVICE_ID_INTEL_ISH_SPT_Ax		0x9D35
#define PCI_DEVICE_ID_INTEL_ISH_CNL_Ax		0x9DFC
#define PCI_DEVICE_ID_INTEL_ISH_GLK_Ax		0x31A2
#define PCI_DEVICE_ID_INTEL_ISH_CNL_H		0xA37C
#define PCI_DEVICE_ID_INTEL_ISH_ICL_MOBILE	0x34FC
#define PCI_DEVICE_ID_INTEL_ISH_SPT_H		0xA135
#define PCI_DEVICE_ID_INTEL_ISH_CML_LP		0x02FC
#define PCI_DEVICE_ID_INTEL_ISH_CMP_H		0x06FC
#define PCI_DEVICE_ID_INTEL_ISH_EHL_Ax		0x4BB3
#define PCI_DEVICE_ID_INTEL_ISH_TGL_LP		0xA0FC
#define PCI_DEVICE_ID_INTEL_ISH_TGL_H		0x43FC
#define PCI_DEVICE_ID_INTEL_ISH_ADL_S		0x7AF8
#define PCI_DEVICE_ID_INTEL_ISH_ADL_P		0x51FC
#define PCI_DEVICE_ID_INTEL_ISH_ADL_N		0x54FC
#define PCI_DEVICE_ID_INTEL_ISH_RPL_S		0x7A78
#define PCI_DEVICE_ID_INTEL_ISH_MTL_P		0x7E45
#define PCI_DEVICE_ID_INTEL_ISH_ARL_H		0x7745
#define PCI_DEVICE_ID_INTEL_ISH_ARL_S		0x7F78
#define PCI_DEVICE_ID_INTEL_ISH_LNL_M		0xA845
#define PCI_DEVICE_ID_INTEL_ISH_PTL_H		0xE345
#define PCI_DEVICE_ID_INTEL_ISH_PTL_P		0xE445
#define PCI_DEVICE_ID_INTEL_ISH_WCL		0x4D45

#define	REVISION_ID_CHT_A0	0x6
#define	REVISION_ID_CHT_Ax_SI	0x0
#define	REVISION_ID_CHT_Bx_SI	0x10
#define	REVISION_ID_CHT_Kx_SI	0x20
#define	REVISION_ID_CHT_Dx_SI	0x30
#define	REVISION_ID_CHT_B0	0xB0
#define	REVISION_ID_SI_MASK	0x70

struct ipc_rst_payload_type {
	uint16_t	reset_id;
	uint16_t	reserved;
};

struct time_sync_format {
	uint8_t ts1_source;
	uint8_t ts2_source;
	uint16_t reserved;
} __packed;

struct ipc_time_update_msg {
	uint64_t primary_host_time;
	struct time_sync_format sync_info;
	uint64_t secondary_host_time;
} __packed;

enum {
	HOST_UTC_TIME_USEC = 0,
	HOST_SYSTEM_TIME_USEC = 1
};

struct ish_hw {
	void __iomem *mem_addr;
};

/*
 * ISH FW status type
 */
enum {
	FWSTS_AFTER_RESET		= 0,
	FWSTS_WAIT_FOR_HOST		= 4,
	FWSTS_START_KERNEL_DMA		= 5,
	FWSTS_FW_IS_RUNNING		= 7,
	FWSTS_SENSOR_APP_LOADED		= 8,
	FWSTS_SENSOR_APP_RUNNING	= 15
};

#define to_ish_hw(dev) (struct ish_hw *)((dev)->hw)

irqreturn_t ish_irq_handler(int irq, void *dev_id);
struct ishtp_device *ish_dev_init(struct pci_dev *pdev);
int ish_hw_start(struct ishtp_device *dev);
void ish_device_disable(struct ishtp_device *dev);
int ish_disable_dma(struct ishtp_device *dev);
void ish_set_host_ready(struct ishtp_device *dev);

#endif /* _ISHTP_HW_ISH_H_ */
