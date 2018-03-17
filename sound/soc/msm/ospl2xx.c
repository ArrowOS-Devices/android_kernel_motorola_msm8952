/* Copyright (c) 2015-2016, Motorola Mobility, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/firmware.h>
#include <sound/soc.h>
#include <sound/core.h>
#include <sound/q6afe-v2.h>
#include <sound/ospl2xx.h>
#include "../codecs/msm8x16-wcd.h"

/*
 * external configuration string reading
 */
static char const *ospl2xx_ext_config_tables[] = {
	"opalum.rx.ext.config.0",
	"opalum.rx.ext.config.1",
	"opalum.rx.ext.config.2",
	"opalum.tx.ext.config.0",
};

static const struct firmware
		*ospl2xx_config[ARRAY_SIZE(ospl2xx_ext_config_tables)];

static void ospl2xx_config_print(int i, char const *firmware, int size)
{
	u8 config[size+1];

	if (size > 0 && firmware != NULL) {
		memcpy(config, firmware, size);
		config[size] = '\0';
		pr_debug("index[%d] size[%d] config[%s]\n", i, size, config);
	} else {
		pr_err("%s: can't find ospl external configs\n", __func__);
	}
}

static DEFINE_MUTEX(lr_lock);
static void ospl2xx_load_config(struct work_struct *work)
{
	int i, ret;
	struct msm8x16_wcd_priv *msm8x16_wcd =
		container_of(work, struct msm8x16_wcd_priv, ospl2xx_config);
	struct snd_soc_codec *codec = msm8x16_wcd->codec;

	if (codec == NULL) {
		pr_err("can't load external configuration\n");
		return;
	}

	mutex_lock(&lr_lock);
	for (i = 0; i < ARRAY_SIZE(ospl2xx_ext_config_tables); i++) {
		ret = request_firmware(&ospl2xx_config[i],
			ospl2xx_ext_config_tables[i], codec->dev);
		if (ret || ospl2xx_config[i]->data == NULL) {
			pr_err("failed to load config\n");
		} else {
			pr_info("loading cfg file %s\n",
				ospl2xx_ext_config_tables[i]);
			ospl2xx_config_print(i, ospl2xx_config[i]->data,
						ospl2xx_config[i]->size);
		}
	}
	mutex_unlock(&lr_lock);
}

/*
 * OSPL Hexagon AFE communication
 */
int ospl2xx_afe_set_single_param(uint32_t param_id, int32_t arg1)
{
	int result = 0;
	int index = 0;
	int size = 0;
	uint32_t module_id = 0, port_id = 0;
	struct afe_custom_opalum_set_config_t *config = NULL;
	struct opalum_single_data_ctrl_t *settings = NULL;

	/* Destination settings for message */
	if ((param_id >> 8) << 8 == AFE_CUSTOM_OPALUM_RX_MODULE) {
		module_id = AFE_CUSTOM_OPALUM_RX_MODULE;
		port_id = AFE_PORT_ID_QUINARY_MI2S_RX;
	} else if ((param_id >> 8) << 8 == AFE_CUSTOM_OPALUM_TX_MODULE) {
		module_id = AFE_CUSTOM_OPALUM_TX_MODULE;
		port_id = AFE_PORT_ID_QUINARY_MI2S_TX;
	} else {
		pr_err("%s: unsupported paramID[0x%08X]\n", __func__, param_id);
		return -EINVAL;
	}
	if (param_id == AFE_CUSTOM_OPALUM_RX_MODULE ||
	    param_id == AFE_CUSTOM_OPALUM_TX_MODULE) {
		param_id = AFE_PARAM_ID_ENABLE;
	}
	index = afe_get_port_index(port_id);

	/* Allocate memory for the message */
	size = sizeof(struct afe_custom_opalum_set_config_t) +
		sizeof(struct opalum_single_data_ctrl_t);
	config = kzalloc(size, GFP_KERNEL);
	if (config == NULL) {
		pr_err("%s: Memory allocation failed!\n", __func__);
		return -ENOMEM;
	}
	settings = (struct opalum_single_data_ctrl_t *)
		((u8 *)config + sizeof(struct afe_custom_opalum_set_config_t));

	/* Configure actual parameter settings */
	settings->value = arg1;

	/* Set header section */
	config->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
						APR_HDR_LEN(APR_HDR_SIZE),
						APR_PKT_VER);
	config->hdr.pkt_size = size;
	config->hdr.src_svc = APR_SVC_AFE;
	config->hdr.src_domain = APR_DOMAIN_APPS;
	config->hdr.src_port = 0;
	config->hdr.dest_svc = APR_SVC_AFE;
	config->hdr.dest_domain = APR_DOMAIN_ADSP;
	config->hdr.dest_port = 0;
	config->hdr.token = index;
	config->hdr.opcode = AFE_PORT_CMD_SET_PARAM_V2;

	/* Set param section */
	config->param.port_id = port_id;
	config->param.payload_size =
		sizeof(struct afe_port_param_data_v2) +
		sizeof(struct opalum_single_data_ctrl_t);
	config->param.payload_address_lsw = 0;
	config->param.payload_address_msw = 0;
	config->param.mem_map_handle = 0;

	/* Set data section */
	config->data.module_id = module_id;
	config->data.param_id = param_id;
	config->data.param_size = sizeof(struct opalum_single_data_ctrl_t);
	config->data.reserved = 0;

	result = ospl2xx_afe_apr_send_pkt(config, index);
	if (result) {
		pr_err("%s: set_param for port %d failed with code %d\n",
			 __func__, port_id, result);
	} else {
		pr_info("%s: set_param packet param 0x%08X to module 0x%08X\n",
			__func__, param_id, module_id);
	}
	kfree(config);
	return result;
}

