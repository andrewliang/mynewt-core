/*  Bluetooth Mesh */

/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "syscfg/syscfg.h"

#if MYNEWT_VAL(BLE_MESH_PROXY) == 1

#include "mesh/mesh.h"

#define BT_DBG_ENABLED (MYNEWT_VAL(BLE_MESH_DEBUG_PROXY))
#include "host/ble_hs_log.h"
#include "host/ble_att.h"
#include "services/gatt/ble_svc_gatt.h"
#include "../../host/src/ble_hs_priv.h"

#include "mesh_priv.h"
#include "adv.h"
#include "net.h"
#include "prov.h"
#include "beacon.h"
#include "foundation.h"
#include "access.h"
#include "proxy.h"

#define PDU_TYPE(data)     (data[0] & BIT_MASK(6))
#define PDU_SAR(data)      (data[0] >> 6)

#define SAR_COMPLETE       0x00
#define SAR_FIRST          0x01
#define SAR_CONT           0x02
#define SAR_LAST           0x03

#define CFG_FILTER_SET     0x00
#define CFG_FILTER_ADD     0x01
#define CFG_FILTER_REMOVE  0x02
#define CFG_FILTER_STATUS  0x03

/** @def BT_UUID_MESH_PROV
 *  @brief Mesh Provisioning Service
 */
ble_uuid16_t BT_UUID_MESH_PROV                 = BLE_UUID16_INIT(0x1827);
#define BT_UUID_MESH_PROV_VAL             0x1827
/** @def BT_UUID_MESH_PROXY
 *  @brief Mesh Proxy Service
 */
ble_uuid16_t BT_UUID_MESH_PROXY                = BLE_UUID16_INIT(0x1828);
#define BT_UUID_MESH_PROXY_VAL            0x1828
/** @def BT_UUID_GATT_CCC
 *  @brief GATT Client Characteristic Configuration
 */
ble_uuid16_t BT_UUID_GATT_CCC                  = BLE_UUID16_INIT(0x2902);
#define BT_UUID_GATT_CCC_VAL              0x2902
/** @def BT_UUID_MESH_PROV_DATA_IN
 *  @brief Mesh Provisioning Data In
 */
ble_uuid16_t BT_UUID_MESH_PROV_DATA_IN         = BLE_UUID16_INIT(0x2adb);
#define BT_UUID_MESH_PROV_DATA_IN_VAL     0x2adb
/** @def BT_UUID_MESH_PROV_DATA_OUT
 *  @brief Mesh Provisioning Data Out
 */
ble_uuid16_t BT_UUID_MESH_PROV_DATA_OUT        = BLE_UUID16_INIT(0x2adc);
#define BT_UUID_MESH_PROV_DATA_OUT_VAL    0x2adc
/** @def BT_UUID_MESH_PROXY_DATA_IN
 *  @brief Mesh Proxy Data In
 */
ble_uuid16_t BT_UUID_MESH_PROXY_DATA_IN        = BLE_UUID16_INIT(0x2add);
#define BT_UUID_MESH_PROXY_DATA_IN_VAL    0x2add
/** @def BT_UUID_MESH_PROXY_DATA_OUT
 *  @brief Mesh Proxy Data Out
 */
ble_uuid16_t BT_UUID_MESH_PROXY_DATA_OUT       = BLE_UUID16_INIT(0x2ade);
#define BT_UUID_MESH_PROXY_DATA_OUT_VAL   0x2ade

#define PDU_HDR(sar, type) (sar << 6 | (type & BIT_MASK(6)))

#define CLIENT_BUF_SIZE 68

static const struct ble_gap_adv_params slow_adv_param = {
	.conn_mode = (BLE_GAP_CONN_MODE_UND ),
	.itvl_min = BT_GAP_ADV_SLOW_INT_MIN,
	.itvl_max = BT_GAP_ADV_SLOW_INT_MAX,
};

static const struct ble_gap_adv_params fast_adv_param = {
	.conn_mode = (BLE_GAP_CONN_MODE_UND),
	.itvl_min = BT_GAP_ADV_FAST_INT_MIN_2,
	.itvl_max = BT_GAP_ADV_FAST_INT_MAX_2,
};

static bool proxy_adv_enabled;

#if (MYNEWT_VAL(BLE_MESH_GATT_PROXY))
static void proxy_send_beacons(struct os_event *work);
#endif

#if (MYNEWT_VAL(BLE_MESH_PB_GATT))
static bool prov_fast_adv;
#endif

static struct bt_mesh_proxy_client {
	uint16_t conn_handle;
	u16_t filter[MYNEWT_VAL(BLE_MESH_PROXY_FILTER_SIZE)];
	enum __packed {
		NONE,
		WHITELIST,
		BLACKLIST,
		PROV,
	} filter_type;
	u8_t msg_type;
#if (MYNEWT_VAL(BLE_MESH_GATT_PROXY))
	struct os_callout send_beacons;
#endif
	struct os_mbuf    *buf;
} clients[MYNEWT_VAL(BLE_MAX_CONNECTIONS)] = {
	[0 ... (MYNEWT_VAL(BLE_MAX_CONNECTIONS) - 1)] = { 0 },
};

