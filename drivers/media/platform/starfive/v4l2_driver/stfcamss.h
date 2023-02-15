// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021 StarFive Technology Co., Ltd.
 */
#ifndef STFCAMSS_H
#define STFCAMSS_H

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/reset.h>
#include <linux/clk.h>

enum sensor_type {
	SENSOR_VIN,
	SENSOR_ISP0,  // need replace sensor
	SENSOR_ISP1,  // need replace sensor
};

enum subdev_type {
	VIN_DEV_TYPE,
	ISP0_DEV_TYPE,
	ISP1_DEV_TYPE,
};

#include "stf_common.h"
#include "stf_dvp.h"
#include "stf_csi.h"
#include "stf_csiphy.h"
#include "stf_isp.h"
#include "stf_vin.h"

#define STF_PAD_SINK   0
#define STF_PAD_SRC    1
#define STF_PADS_NUM   2

enum port_num {
	CSI2RX0_PORT_NUMBER = 0,
	CSI2RX1_PORT_NUMBER,
	DVP_SENSOR_PORT_NUMBER,
	CSI2RX0_SENSOR_PORT_NUMBER,
	CSI2RX1_SENSOR_PORT_NUMBER
};

enum stf_clk_num {
	STFCLK_ISPCORE_2X = 0,
	STFCLK_ISP_AXI,
	STFCLK_NUM
};

enum stf_rst_num {
	STFRST_ISP_TOP_N = 0,
	STFRST_ISP_TOP_AXI,
	STFRST_WRAPPER_P,
	STFRST_WRAPPER_C,
	STFRST_PCLK,
	STFRST_SYS_CLK,
	STFRST_AXIRD,
	STFRST_AXIWR,
	STFRST_PIXEL_CLK_IF0,
	STFRST_PIXEL_CLK_IF1,
	STFRST_PIXEL_CLK_IF2,
	STFRST_PIXEL_CLK_IF3,
	STFRST_NUM
};

struct stfcamss_clk {
	struct clk *clk;
	const char *name;
};

struct stfcamss_rst {
	struct reset_control *rst;
	const char *name;
};

struct stfcamss {
	struct stf_vin_dev *vin;  // stfcamss phy res
	struct v4l2_device v4l2_dev;
	struct media_device media_dev;
	struct device *dev;
	struct stf_vin2_dev *vin_dev;  // subdev
	struct stf_dvp_dev *dvp_dev;   // subdev
	int csi_num;
	struct stf_csi_dev *csi_dev;   // subdev
	int csiphy_num;
	struct stf_csiphy_dev *csiphy_dev;   // subdev
	int isp_num;
	struct stf_isp_dev *isp_dev;   // subdev
	struct v4l2_async_notifier notifier;
	struct stfcamss_clk *sys_clk;
	int nclks;
	struct stfcamss_rst *sys_rst;
	int nrsts;
#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_entry;
	struct dentry *vin_debugfs;
#endif
};

struct stfcamss_async_subdev {
	struct v4l2_async_subdev asd;  // must be first
	enum port_num port;
	struct {
		struct dvp_cfg dvp;
		struct csi2phy_cfg csiphy;
	} interface;
};

extern struct media_entity *stfcamss_find_sensor(struct media_entity *entity);

#endif /* STFCAMSS_H */