int ospl2xx_afe_set_tri_param(uint32_t param_id,
			int32_t arg1, int32_t arg2, int32_t arg3) {
	int result = 0;
	int index = 0;
	int size = 0;
	uint32_t port_id = 0, module_id = 0;
	struct afe_custom_opalum_set_config_t *config = NULL;
	struct opalum_tri_data_ctrl_t *settings = NULL;

	/* Destination settings for message */
	if ((param_id >> 8) << 8 ==
		AFE_CUSTOM_OPALUM_RX_MODULE) {
		module_id = AFE_CUSTOM_OPALUM_RX_MODULE;
		port_id = AFE_PORT_ID_QUINARY_MI2S_RX;
	} else if ((param_id >> 8) << 8 ==
		AFE_CUSTOM_OPALUM_TX_MODULE) {
		module_id = AFE_CUSTOM_OPALUM_TX_MODULE;
		port_id = AFE_PORT_ID_QUINARY_MI2S_TX;
	} else {
		pr_err("%s: unsupported paramID[0x%08X]\n",
			__func__, param_id);
		return -EINVAL;
	}
	index = afe_get_port_index(port_id);

	/* Allocate memory for the message */
	size = sizeof(struct afe_custom_opalum_set_config_t) +
		sizeof(struct opalum_tri_data_ctrl_t);
	config = kzalloc(size, GFP_KERNEL);
	if (config == NULL) {
		pr_err("%s: Memory allocation failed!\n", __func__);
		return -ENOMEM;
	}
	settings = (struct opalum_tri_data_ctrl_t *)
		((u8 *)config + sizeof(struct afe_custom_opalum_set_config_t));

	/* Configure actual parameter settings */
	settings->data1 = arg1;
	settings->data2 = arg2;
	settings->data3 = arg3;

	/* Set header section */
	config->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
						APR_HDR_LEN(APR_HDR_SIZE),
						APR_PKT_VER);
	config->hdr.pkt_size = size;
	config->hdr.src_svc = APR_SVC_AFE;
	config->hdr.src_domain = APR_DOMAIN_APPS;
	config->hdr.src_port = 0;
	config->hdr.dest_svc = APR_SVC_AFE;
	config->hdr.dest_domain = APR_DOMAIN_ADSP;
	config->hdr.dest_port = 0;
	config->hdr.token = index;
	config->hdr.opcode = AFE_PORT_CMD_SET_PARAM_V2;

	/* Set param section */
	config->param.port_id = port_id;
	config->param.payload_size =
		sizeof(struct afe_port_param_data_v2) +
		sizeof(struct opalum_tri_data_ctrl_t);
	config->param.payload_address_lsw = 0;
	config->param.payload_address_msw = 0;
	config->param.mem_map_handle = 0;

	/* Set data section */
	config->data.module_id = module_id;
	config->data.param_id = param_id;
	config->data.param_size = sizeof(struct opalum_tri_data_ctrl_t);
	config->data.reserved = 0;
	result = ospl2xx_afe_apr_send_pkt(config, index);
	if (result) {
		pr_err("%s: set_param for port %d failed with code %d\n",
			__func__, port_id, result);
	} else {
		pr_info("%s: set_param packet param 0x%08X to module 0x%08X\n",
			__func__, param_id, module_id);
	}
	kfree(config);
	return result;
}