/* Track which service is enabled */
static enum {
	MESH_GATT_NONE,
	MESH_GATT_PROV,
	MESH_GATT_PROXY,
} gatt_svc = MESH_GATT_NONE;

static struct {
	uint16_t proxy_h;
	uint16_t proxy_data_out_h;
	uint16_t prov_h;
	uint16_t prov_data_in_h;
	uint16_t prov_data_out_h;
} svc_handles;

static void resolve_svc_handles(void)
{
	int rc;

	/* Either all handles are already resolved, or none of them */
	if (svc_handles.prov_data_out_h) {
		return;
	}

	/*
	 * We assert if attribute is not found since at this stage all attributes
	 * shall be already registered and thus shall be found.
	 */

	rc = ble_gatts_find_svc(BLE_UUID16_DECLARE(BT_UUID_MESH_PROXY_VAL),
				&svc_handles.proxy_h);
	assert(rc == 0);

	rc = ble_gatts_find_chr(BLE_UUID16_DECLARE(BT_UUID_MESH_PROXY_VAL),
				BLE_UUID16_DECLARE(BT_UUID_MESH_PROXY_DATA_OUT_VAL),
				NULL, &svc_handles.proxy_data_out_h);
	assert(rc == 0);

	rc = ble_gatts_find_svc(BLE_UUID16_DECLARE(BT_UUID_MESH_PROV_VAL),
				&svc_handles.prov_h);
	assert(rc == 0);

	rc = ble_gatts_find_chr(BLE_UUID16_DECLARE(BT_UUID_MESH_PROV_VAL),
				BLE_UUID16_DECLARE(BT_UUID_MESH_PROV_DATA_IN_VAL),
				NULL, &svc_handles.prov_data_in_h);
	assert(rc == 0);

	rc = ble_gatts_find_chr(BLE_UUID16_DECLARE(BT_UUID_MESH_PROV_VAL),
				BLE_UUID16_DECLARE(BT_UUID_MESH_PROV_DATA_OUT_VAL),
				NULL, &svc_handles.prov_data_out_h);
	assert(rc == 0);
}

static struct bt_mesh_proxy_client *find_client(uint16_t conn_handle)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		if (clients[i].conn_handle == conn_handle) {
			return &clients[i];
		}
	}

	return NULL;
}

#if (MYNEWT_VAL(BLE_MESH_GATT_PROXY))
static int proxy_segment_and_send(uint16_t conn_handle, u8_t type,
				  struct os_mbuf *msg);

static int filter_set(struct bt_mesh_proxy_client *client,
		      struct os_mbuf *buf)
{
	u8_t type;

	if (buf->om_len < 1) {
		BT_WARN("Too short Filter Set message");
		return -EINVAL;
	}

	type = net_buf_simple_pull_u8(buf);
	BT_DBG("type 0x%02x", type);

	switch (type) {
	case 0x00:
		memset(client->filter, 0, sizeof(client->filter));
		client->filter_type = WHITELIST;
		break;
	case 0x01:
		memset(client->filter, 0, sizeof(client->filter));
		client->filter_type = BLACKLIST;
		break;
	default:
		BT_WARN("Prohibited Filter Type 0x%02x", type);
		return -EINVAL;
	}

	return 0;
}

static void filter_add(struct bt_mesh_proxy_client *client, u16_t addr)
{
	int i;

	BT_DBG("addr 0x%02x", addr);

	if (addr == BT_MESH_ADDR_UNASSIGNED) {
		return;
	}

	for (i = 0; i < ARRAY_SIZE(client->filter); i++) {
		if (client->filter[i] == addr) {
			return;
		}
	}

	for (i = 0; i < ARRAY_SIZE(client->filter); i++) {
		if (client->filter[i] == BT_MESH_ADDR_UNASSIGNED) {
			client->filter[i] = addr;
			return;
		}
	}
}

static void filter_remove(struct bt_mesh_proxy_client *client, u16_t addr)
{
	int i;

	BT_DBG("addr 0x%02x", addr);

	if (addr == BT_MESH_ADDR_UNASSIGNED) {
		return;
	}

	for (i = 0; i < ARRAY_SIZE(client->filter); i++) {
		if (client->filter[i] == addr) {
			client->filter[i] = BT_MESH_ADDR_UNASSIGNED;
			return;
		}
	}
}

static void send_filter_status(struct bt_mesh_proxy_client *client,
			       struct bt_mesh_net_rx *rx,
			       struct os_mbuf *buf)
{
	struct bt_mesh_net_tx tx = {
		.sub = rx->sub,
		.ctx = &rx->ctx,
		.src = bt_mesh_primary_addr(),
	};
	u16_t filter_size;
	int i, err;

