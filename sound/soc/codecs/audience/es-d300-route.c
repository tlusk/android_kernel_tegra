#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/time.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include "escore.h"
#include "es755.h"
#include "es-d300.h"
#include "es-d300-route.h"

enum {
	VP_PRIMARY_MUX,
	VP_SECONDARY_MUX,
	VP_TERTIARY_MUX,
	VP_AECREF_MUX,
	VP_FEIN_MUX,
	VP_UI1_MUX,
	VP_UI2_MUX,
	VP_IN_MUX_LEN,
};

enum {
	VP_FEOUT1_MUX,
	VP_FEOUT2_MUX,
	VP_CSOUT1_MUX,
	VP_CSOUT2_MUX,
	VP_OUT_MUX_LEN,
};

static struct route_tbl es_vp_in_route_tbl[VP_IN_MUX_LEN] = {
	[VP_PRIMARY_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_PATH_CMD,
				(ES300_DATA_PATH(0, 0, RXCHMGR0))),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0000),
		.mux_type = ES_PRIMARY_MUX,
		.cmd_len = 2,
		.chn_mgr_mask = BIT(RXCHMGR0),
	},
	[VP_SECONDARY_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_PATH_CMD,
				(ES300_DATA_PATH(0, 0, RXCHMGR1))),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0101),
		.cmd[2] = ES_API_WORD(0xB064, 0x0048),
		.cmd[3] = ES_API_WORD(0xB064, 0x0132),
		.cmd[4] = ES_API_WORD(0xB063, 0x0104),
		.cmd[5] = ES_API_WORD(0xB068, 0x0400),
		.mux_type = ES_SECONDARY_MUX,
		.cmd_len = 6,
		.chn_mgr_mask = BIT(RXCHMGR1),
	},
	[VP_TERTIARY_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_PATH_CMD,
				(ES300_DATA_PATH(0, 0, RXCHMGR2))),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0202),
		.cmd[2] = ES_API_WORD(0xB064, 0x0058),
		.cmd[3] = ES_API_WORD(0xB064, 0x0133),
		.cmd[4] = ES_API_WORD(0xB063, 0x0105),
		.cmd[5] = ES_API_WORD(0xB068, 0x0500),
		.mux_type = ES_TERTIARY_MUX,
		.cmd_len = 6,
		.chn_mgr_mask = BIT(RXCHMGR2),
	},
	[VP_AECREF_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_PATH_CMD,
				(ES300_DATA_PATH(0, 0, RXCHMGR4))),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0404),
		.cmd[2] = ES_API_WORD(0xB064, 0x0078),
		.cmd[3] = ES_API_WORD(0xB064, 0x0134),
		.cmd[4] = ES_API_WORD(0xB063, 0x0107),
		.cmd[5] = ES_API_WORD(0xB068, 0x0700),
		.mux_type = ES_AECREF1_MUX,
		.cmd_len = 6,
		.chn_mgr_mask = BIT(RXCHMGR4),
	},
	[VP_FEIN_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_PATH_CMD,
				(ES300_DATA_PATH(0, 0, RXCHMGR3))),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0303),
		.mux_type = ES_FEIN_MUX,
		.cmd_len = 2,
		.chn_mgr_mask = BIT(RXCHMGR3),
	},
	[VP_UI1_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_PATH_CMD,
				(ES300_DATA_PATH(0, 0, RXCHMGR5))),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0509),
		.mux_type = ES_UITONE1_MUX,
		.cmd_len = 2,
		.chn_mgr_mask = BIT(RXCHMGR5),
	},
	[VP_UI2_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_PATH_CMD,
				(ES300_DATA_PATH(0, 0, RXCHMGR6))),
		.cmd[1] = ES_API_WORD(0xB05B, 0x060A),
		.mux_type = ES_UITONE2_MUX,
		.cmd_len = 2,
		.chn_mgr_mask = BIT(RXCHMGR6),
	},
};

static struct route_tbl es_vp_out_route_tbl[VP_OUT_MUX_LEN] = {
	[VP_FEOUT1_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, TXCHMGR2)),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0C22),
		.mux_type = VP_FEOUT1,
		.cmd_len = 2,
		.chn_mgr_mask = BIT(TXCHMGR2),
	},
	[VP_FEOUT2_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, TXCHMGR3)),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0D23),
		.mux_type = VP_FEOUT2,
		.cmd_len = 2,
		.chn_mgr_mask = BIT(TXCHMGR3),
	},
	[VP_CSOUT1_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, TXCHMGR0)),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0A20),
		.mux_type = VP_CSOUT1,
		.cmd_len = 2,
		.chn_mgr_mask = BIT(TXCHMGR0),
	},
	[VP_CSOUT2_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, TXCHMGR1)),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0B21),
		.mux_type = VP_CSOUT2,
		.cmd_len = 2,
		.chn_mgr_mask = BIT(TXCHMGR1),
	},
};

struct es_mux_info es_vp_mux_info = {
	.in_mux_start = VP_PRIMARY_MUX,
	.in_mux_len = VP_IN_MUX_LEN,
	.out_mux_start = VP_FEOUT1_MUX,
	.out_mux_len = VP_OUT_MUX_LEN,
	.in_tbl = &es_vp_in_route_tbl,
	.out_tbl = &es_vp_out_route_tbl,
};