int ospl2xx_afe_set_ext_config_param(const char *string, uint32_t param_id)
{
	int result = 0;
	int index = 0;
	int size = 0;
	int string_size = 0;
	int mem_size = 0;
	int sent = 0;
	int chars_to_send = 0;
	uint32_t port_id = 0, module_id = 0;
	struct afe_custom_opalum_set_config_t *config = NULL;
	struct opalum_external_config_t *payload = NULL;

	/* Destination settings for message */
	if ((param_id >> 8) << 8 ==
		AFE_CUSTOM_OPALUM_RX_MODULE) {
		module_id = AFE_CUSTOM_OPALUM_RX_MODULE;
		port_id = AFE_PORT_ID_QUINARY_MI2S_RX;
	} else if ((param_id >> 8) << 8 ==
		AFE_CUSTOM_OPALUM_TX_MODULE) {
		module_id = AFE_CUSTOM_OPALUM_TX_MODULE;
		port_id = AFE_PORT_ID_QUINARY_MI2S_TX;
	} else {
		pr_err("%s: unsupported paramID[0x%08X]\n",
			__func__, param_id);
		return -EINVAL;
	}
	index = afe_get_port_index(port_id);

	string_size = strlen(string);
	if (string_size > 4000)
		mem_size = 4000;
	else
		mem_size = string_size;

	/* Allocate memory for the message */
	size = sizeof(struct afe_custom_opalum_set_config_t) +
		sizeof(struct opalum_external_config_t) + mem_size;
	config = kzalloc(size, GFP_KERNEL);
	if (config == NULL) {
		pr_err("%s: Memory allocation failed!\n", __func__);
		return -ENOMEM;
	}
	payload = (struct opalum_external_config_t *)
		((u8 *)config + sizeof(struct afe_custom_opalum_set_config_t));
	payload->total_size = (uint32_t)string_size;

	/* Initial configuration of message */
	/* Set header section */
	config->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
						APR_HDR_LEN(APR_HDR_SIZE),
						APR_PKT_VER);
	config->hdr.pkt_size = size;
	config->hdr.src_svc = APR_SVC_AFE;
	config->hdr.src_domain = APR_DOMAIN_APPS;
	config->hdr.src_port = 0;
	config->hdr.dest_svc = APR_SVC_AFE;
	config->hdr.dest_domain = APR_DOMAIN_ADSP;
	config->hdr.dest_port = 0;
	config->hdr.token = index;
	config->hdr.opcode = AFE_PORT_CMD_SET_PARAM_V2;

	/* Set param section */
	config->param.port_id = port_id;
	config->param.payload_size =
		sizeof(struct afe_port_param_data_v2) +
		sizeof(struct opalum_external_config_t) +
		mem_size;
	config->param.payload_address_lsw = 0;
	config->param.payload_address_msw = 0;
	config->param.mem_map_handle = 0;

	/* Set data section */
	config->data.module_id = module_id;
	config->data.param_id = param_id;
	config->data.param_size =
		sizeof(struct opalum_external_config_t) +
		mem_size;
	config->data.reserved = 0;

	/* Send config string in chunks of maximum 4000 bytes */
	while (sent < string_size) {
		chars_to_send = string_size - sent;
		if (chars_to_send > 4000) {
			chars_to_send = 4000;
			payload->done = 0;
		} else {
			payload->done = 1;
		}

		/* Configure per message parameter settings */
		memcpy(&payload->config, string + sent, chars_to_send);
		payload->chunk_size = chars_to_send;

		/* Send the actual message */
		result = ospl2xx_afe_apr_send_pkt(config, index);
		if (result) {
			pr_err("%s: set_param for port %d failed, code %d\n",
				__func__, port_id, result);
		} else {
			pr_info("%s: set_param param 0x%08X to module 0x%08X\n",
				 __func__, param_id, module_id);
		}
		sent += chars_to_send;
	}
	kfree(config);
	return result;
}