	/* Configuration messages always have dst unassigned */
	tx.ctx->addr = BT_MESH_ADDR_UNASSIGNED;

	net_buf_simple_init(buf, 10);

	net_buf_simple_add_u8(buf, CFG_FILTER_STATUS);

	if (client->filter_type == WHITELIST) {
		net_buf_simple_add_u8(buf, 0x00);
	} else {
		net_buf_simple_add_u8(buf, 0x01);
	}

	for (filter_size = 0, i = 0; i < ARRAY_SIZE(client->filter); i++) {
		if (client->filter[i] != BT_MESH_ADDR_UNASSIGNED) {
			filter_size++;
		}
	}

	net_buf_simple_add_be16(buf, filter_size);

	BT_DBG("%u bytes: %s", buf->om_len, bt_hex(buf->om_data, buf->om_len));

	err = bt_mesh_net_encode(&tx, buf, true);
	if (err) {
		BT_ERR("Encoding Proxy cfg message failed (err %d)", err);
		return;
	}

	err = proxy_segment_and_send(client->conn_handle, BT_MESH_PROXY_CONFIG, buf);
	if (err) {
		BT_ERR("Failed to send proxy cfg message (err %d)", err);
	}
}

static void proxy_cfg(struct bt_mesh_proxy_client *client)
{
	struct os_mbuf *buf = NET_BUF_SIMPLE(29);
	struct bt_mesh_net_rx rx;
	u8_t opcode;
	int err;

	err = bt_mesh_net_decode(client->buf, BT_MESH_NET_IF_PROXY_CFG,
				 &rx, buf);
	if (err) {
		BT_ERR("Failed to decode Proxy Configuration (err %d)", err);
		goto done;
	}

	/* Remove network headers */
	net_buf_simple_pull(buf, BT_MESH_NET_HDR_LEN);

	BT_DBG("%u bytes: %s", buf->om_len, bt_hex(buf->om_data, buf->om_len));

	if (buf->om_len < 1) {
		BT_WARN("Too short proxy configuration PDU");
		goto done;
	}

	opcode = net_buf_simple_pull_u8(buf);
	switch (opcode) {
	case CFG_FILTER_SET:
		filter_set(client, buf);
		send_filter_status(client, &rx, buf);
		break;
	case CFG_FILTER_ADD:
		while (buf->om_len >= 2) {
			u16_t addr;

			addr = net_buf_simple_pull_be16(buf);
			filter_add(client, addr);
		}
		send_filter_status(client, &rx, buf);
		break;
	case CFG_FILTER_REMOVE:
		while (buf->om_len >= 2) {
			u16_t addr;

			addr = net_buf_simple_pull_be16(buf);
			filter_remove(client, addr);
		}
		send_filter_status(client, &rx, buf);
		break;
	default:
		BT_WARN("Unhandled configuration OpCode 0x%02x", opcode);
		break;
	}

done:
	os_mbuf_free_chain(buf);
}

static int beacon_send(uint16_t conn_handle, struct bt_mesh_subnet *sub)
{
	struct os_mbuf *buf = NET_BUF_SIMPLE(23);
	int rc;

	net_buf_simple_init(buf, 1);
	bt_mesh_beacon_create(sub, buf);

	rc = proxy_segment_and_send(conn_handle, BT_MESH_PROXY_BEACON, buf);
	os_mbuf_free_chain(buf);
	return rc;
}

static void proxy_send_beacons(struct os_event *work)
{
	struct bt_mesh_proxy_client *client;
	int i;

	client = work->ev_arg;

	for (i = 0; i < ARRAY_SIZE(bt_mesh.sub); i++) {
		struct bt_mesh_subnet *sub = &bt_mesh.sub[i];

		if (sub->net_idx != BT_MESH_KEY_UNUSED) {
			beacon_send(client->conn_handle, sub);
		}
	}
}

void bt_mesh_proxy_beacon_send(struct bt_mesh_subnet *sub)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		if (clients[i].conn_handle) {
			beacon_send(clients[i].conn_handle, sub);
		}
	}
}

int bt_mesh_proxy_identity_enable(void)
{
	/* FIXME: Add support for multiple subnets */
	struct bt_mesh_subnet *sub = &bt_mesh.sub[0];

	BT_DBG("");

	if (!bt_mesh_is_provisioned()) {
		return -EAGAIN;
	}

	if (sub->net_idx == BT_MESH_KEY_UNUSED) {
		return -ENOENT;
	}

	if (sub->node_id == BT_MESH_NODE_IDENTITY_NOT_SUPPORTED) {
		return -ENOTSUP;
	}

	if (sub->node_id == BT_MESH_NODE_IDENTITY_RUNNING) {
		return 0;
	}

	sub->node_id = BT_MESH_NODE_IDENTITY_RUNNING;
	sub->node_id_start = k_uptime_get_32();
	bt_mesh_adv_update();

	return 0;
}