/* VP_PT Mux ENUM */
enum {
	PT_VP_PASSIN1_MUX,
	PT_VP_PASSIN2_MUX,
	PT_VP_PRIMARY_MUX,
	PT_VP_SECONDARY_MUX,
	PT_VP_FEIN_MUX,
	PT_VP_IN_MUX_LEN,
};
enum {
	PT_VP_PASSOUT1_MUX,
	PT_VP_PASSOUT2_MUX,
	PT_VP_CSOUT1_MUX,
	PT_VP_FEOUT1_MUX,
	PT_VP_AO1_MUX,
	PT_VP_MO2_MUX,
	PT_VP_OUT_MUX_LEN,
};

static struct route_tbl es_pt_vp_in_route_tbl[PT_VP_IN_MUX_LEN] = {
	[PT_VP_PASSIN1_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_PATH_CMD,
				(ES300_DATA_PATH(0, 0, RXCHMGR3))),
		.mux_type = ES_PASSIN1_MUX,
		.cmd_len = 1,
		.chn_mgr_mask = BIT(RXCHMGR3),
	},
	[PT_VP_PASSIN2_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, RXCHMGR4)),
		.mux_type = ES_PASSIN2_MUX,
		.cmd_len = 1,
		.chn_mgr_mask = BIT(RXCHMGR4),
	},
	[PT_VP_PRIMARY_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, RXCHMGR0)),
		.mux_type = ES_PRIMARY_MUX,
		.cmd_len = 1,
		.chn_mgr_mask = BIT(RXCHMGR0),
	},
	[PT_VP_SECONDARY_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, RXCHMGR1)),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0101),
		.cmd[2] = ES_API_WORD(0xB064, 0x0048),
		.cmd[3] = ES_API_WORD(0xB064, 0x0132),
		.cmd[4] = ES_API_WORD(0xB063, 0x0104),
		.cmd[5] = ES_API_WORD(0xB068, 0x0400),
		.mux_type = ES_SECONDARY_MUX,
		.cmd_len = 6,
		.chn_mgr_mask = BIT(RXCHMGR1),
	},
	[PT_VP_FEIN_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, RXCHMGR2)),
		.cmd[1] = ES_API_WORD(0xB05B, 0x0203),
		.cmd[2] = ES_API_WORD(0xB064, 0x0058),
		.cmd[3] = ES_API_WORD(0xB064, 0x0130),
		.cmd[4] = ES_API_WORD(0xB063, 0x0105),
		.cmd[5] = ES_API_WORD(0xB068, 0x0500),
		.mux_type = ES_FEIN_MUX,
		.cmd_len = 6,
		.chn_mgr_mask = BIT(RXCHMGR2),
	},
};

static struct route_tbl es_pt_vp_out_route_tbl[PT_VP_OUT_MUX_LEN] = {
	[PT_VP_PASSOUT1_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, TXCHMGR0)),
		.mux_type = PASS_AUDOUT1,
		.cmd_len = 1,
		.chn_mgr_mask = BIT(TXCHMGR0),
	},
	[PT_VP_PASSOUT2_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, TXCHMGR1)),
		.mux_type = PASS_AUDOUT2,
		.cmd_len = 1,
		.chn_mgr_mask = BIT(TXCHMGR1),
	},
	[PT_VP_CSOUT1_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, TXCHMGR2)),
		.mux_type = VP_CSOUT1,
		.cmd_len = 1,
		.chn_mgr_mask = BIT(TXCHMGR2),
	},
	[PT_VP_FEOUT1_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, TXCHMGR3)),
		.mux_type = VP_FEOUT1,
		.cmd_len = 1,
		.chn_mgr_mask = BIT(TXCHMGR3),
	},
	[PT_VP_AO1_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, TXCHMGR4)),
		.mux_type = PASS_AO1,
		.cmd_len = 1,
		.chn_mgr_mask = BIT(TXCHMGR4),
	},
	[PT_VP_MO2_MUX] = {
		.cmd[0] = ES_API_WORD(ES_SET_MUX_CMD,
				ES300_DATA_PATH(0, 0, TXCHMGR5)),
		.mux_type = PASS_MO2,
		.cmd_len = 1,
		.chn_mgr_mask = BIT(TXCHMGR5),
	},
};

struct es_mux_info es_pt_vp_mux_info = {
	.in_mux_start = PT_VP_PASSIN1_MUX,
	.in_mux_len = PT_VP_IN_MUX_LEN,
	.out_mux_start = PT_VP_PASSOUT1_MUX,
	.out_mux_len = PT_VP_OUT_MUX_LEN,
	.in_tbl = &es_pt_vp_in_route_tbl,
	.out_tbl = &es_pt_vp_out_route_tbl,
};

void prepare_mux_cmd(int mux, u32 *msg, u32 *msg_len,
		u16 *chn_mgr_mask, struct es_mux_info *mux_info, int type)
{
	int i = 0;
	u32 mux_start;
	u32 mux_len;
	struct route_tbl *tbl;

	if (type == CMD_INPUT) {
		mux_start = mux_info->in_mux_start;
		mux_len = mux_info->in_mux_len;
		tbl = mux_info->in_tbl;

	} else {
		mux_start = mux_info->out_mux_start;
		mux_len = mux_info->out_mux_len;
		tbl = mux_info->out_tbl;
	}

	for (i = mux_start; i < mux_len; i++) {
		if (mux != tbl[i].mux_type)
			continue;
		*msg_len = (tbl[i].cmd_len * sizeof(u32));
		*chn_mgr_mask |= tbl[i].chn_mgr_mask;
		memset(msg, 0x0, *msg_len);
		memcpy(msg, tbl[i].cmd, *msg_len);
		break;
	}
}