int ospl2xx_afe_get_param(uint32_t param_id)
{
	int result = 0;
	int index = 0;
	int size = 0;
	uint32_t module_id = 0, port_id = 0;
	struct afe_custom_opalum_get_config_t *config = NULL;

	if ((param_id >> 8) << 8 == AFE_CUSTOM_OPALUM_RX_MODULE) {
		module_id = AFE_CUSTOM_OPALUM_RX_MODULE;
		port_id = AFE_PORT_ID_QUINARY_MI2S_RX;
	} else if ((param_id >> 8) << 8 == AFE_CUSTOM_OPALUM_TX_MODULE) {
		module_id = AFE_CUSTOM_OPALUM_TX_MODULE;
		port_id = AFE_PORT_ID_QUINARY_MI2S_TX;
	} else {
		pr_err("%s: unsupported paramID[0x%08X]\n", __func__, param_id);
		return -EINVAL;
	}

	index = afe_get_port_index(port_id);

	/* Allocate memory for the message */
	size = sizeof(struct afe_custom_opalum_get_config_t);
	config = kzalloc(size, GFP_KERNEL);
	if (config == NULL) {
		pr_err("%s: Memory allocation failed!\n", __func__);
		return -ENOMEM;
	}

	/* Set header section */
	config->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
						APR_HDR_LEN(APR_HDR_SIZE),
						APR_PKT_VER);
	config->hdr.pkt_size = size;
	config->hdr.src_svc = APR_SVC_AFE;
	config->hdr.src_domain = APR_DOMAIN_APPS;
	config->hdr.src_port = 0;
	config->hdr.dest_svc = APR_SVC_AFE;
	config->hdr.dest_domain = APR_DOMAIN_ADSP;
	config->hdr.dest_port = 0;
	config->hdr.token = index;
	config->hdr.opcode = AFE_PORT_CMD_GET_PARAM_V2;

	/* Set param section */
	config->param.port_id = port_id;
	config->param.payload_size =
		sizeof(struct afe_port_param_data_v2) +
		sizeof(struct opalum_tri_data_ctrl_t);
	config->param.payload_address_lsw = 0;
	config->param.payload_address_msw = 0;
	config->param.mem_map_handle = 0;
	config->param.module_id = module_id;
	config->param.param_id = param_id;

	/* Set data section */
	config->data.module_id = module_id;
	config->data.param_id = param_id;
	config->data.param_size = sizeof(struct opalum_tri_data_ctrl_t);
	config->data.reserved = 0;

	result = ospl2xx_afe_apr_send_pkt(config, index);
	if (result) {
		pr_err("%s: get_param for port %d failed with code %d\n",
			__func__, port_id, result);
	} else {
		pr_info("%s: get_param packet param 0x%08X to module 0x%08X\n",
			__func__, param_id, module_id);
	}

	kfree(config);
	return result;
}

/*
 * AFE callback
 */
int32_t afe_cb_payload32_data[2] = {0,};
static int32_t ospl2xx_afe_callback(struct apr_client_data *data)
{
	if (!data) {
		pr_err("%s: Invalid param data\n", __func__);
		return -EINVAL;
	}

	if (data->opcode == AFE_PORT_CMDRSP_GET_PARAM_V2) {
		uint32_t *payload32 = data->payload;

		if (payload32[1] == AFE_CUSTOM_OPALUM_RX_MODULE ||
		    payload32[1] == AFE_CUSTOM_OPALUM_TX_MODULE) {
			switch (payload32[2]) {
			case PARAM_ID_OPALUM_RX_EXC_MODEL:
			case PARAM_ID_OPLAUM_RX_TEMPERATURE:
			case PARAM_ID_OPALUM_TX_F0_CALIBRATION_VALUE:
			case PARAM_ID_OPALUM_TX_TEMP_MEASUREMENT_VALUE:
				afe_cb_payload32_data[0] = payload32[4];
				afe_cb_payload32_data[1] = payload32[5];
				pr_info("%s, payload-module_id = 0x%08X, ",
					__func__, (int) payload32[1]);
				pr_info("payload-param_id = 0x%08X, ",
					(int) payload32[2]);
				pr_info("afe_cb_payload32_data[0] = %d, ",
					(int) afe_cb_payload32_data[0]);
				pr_info("afe_cb_payload32_data[1] = %d\n",
					(int) afe_cb_payload32_data[1]);
				break;
			default:
				break;
			}
		}
	}

	return 0;
}