#endif /* GATT_PROXY */

static void proxy_complete_pdu(struct bt_mesh_proxy_client *client)
{
	switch (client->msg_type) {
#if (MYNEWT_VAL(BLE_MESH_GATT_PROXY))
	case BT_MESH_PROXY_NET_PDU:
		BT_INFO("Mesh Network PDU");
		bt_mesh_net_recv(client->buf, 0, BT_MESH_NET_IF_PROXY);
		break;
	case BT_MESH_PROXY_BEACON:
		BT_INFO("Mesh Beacon PDU");
		bt_mesh_beacon_recv(client->buf);
		break;
	case BT_MESH_PROXY_CONFIG:
		BT_INFO("Mesh Configuration PDU");
		proxy_cfg(client);
		break;
#endif
#if (MYNEWT_VAL(BLE_MESH_PB_GATT))
	case BT_MESH_PROXY_PROV:
		BT_INFO("Mesh Provisioning PDU");
		bt_mesh_pb_gatt_recv(client->conn_handle, client->buf);
		break;
#endif
	default:
		BT_WARN("Unhandled Message Type 0x%02x", client->msg_type);
		break;
	}

	net_buf_simple_init(client->buf, 0);
}

static int proxy_recv(uint16_t conn_handle, uint16_t attr_handle,
		      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	struct bt_mesh_proxy_client *client;
	const u8_t *data = ctxt->om->om_data;
	u16_t len = ctxt->om->om_len;

	client = find_client(conn_handle);

	if (!client) {
		return -ENOTCONN;
	}

	if (len < 1) {
		BT_WARN("Too small Proxy PDU");
		return -EINVAL;
	}

	if ((attr_handle == svc_handles.prov_data_in_h) !=
	    (PDU_TYPE(data) == BT_MESH_PROXY_PROV)) {
		BT_WARN("Proxy PDU type doesn't match GATT service");
		return -EINVAL;
	}

	if (len - 1 > net_buf_simple_tailroom(client->buf)) {
		BT_WARN("Too big proxy PDU");
		return -EINVAL;
	}

	switch (PDU_SAR(data)) {
	case SAR_COMPLETE:
		if (client->buf->om_len) {
			BT_WARN("Complete PDU while a pending incomplete one");
			return -EINVAL;
		}

		client->msg_type = PDU_TYPE(data);
		net_buf_simple_add_mem(client->buf, data + 1, len - 1);
		proxy_complete_pdu(client);
		break;

	case SAR_FIRST:
		if (client->buf->om_len) {
			BT_WARN("First PDU while a pending incomplete one");
			return -EINVAL;
		}

		client->msg_type = PDU_TYPE(data);
		net_buf_simple_add_mem(client->buf, data + 1, len - 1);
		break;

	case SAR_CONT:
		if (!client->buf->om_len) {
			BT_WARN("Continuation with no prior data");
			return -EINVAL;
		}

		if (client->msg_type != PDU_TYPE(data)) {
			BT_WARN("Unexpected message type in continuation");
			return -EINVAL;
		}

		net_buf_simple_add_mem(client->buf, data + 1, len - 1);
		break;

	case SAR_LAST:
		if (!client->buf->om_len) {
			BT_WARN("Last SAR PDU with no prior data");
			return -EINVAL;
		}

		if (client->msg_type != PDU_TYPE(data)) {
			BT_WARN("Unexpected message type in last SAR PDU");
			return -EINVAL;
		}

		net_buf_simple_add_mem(client->buf, data + 1, len - 1);
		proxy_complete_pdu(client);
		break;
	}

	return len;
}

static void proxy_connected(uint16_t conn_handle)
{
	struct bt_mesh_proxy_client *client;
	int i;

	BT_INFO("conn_handle %d", conn_handle);

	/* Since we use ADV_OPT_ONE_TIME */
	proxy_adv_enabled = false;

#if MYNEWT_VAL(BLE_MAX_CONNECTIONS) > 1
	/* Try to re-enable advertising in case it's possible */
	bt_mesh_adv_update();
#endif

	for (client = NULL, i = 0; i < ARRAY_SIZE(clients); i++) {
		if (!clients[i].conn_handle) {
			client = &clients[i];
			break;
		}
	}

	if (!client) {
		BT_ERR("No free Proxy Client objects");
		return;
	}

	client->conn_handle = conn_handle;
	client->filter_type = NONE;
	memset(client->filter, 0, sizeof(client->filter));
	net_buf_simple_init(client->buf, 0);
}

static void proxy_disconnected(uint16_t conn_handle, int reason)
{
	int i;

	BT_INFO("conn_handle %d reason %d", conn_handle, reason);

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		struct bt_mesh_proxy_client *client = &clients[i];

		if (client->conn_handle == conn_handle) {
			if ((MYNEWT_VAL(BLE_MESH_PB_GATT)) &&
			    client->filter_type == PROV) {
				bt_mesh_pb_gatt_close(conn_handle);
			}

			client->conn_handle = 0;
			break;
		}
	}

	bt_mesh_adv_update();
}

