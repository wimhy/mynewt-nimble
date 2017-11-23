/*  Bluetooth Mesh */

/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include "syscfg/syscfg.h"
#define BT_DBG_ENABLED (MYNEWT_VAL(BLE_MESH_DEBUG_MODEL))
#include "host/ble_hs_log.h"

#include "mesh/mesh.h"
#include "mesh_priv.h"
#include "adv.h"
#include "net.h"
#include "transport.h"
#include "access.h"
#include "foundation.h"

static s32_t msg_timeout = K_SECONDS(2);

static struct bt_mesh_health_cli *health_cli;

struct health_fault_param {
	u16_t   cid;
	u8_t   *test_id;
	u8_t   *faults;
	size_t *fault_count;
};

static void health_fault_status(struct bt_mesh_model *model,
				struct bt_mesh_msg_ctx *ctx,
				struct os_mbuf *buf)
{
	struct health_fault_param *param;
	u8_t test_id;
	u16_t cid;

	BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
	       ctx->net_idx, ctx->app_idx, ctx->addr, buf->om_len,
	       bt_hex(buf->om_data, buf->om_len));

	if (health_cli->op_pending != OP_HEALTH_FAULT_STATUS) {
		BT_WARN("Unexpected Health Fault Status message");
		return;
	}

	param = health_cli->op_param;

	test_id = net_buf_simple_pull_u8(buf);
	cid = net_buf_simple_pull_le16(buf);

	if (cid != param->cid) {
		BT_WARN("Health fault with unexpected Company ID");
		return;
	}

	*param->test_id = test_id;

	if (buf->om_len > *param->fault_count) {
		BT_WARN("Got more faults than there's space for");
	} else {
		*param->fault_count = buf->om_len;
	}

	memcpy(param->faults, buf->om_data, *param->fault_count);

	k_sem_give(&health_cli->op_sync);
}

static void health_current_status(struct bt_mesh_model *model,
				  struct bt_mesh_msg_ctx *ctx,
				  struct os_mbuf *buf)
{
	struct bt_mesh_health_cli *cli = model->user_data;
	u8_t test_id;
	u16_t cid;

	BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
	       ctx->net_idx, ctx->app_idx, ctx->addr, buf->om_len,
	       bt_hex(buf->om_data, buf->om_len));

	test_id = net_buf_simple_pull_u8(buf);
	cid = net_buf_simple_pull_le16(buf);

	BT_DBG("Test ID 0x%02x Company ID 0x%04x Fault Count %u",
	       test_id, cid, buf->om_len);

	if (!cli->current_status) {
		BT_WARN("No Current Status callback available");
		return;
	}

	cli->current_status(cli, ctx->addr, test_id, cid, buf->om_data, buf->om_len);
}

const struct bt_mesh_model_op bt_mesh_health_cli_op[] = {
	{ OP_HEALTH_FAULT_STATUS,    3,   health_fault_status },
	{ OP_HEALTH_CURRENT_STATUS,  3,   health_current_status },
	BT_MESH_MODEL_OP_END,
};

static int check_cli(void)
{
	if (!health_cli) {
		BT_ERR("No available Health Client context!");
		return -EINVAL;
	}

	if (health_cli->op_pending) {
		BT_WARN("Another synchronous operation pending");
		return -EBUSY;
	}

	return 0;
}

int bt_mesh_health_fault_get(u16_t net_idx, u16_t addr, u16_t app_idx,
			     u16_t cid, u8_t *test_id, u8_t *faults,
			     size_t *fault_count)
{
	struct os_mbuf *msg = NET_BUF_SIMPLE(2 + 2 + 4);
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net_idx,
		.app_idx = app_idx,
		.addr = addr,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};
	struct health_fault_param param = {
		.cid = cid,
		.test_id = test_id,
		.faults = faults,
		.fault_count = fault_count,
	};
	int err;

	err = check_cli();
	if (err) {
		return err;
	}

	bt_mesh_model_msg_init(msg, OP_HEALTH_FAULT_GET);
	net_buf_simple_add_le16(msg, cid);

	err = bt_mesh_model_send(health_cli->model, &ctx, msg, NULL, NULL);
	if (err) {
		BT_ERR("model_send() failed (err %d)", err);
		return err;
	}

	health_cli->op_param = &param;
	health_cli->op_pending = OP_HEALTH_FAULT_STATUS;

	err = k_sem_take(&health_cli->op_sync, msg_timeout);

	health_cli->op_pending = 0;
	health_cli->op_param = NULL;

	return err;
}

s32_t bt_mesh_health_cli_timeout_get(void)
{
	return msg_timeout;
}

void bt_mesh_health_cli_timeout_set(s32_t timeout)
{
	msg_timeout = timeout;
}

int bt_mesh_health_cli_set(struct bt_mesh_model *model)
{
	if (!model->user_data) {
		BT_ERR("No Health Client context for given model");
		return -EINVAL;
	}

	health_cli = model->user_data;

	return 0;
}

int bt_mesh_health_cli_init(struct bt_mesh_model *model, bool primary)
{
	struct bt_mesh_health_cli *cli = model->user_data;

	BT_DBG("primary %u", primary);

	if (!cli) {
		BT_ERR("No Health Client context provided");
		return -EINVAL;
	}

	cli = model->user_data;
	cli->model = model;

	k_sem_init(&cli->op_sync, 0, 1);

	/* Set the default health client pointer */
	if (!health_cli) {
		health_cli = cli;
	}

	return 0;
}