/*
 * mixer controls
 */
static DEFINE_MUTEX(mr_lock);
/* RX */
/* AFE_PARAM_ID_ENABLE */
static int ospl2xx_rx_enable_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int enable = ucontrol->value.integer.value[0];

	ospl2xx_afe_set_single_param(
		AFE_CUSTOM_OPALUM_RX_MODULE,
		enable);

	return 0;
}
static int ospl2xx_rx_enable_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}
static const char *const ospl2xx_rx_enable_text[] = {"Disable", "Enable"};
static const struct soc_enum ospl2xx_rx_enable_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, ospl2xx_rx_enable_text),
};

/* PARAM_ID_OPALUM_RX_SET_USE_CASE */
static int ospl2xx_rx_int_config_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}
static int ospl2xx_rx_int_config_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	ospl2xx_afe_set_single_param(
		PARAM_ID_OPALUM_RX_SET_USE_CASE,
		(int)ucontrol->value.integer.value[0]);

	return 0;
}
static char const *ospl2xx_rx_int_config_index[] = {
	"opalum.rx.int.config.0",
	"opalum.rx.int.config.1",
	"opalum.rx.int.config.2",
};
static const struct soc_enum ospl2xx_rx_int_config_enum[] = {
	SOC_ENUM_SINGLE_EXT(3, ospl2xx_rx_int_config_index),
};

/* PARAM_ID_OPALUM_RX_RUN_CALIBRATION */
static int ospl2xx_rx_run_diagnostic(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int command = ucontrol->value.integer.value[0];

	if (command != 1)
		return 0;

	ospl2xx_afe_set_single_param(PARAM_ID_OPALUM_RX_RUN_CALIBRATION, 1);

	return 0;
}
static int ospl2xx_rx_run_diagnostic_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	/* not supported */
	return 0;
}
static const char *const ospl2xx_rx_run_diagnostic_text[] = {"Off", "Run"};
static const struct soc_enum ospl2xx_rx_run_diagnostic_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, ospl2xx_rx_run_diagnostic_text),
};

/* PARAM_ID_OPALUM_RX_SET_EXTERNAL_CONFIG */
static int ospl2xx_rx_ext_config_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	/* not implemented */
	return 0;
}
static int ospl2xx_rx_ext_config_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int i = ucontrol->value.integer.value[0];
	int size = ospl2xx_config[i]->size;
	char config[size+1];

	memcpy(config, ospl2xx_config[i]->data, size);
	config[size] = '\0';

	pr_info("%s: ospl2xx_config[%d] size[%d]\n",
		__func__, i, size);

	ospl2xx_afe_set_ext_config_param(config,
		PARAM_ID_OPALUM_RX_SET_EXTERNAL_CONFIG);

	return 0;
}
#define NUM_RX_CONFIGS 3
static char const *ospl2xx_rx_ext_config_text[] = {
	"opalum.rx.ext.config.0",
	"opalum.rx.ext.config.1",
	"opalum.rx.ext.config.2",
};
static const struct soc_enum ospl2xx_rx_ext_config_enum[] = {
	SOC_ENUM_SINGLE_EXT(NUM_RX_CONFIGS,
		ospl2xx_rx_ext_config_text),
};

/* PARAM_ID_OPALUM_RX_EXC_MODEL */
static int ospl2xx_rx_get_exc_model(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	mutex_lock(&mr_lock);
	ospl2xx_afe_set_callback(ospl2xx_afe_callback);
	ospl2xx_afe_get_param(PARAM_ID_OPALUM_RX_EXC_MODEL);

	ucontrol->value.integer.value[0] = afe_cb_payload32_data[0];
	mutex_unlock(&mr_lock);

	return 0;
}
static int ospl2xx_rx_put_exc_model(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	/* not implemented yet */
	return 0;
}