struct os_mbuf *bt_mesh_proxy_get_buf(void)
{
	struct os_mbuf *buf = clients[0].buf;

	net_buf_simple_init(buf, 0);

	return buf;
}

#if (MYNEWT_VAL(BLE_MESH_PB_GATT))
static void prov_ccc_write(uint16_t conn_handle)
{
	struct bt_mesh_proxy_client *client;

	BT_DBG("conn_handle %d", conn_handle);

	/* If a connection exists there must be a client */
	client = find_client(conn_handle);
	__ASSERT(client, "No client for connection");

	if (client->filter_type == NONE) {
		client->filter_type = PROV;
		bt_mesh_pb_gatt_open(conn_handle);
	}
}

int bt_mesh_proxy_prov_enable(void)
{
	uint16_t handle;
	int rc;
	int i;

	BT_DBG("");

	rc = ble_gatts_find_svc(BLE_UUID16_DECLARE(BT_UUID_MESH_PROV_VAL), &handle);
	assert(rc == 0);
	ble_gatts_svc_set_visibility(handle, 1);
	/* FIXME: figure out end handle */
	ble_svc_gatt_changed(svc_handles.prov_h, 0xffff);

	gatt_svc = MESH_GATT_PROV;
	prov_fast_adv = true;

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		if (clients[i].conn_handle) {
			clients[i].filter_type = PROV;
		}
	}


	return 0;
}

int bt_mesh_proxy_prov_disable(void)
{
	uint16_t handle;
	int rc;
	int i;

	BT_DBG("");

	rc = ble_gatts_find_svc(BLE_UUID16_DECLARE(BT_UUID_MESH_PROV_VAL), &handle);
	assert(rc == 0);
	ble_gatts_svc_set_visibility(handle, 0);
	/* FIXME: figure out end handle */
	ble_svc_gatt_changed(svc_handles.prov_h, 0xffff);

	gatt_svc = MESH_GATT_NONE;

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		struct bt_mesh_proxy_client *client = &clients[i];

		if (clients->conn_handle && client->filter_type == PROV) {
			bt_mesh_pb_gatt_close(client->conn_handle);
			client->filter_type = NONE;
		}
	}

	return 0;
}
#endif /* MYNEWT_VAL(BLE_MESH_PB_GATT) */

#if (MYNEWT_VAL(BLE_MESH_GATT_PROXY))
static void proxy_ccc_write(uint16_t conn_handle)
{
	struct bt_mesh_proxy_client *client;

	BT_DBG("conn_handle %d", conn_handle);

	client = find_client(conn_handle);
	__ASSERT(client, "No client for connection");

	if (client->filter_type == NONE) {
		client->filter_type = WHITELIST;
		k_work_add_arg(&client->send_beacons, client);
		k_work_submit(&client->send_beacons);
	}
}

int bt_mesh_proxy_gatt_enable(void)
{
	uint16_t handle;
	int rc;
	int i;

	BT_DBG("");

	rc = ble_gatts_find_svc(BLE_UUID16_DECLARE(BT_UUID_MESH_PROXY_VAL), &handle);
	assert(rc == 0);
	ble_gatts_svc_set_visibility(handle, 1);
	/* FIXME: figure out end handle */
	ble_svc_gatt_changed(svc_handles.proxy_h, 0xffff);

	gatt_svc = MESH_GATT_PROXY;

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		if (clients[i].conn_handle) {
			clients[i].filter_type = WHITELIST;
		}
	}

	return 0;
}

int bt_mesh_proxy_gatt_disable(void)
{
	uint16_t handle;
	int rc;
	int i;

	BT_DBG("");

	rc = ble_gatts_find_svc(BLE_UUID16_DECLARE(BT_UUID_MESH_PROXY_VAL), &handle);
	assert(rc == 0);
	ble_gatts_svc_set_visibility(handle, 0);
	/* FIXME: figure out end handle */
	ble_svc_gatt_changed(svc_handles.proxy_h, 0xffff);

	gatt_svc = MESH_GATT_NONE;

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		struct bt_mesh_proxy_client *client = &clients[i];

		if (client->conn_handle && (client->filter_type == WHITELIST ||
					    client->filter_type == BLACKLIST)) {
			client->filter_type = NONE;
		}
	}

	return 0;
}

void bt_mesh_proxy_addr_add(struct os_mbuf *buf, u16_t addr)
{
	struct bt_mesh_proxy_client *client = NULL;
	int i;

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		client = &clients[i];
		if (client->buf == buf) {
			break;
		}
	}

	assert(client);

	BT_DBG("filter_type %u addr 0x%04x", client->filter_type, addr);

	if (client->filter_type == WHITELIST) {
		filter_add(client, addr);
	} else if (client->filter_type == BLACKLIST) {
		filter_remove(client, addr);
	}
}