/* PARAM_ID_OPLAUM_RX_TEMPERATURE */
static int ospl2xx_rx_get_temperature(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	mutex_lock(&mr_lock);
	ospl2xx_afe_set_callback(ospl2xx_afe_callback);
	ospl2xx_afe_get_param(PARAM_ID_OPLAUM_RX_TEMPERATURE);

	ucontrol->value.integer.value[0] = afe_cb_payload32_data[0];
	mutex_unlock(&mr_lock);

	return 0;
}
static int ospl2xx_rx_put_temperature(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	/* not implemented yet */
	return 0;
}

/* PARAM_ID_OPALUM_RX_TEMP_CAL_DATA */
static int ospl2xx_rx_put_temp_cal(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int32_t accumulated = ucontrol->value.integer.value[0];
	int32_t count       = ucontrol->value.integer.value[1];
	int32_t temperature = ucontrol->value.integer.value[2];

	mutex_lock(&mr_lock);
	pr_debug("%s, acc[%d], cnt[%d], tmpr[%d]\n", __func__,
			accumulated, count, temperature);
	ospl2xx_afe_set_tri_param(
			PARAM_ID_OPALUM_RX_TEMP_CAL_DATA,
			accumulated, count, temperature);
	mutex_unlock(&mr_lock);

	return 0;
}

/* TX */
/* AFE_PARAM_ID_ENABLE */
static int ospl2xx_tx_enable_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int enable = ucontrol->value.integer.value[0];

	ospl2xx_afe_set_single_param(
		AFE_CUSTOM_OPALUM_TX_MODULE,
		enable);

	return 0;
}
static int ospl2xx_tx_enable_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	/* not implemented yet */
	return 0;
}
static const char *const ospl2xx_tx_enable_text[] = {"Disable", "Enable"};
static const struct soc_enum ospl2xx_tx_enable_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, ospl2xx_tx_enable_text),
};

/* PARAM_ID_OPALUM_TX_RUN_CALIBRATION */
static int ospl2xx_tx_run_diagnostic(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int command = ucontrol->value.integer.value[0];

	if (command != 1)
		return 0;
	ospl2xx_afe_set_single_param(PARAM_ID_OPALUM_TX_RUN_CALIBRATION, 1);

	return 0;
}
static int ospl2xx_tx_run_diagnostic_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	/* not supported */
	return 0;
}
static const char *const ospl2xx_tx_run_diagnostic_text[] = {"Off", "Run"};
static const struct soc_enum ospl2xx_tx_run_diagnostic_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, ospl2xx_tx_run_diagnostic_text),
};

/* PARAM_ID_OPALUM_TX_F0_CALIBRATION_VALUE */
static int ospl2xx_tx_get_f0(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	mutex_lock(&mr_lock);
	ospl2xx_afe_set_callback(ospl2xx_afe_callback);
	ospl2xx_afe_get_param(PARAM_ID_OPALUM_TX_F0_CALIBRATION_VALUE);

	ucontrol->value.integer.value[0] = afe_cb_payload32_data[0];
	mutex_unlock(&mr_lock);

	return 0;
}

/* PARAM_ID_OPALUM_TX_TEMP_MEASUREMENT_VALUE */
static int ospl2xx_tx_get_temp_cal(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	mutex_lock(&mr_lock);
	ospl2xx_afe_set_callback(ospl2xx_afe_callback);
	ospl2xx_afe_get_param(PARAM_ID_OPALUM_TX_TEMP_MEASUREMENT_VALUE);

	ucontrol->value.integer.value[0] = afe_cb_payload32_data[0];
	ucontrol->value.integer.value[1] = afe_cb_payload32_data[1];
	mutex_unlock(&mr_lock);

	return 0;
}

/* PARAM_ID_OPALUM_TX_SET_EXTERNAL_CONFIG */
static int ospl2xx_tx_ext_config_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	/* not implemented yet */
	return 0;
}

static int ospl2xx_tx_ext_config_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int i = ucontrol->value.integer.value[0] + NUM_RX_CONFIGS;
	int size = ospl2xx_config[i]->size;
	char config[size+1];

	memcpy(config, ospl2xx_config[i]->data, size);
	config[size] = '\0';

	pr_info("%s: ospl2xx_config[%d] size[%d]\n",
		__func__, i, size);

	ospl2xx_afe_set_ext_config_param(config,
		PARAM_ID_OPALUM_TX_SET_EXTERNAL_CONFIG);

	return 0;
}
static char const *ospl2xx_tx_ext_config_text[] = {
	"opalum.tx.ext.config.0",
};
static const struct soc_enum ospl2xx_tx_ext_config_enum[] = {
	SOC_ENUM_SINGLE_EXT(1, ospl2xx_tx_ext_config_text),
};

static const struct snd_kcontrol_new ospl2xx_params_controls[] = {
	/* PUT */ SOC_ENUM_EXT("OSPL Rx",
		ospl2xx_rx_enable_enum[0],
		ospl2xx_rx_enable_get, ospl2xx_rx_enable_put),
	/* PUT */ SOC_ENUM_EXT("OSPL Int RxConfig",
		ospl2xx_rx_int_config_enum[0],
		ospl2xx_rx_int_config_get, ospl2xx_rx_int_config_put),
	/* PUT */ SOC_ENUM_EXT("OSPL Ext RxConfig",
		ospl2xx_rx_ext_config_enum[0],
		ospl2xx_rx_ext_config_get, ospl2xx_rx_ext_config_put),
	/* PUT */ SOC_ENUM_EXT("OSPL Rx diagnostic",
		ospl2xx_rx_run_diagnostic_enum[0],
		ospl2xx_rx_run_diagnostic_get, ospl2xx_rx_run_diagnostic),
	/* GET */ SOC_SINGLE_EXT("OSPL Rx exc_model",
		SND_SOC_NOPM, 0, 0xFFFF, 0,
		ospl2xx_rx_get_exc_model, ospl2xx_rx_put_exc_model),
	/* GET */ SOC_SINGLE_EXT("OSPL Rx temperature",
		SND_SOC_NOPM, 0, 0xFFFF, 0,
		ospl2xx_rx_get_temperature, ospl2xx_rx_put_temperature),
	/* PUT */ SOC_SINGLE_MULTI_EXT("OSPL Rx temp_cal",
		SND_SOC_NOPM, 0, 0xFFFF, 0, 3,
		NULL, ospl2xx_rx_put_temp_cal),

	/* PUT */ SOC_ENUM_EXT("OSPL Tx",
		ospl2xx_tx_enable_enum[0],
		ospl2xx_tx_enable_get, ospl2xx_tx_enable_put),
	/* PUT */ SOC_ENUM_EXT("OSPL Tx diagnostic",
		ospl2xx_tx_run_diagnostic_enum[0],
		ospl2xx_tx_run_diagnostic_get, ospl2xx_tx_run_diagnostic),
	/* GET */ SOC_SINGLE_EXT("OSPL Tx F0",
		SND_SOC_NOPM, 0, 0xFFFF, 0,
		ospl2xx_tx_get_f0, NULL),
	/* GET */ SOC_SINGLE_MULTI_EXT("OSPL Tx temp_cal",
		SND_SOC_NOPM, 0, 0xFFFF, 0, 2,
		ospl2xx_tx_get_temp_cal, NULL),
	/* PUT */ SOC_ENUM_EXT("OSPL Ext TxConfig",
		ospl2xx_tx_ext_config_enum[0],
		ospl2xx_tx_ext_config_get, ospl2xx_tx_ext_config_put),
};

/*
 * ospl2xx initialization
 * - register mixer controls
 * - load external configuration strings
 */
int ospl2xx_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct msm8x16_wcd_priv *msm8x16_wcd =
		snd_soc_codec_get_drvdata(codec);

	snd_soc_add_codec_controls(codec, ospl2xx_params_controls,
			ARRAY_SIZE(ospl2xx_params_controls));

	msm8x16_wcd->ospl2xx_wq =
		create_singlethread_workqueue("ospl2xx");
	if (msm8x16_wcd->ospl2xx_wq == NULL)
		return -ENOMEM;

	if (msm8x16_wcd->codec == NULL)
		msm8x16_wcd->codec = codec;
	INIT_WORK(&msm8x16_wcd->ospl2xx_config, ospl2xx_load_config);
	queue_work(msm8x16_wcd->ospl2xx_wq, &msm8x16_wcd->ospl2xx_config);

	return 0;
}