static bool client_filter_match(struct bt_mesh_proxy_client *client,
				u16_t addr)
{
	int i;

	BT_DBG("filter_type %u addr 0x%04x", client->filter_type, addr);

	if (client->filter_type == WHITELIST) {
		for (i = 0; i < ARRAY_SIZE(client->filter); i++) {
			if (client->filter[i] == addr) {
				return true;
			}
		}

		return false;
	}

	if (client->filter_type == BLACKLIST) {
		for (i = 0; i < ARRAY_SIZE(client->filter); i++) {
			if (client->filter[i] == addr) {
				return false;
			}
		}

		return true;
	}

	return false;
}

bool bt_mesh_proxy_relay(struct os_mbuf *buf, u16_t dst)
{
	bool relayed = false;
	int i;

	BT_DBG("%u bytes to dst 0x%04x", buf->om_len, dst);

	for (i = 0; i < ARRAY_SIZE(clients); i++) {
		struct bt_mesh_proxy_client *client = &clients[i];
		struct os_mbuf *msg;

		if (!client->conn_handle) {
			continue;
		}

		if (!client_filter_match(client, dst)) {
			continue;
		}

		/* Proxy PDU sending modifies the original buffer,
		 * so we need to make a copy.
		 */
		msg = NET_BUF_SIMPLE(32);
		net_buf_simple_init(msg, 1);
		net_buf_simple_add_mem(msg, buf->om_data, buf->om_len);

		bt_mesh_proxy_send(client->conn_handle, BT_MESH_PROXY_NET_PDU, msg);
		os_mbuf_free_chain(msg);
		relayed = true;
	}

	return relayed;
}

#endif /* MYNEWT_VAL(BLE_MESH_GATT_PROXY) */

static int proxy_send(uint16_t conn_handle, const void *data, u16_t len)
{
	struct os_mbuf *om;

	BT_DBG("%u bytes: %s", len, bt_hex(data, len));

#if (MYNEWT_VAL(BLE_MESH_GATT_PROXY))
	if (gatt_svc == MESH_GATT_PROXY) {
		om = ble_hs_mbuf_from_flat(data, len);
		assert(om);
		ble_gattc_notify_custom(conn_handle, svc_handles.proxy_data_out_h, om);
	}
#endif

#if (MYNEWT_VAL(BLE_MESH_PB_GATT))
	if (gatt_svc == MESH_GATT_PROV) {
		om = ble_hs_mbuf_from_flat(data, len);
		assert(om);
		ble_gattc_notify_custom(conn_handle, svc_handles.prov_data_out_h, om);
	}
#endif

	return 0;
}

static int proxy_segment_and_send(uint16_t conn_handle, u8_t type,
				  struct os_mbuf *msg)
{
	u16_t mtu;

	BT_DBG("conn_handle %d type 0x%02x len %u: %s", conn_handle, type, msg->om_len,
	       bt_hex(msg->om_data, msg->om_len));

	/* ATT_MTU - OpCode (1 byte) - Handle (2 bytes) */
	mtu = ble_att_mtu(conn_handle) - 3;
	if (mtu > msg->om_len) {
		net_buf_simple_push_u8(msg, PDU_HDR(SAR_COMPLETE, type));
		return proxy_send(conn_handle, msg->om_data, msg->om_len);
	}

	net_buf_simple_push_u8(msg, PDU_HDR(SAR_FIRST, type));
	proxy_send(conn_handle, msg->om_data, mtu);
	net_buf_simple_pull(msg, mtu);

	while (msg->om_len) {
		if (msg->om_len + 1 < mtu) {
			net_buf_simple_push_u8(msg, PDU_HDR(SAR_LAST, type));
			proxy_send(conn_handle, msg->om_data, msg->om_len);
			break;
		}

		net_buf_simple_push_u8(msg, PDU_HDR(SAR_CONT, type));
		proxy_send(conn_handle, msg->om_data, mtu);
		net_buf_simple_pull(msg, mtu);
	}

	return 0;
}

int bt_mesh_proxy_send(uint16_t conn_handle, u8_t type,
		       struct os_mbuf *msg)
{
	struct bt_mesh_proxy_client *client = find_client(conn_handle);

	if (!client) {
		BT_ERR("No Proxy Client found");
		return -ENOTCONN;
	}

	if ((client->filter_type == PROV) != (type == BT_MESH_PROXY_PROV)) {
		BT_ERR("Invalid PDU type for Proxy Client");
		return -EINVAL;
	}

	return proxy_segment_and_send(conn_handle, type, msg);
}

#if (MYNEWT_VAL(BLE_MESH_PB_GATT))
static u8_t prov_svc_data[20] = { 0x27, 0x18, };

static const struct bt_data prov_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x27, 0x18),
	BT_DATA(BT_DATA_SVC_DATA16, prov_svc_data, sizeof(prov_svc_data)),
};

static const struct bt_data prov_sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, "mynewtproxy",
		(sizeof("mynewtproxy") - 1)),
};
#endif /* PB_GATT */

#if (MYNEWT_VAL(BLE_MESH_GATT_PROXY))

#define ID_TYPE_NET  0x00
#define ID_TYPE_NODE 0x01

#define NODE_ID_LEN  19
#define NET_ID_LEN   11

#define NODE_ID_TIMEOUT K_SECONDS(CONFIG_BT_MESH_NODE_ID_TIMEOUT)

static u8_t proxy_svc_data[NODE_ID_LEN] = { 0x28, 0x18, };

static const struct bt_data node_id_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x28, 0x18),
	BT_DATA(BT_DATA_SVC_DATA16, proxy_svc_data, NODE_ID_LEN),
};

static const struct bt_data net_id_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x28, 0x18),
	BT_DATA(BT_DATA_SVC_DATA16, proxy_svc_data, NET_ID_LEN),
};

static int node_id_adv(struct bt_mesh_subnet *sub)
{
	u8_t tmp[16];
	int err;

	BT_DBG("");

	proxy_svc_data[2] = ID_TYPE_NODE;

	err = bt_rand(proxy_svc_data + 11, 8);
	if (err) {
		return err;
	}

	memset(tmp, 0, 6);
	memcpy(tmp + 6, proxy_svc_data + 11, 8);
	sys_put_be16(bt_mesh_primary_addr(), tmp + 14);

	err = bt_encrypt_be(sub->keys[sub->kr_flag].identity, tmp, tmp);
	if (err) {
		return err;
	}

	memcpy(proxy_svc_data + 3, tmp + 8, 8);

	err = bt_le_adv_start(&fast_adv_param, node_id_ad,
			      ARRAY_SIZE(node_id_ad), NULL, 0);
	if (err) {
		BT_WARN("Failed to advertise using Node ID (err %d)", err);
		return err;
	}

	proxy_adv_enabled = true;

	return 0;
}

static int net_id_adv(struct bt_mesh_subnet *sub)
{
	int err;

	BT_DBG("");

	proxy_svc_data[2] = ID_TYPE_NET;

	BT_DBG("Advertising with NetId %s",
	       bt_hex(sub->keys[sub->kr_flag].net_id, 8));

	memcpy(proxy_svc_data + 3, sub->keys[sub->kr_flag].net_id, 8);

	err = bt_le_adv_start(&slow_adv_param, net_id_ad,
			      ARRAY_SIZE(net_id_ad), NULL, 0);
	if (err) {
		BT_WARN("Failed to advertise using Network ID (err %d)", err);
		return err;
	}

	proxy_adv_enabled = true;

	return 0;
}

static s32_t gatt_proxy_advertise(void)
{
	/* TODO: Add support for multiple subnets */
	struct bt_mesh_subnet *sub = &bt_mesh.sub[0];
	s32_t remaining = K_FOREVER;

	BT_DBG("");

	if (sub->net_idx == BT_MESH_KEY_UNUSED) {
		BT_WARN("First subnet is not valid");
		return remaining;
	}

	if (sub->node_id == BT_MESH_NODE_IDENTITY_RUNNING) {
		u32_t active = k_uptime_get_32() - sub->node_id_start;

		if (active < NODE_ID_TIMEOUT) {
			remaining = NODE_ID_TIMEOUT - active;
			BT_DBG("Node ID active for %u ms, %u ms remaining",
			       active, remaining);
			node_id_adv(sub);
		} else {
			sub->node_id = BT_MESH_NODE_IDENTITY_STOPPED;
			sub->node_id_start = 0;
			BT_DBG("Node ID stopped");
		}
	}

	if (sub->node_id == BT_MESH_NODE_IDENTITY_STOPPED &&
	    bt_mesh_gatt_proxy_get() == BT_MESH_GATT_PROXY_ENABLED) {
		net_id_adv(sub);
	}

	return remaining;
}
#endif /* GATT_PROXY */

s32_t bt_mesh_proxy_adv_start(void)
{
	BT_DBG("");

	if (gatt_svc == MESH_GATT_NONE) {
		return K_FOREVER;
	}

#if (MYNEWT_VAL(BLE_MESH_PB_GATT))
	if (!bt_mesh_is_provisioned()) {
		const struct bt_le_adv_param *param;

		if (prov_fast_adv) {
			param = &fast_adv_param;
		} else {
			param = &slow_adv_param;
		}

		if (bt_le_adv_start(param, prov_ad, ARRAY_SIZE(prov_ad),
				    prov_sd, ARRAY_SIZE(prov_sd)) == 0) {
			proxy_adv_enabled = true;

			/* Advertise 60 seconds using fast interval */
			if (prov_fast_adv) {
				prov_fast_adv = false;
				return K_SECONDS(60);
			}
		}
	}
#endif /* PB_GATT */

#if (MYNEWT_VAL(BLE_MESH_GATT_PROXY))
	if (bt_mesh_is_provisioned()) {
		return gatt_proxy_advertise();
	}
#endif /* GATT_PROXY */

	return K_FOREVER;
}

void bt_mesh_proxy_adv_stop(void)
{
	int err;

	BT_DBG("adv_enabled %u", proxy_adv_enabled);

	if (!proxy_adv_enabled) {
		return;
	}

	err = bt_le_adv_stop();
	if (err) {
		BT_ERR("Failed to stop advertising (err %d)", err);
	} else {
		proxy_adv_enabled = false;
	}
}

int
ble_mesh_proxy_gap_event(struct ble_gap_event *event, void *arg)
{
//    BT_DBG("event %d", event->type);

	if (event->type == BLE_GAP_EVENT_CONNECT) {
		proxy_connected(event->connect.conn_handle);
	} else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
		proxy_disconnected(event->disconnect.conn.conn_handle,
				   event->disconnect.reason);
	} else if (event->type == BLE_GAP_EVENT_SUBSCRIBE) {
		if (event->subscribe.attr_handle == svc_handles.proxy_data_out_h) {
#if (MYNEWT_VAL(BLE_MESH_GATT_PROXY))
			proxy_ccc_write(event->subscribe.conn_handle);
#endif
		} else if (event->subscribe.attr_handle ==
			   svc_handles.prov_data_out_h) {
#if (MYNEWT_VAL(BLE_MESH_PB_GATT))
			prov_ccc_write(event->subscribe.conn_handle);
#endif
		}
	}

	return 0;
}

static int
dummy_access_cb(uint16_t conn_handle, uint16_t attr_handle,
		struct ble_gatt_access_ctxt *ctxt, void *arg)
{
	/*
	 * We should never never enter this callback - it's attached to notify-only
	 * characteristic which are notified directly from mbuf. And we can't pass
	 * NULL as access_cb because gatts will assert on init...
	 */
	BLE_HS_DBG_ASSERT(0);
	return 0;
}

static const struct ble_gatt_svc_def svc_defs [] = {
	{
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = BLE_UUID16_DECLARE(BT_UUID_MESH_PROXY_VAL),
		.characteristics = (struct ble_gatt_chr_def[]) { {
				.uuid = BLE_UUID16_DECLARE(BT_UUID_MESH_PROXY_DATA_IN_VAL),
				.access_cb = proxy_recv,
				.flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
			}, {
				.uuid = BLE_UUID16_DECLARE(BT_UUID_MESH_PROXY_DATA_OUT_VAL),
				.access_cb = dummy_access_cb,
				.flags = BLE_GATT_CHR_F_NOTIFY,
			}, {
				0, /* No more characteristics in this service. */
			} },
	}, {
		.type = BLE_GATT_SVC_TYPE_PRIMARY,
		.uuid = BLE_UUID16_DECLARE(BT_UUID_MESH_PROV_VAL),
		.characteristics = (struct ble_gatt_chr_def[]) { {
				.uuid = BLE_UUID16_DECLARE(BT_UUID_MESH_PROV_DATA_IN_VAL),
				.access_cb = proxy_recv,
				.flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
			}, {
				.uuid = BLE_UUID16_DECLARE(BT_UUID_MESH_PROV_DATA_OUT_VAL),
				.access_cb = dummy_access_cb,
				.flags = BLE_GATT_CHR_F_NOTIFY,
			}, {
				0, /* No more characteristics in this service. */
			} },
	}, {
		0, /* No more services. */
	},
};

int bt_mesh_proxy_svcs_register(void)
{
	int rc;

	rc = ble_gatts_count_cfg(svc_defs);
	assert(rc == 0);

	rc = ble_gatts_add_svcs(svc_defs);
	assert(rc == 0);

	return 0;
}

int bt_mesh_proxy_init(void)
{
	int i;

	for (i = 0; i < MYNEWT_VAL(BLE_MAX_CONNECTIONS); ++i) {
#if (MYNEWT_VAL(BLE_MESH_GATT_PROXY))
		k_work_init(&clients[i].send_beacons, proxy_send_beacons);
#endif
		clients[i].buf = NET_BUF_SIMPLE(CLIENT_BUF_SIZE);
	}

#if (MYNEWT_VAL(BLE_MESH_PB_GATT))
	memcpy(prov_svc_data + 2, bt_mesh_prov_get_uuid(), 16);
#endif

	resolve_svc_handles();

	ble_gatts_svc_set_visibility(svc_handles.proxy_h, 0);
	ble_gatts_svc_set_visibility(svc_handles.prov_h, 0);

	return 0;
}

#endif //MYNEWT_VAL(BLE_MESH_PROXY)
