/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>
#include <string.h>
#include <sys/atomic.h>
#include <spinlock.h>
#include <settings/settings.h>
#include <sys/byteorder.h>
#include <drivers/i2c.h>
#include "bsec_integration.h"
#include "env_sensors.h"

#define ENV_INIT_DELAY_S (5) /* Polling delay upon initialization */
#define MAX_INTERVAL_S   (INT_MAX/MSEC_PER_SEC)

#include <logging/log.h>
LOG_MODULE_REGISTER(bsec, CONFIG_ASSET_TRACKER_LOG_LEVEL);

/* @brief Sample rate for the BSEC library
 *
 * BSEC_SAMPLE_RATE_ULP = 0.0033333 Hz = 300 second interval
 * BSEC_SAMPLE_RATE_LP = 0.33333 Hz = 3 second interval
 */
#define BSEC_SAMPLE_RATE	BSEC_SAMPLE_RATE_LP

/* @breif Interval for saving BSEC state to flash
 *
 * Interval = BSEC_STATE_INTERVAL * 1/BSEC_SAMPLE_RATE
 * Example:  600 * 1/0.33333 Hz = 1800 seconds = 0.5 hour
 */
#define BSEC_STATE_SAVE_INTERVAL	600

const struct device *i2c_master;

struct env_sensor {
	env_sensor_data_t sensor;
	struct k_spinlock lock;
};

static struct env_sensor temp_sensor = {
	.sensor =  {
		.type = ENV_SENSOR_TEMPERATURE,
	},
};

static struct env_sensor humid_sensor = {
	.sensor =  {
		.type = ENV_SENSOR_HUMIDITY,
	},
};

static struct env_sensor pressure_sensor = {
	.sensor =  {
		.type = ENV_SENSOR_AIR_PRESSURE,
	},
};

static struct env_sensor air_quality_sensor = {
	.sensor =  {
		.type = ENV_SENSOR_AIR_QUALITY,
	},
};

/* size of stack area used by bsec thread */
#define STACKSIZE 4096

static K_THREAD_STACK_DEFINE(thread_stack, STACKSIZE);
static struct k_thread thread;

static uint8_t s_state_buffer[BSEC_MAX_STATE_BLOB_SIZE];
static int32_t s_state_buffer_len;

static struct k_work_delayable env_sensors_poller;
static env_sensors_data_ready_cb data_ready_cb;
static uint32_t data_send_interval_s = CONFIG_ENVIRONMENT_DATA_SEND_INTERVAL;
static bool backoff_enabled;
static bool initialized;

static struct k_work_q *env_sensors_work_q;
#define SETTINGS_NAME_BSEC "bsec"
#define SETTINGS_KEY_STATE "state"
#define SETTINGS_BSEC_STATE SETTINGS_NAME_BSEC "/" SETTINGS_KEY_STATE

static int settings_set(const char *key, size_t len_rd,
			settings_read_cb read_cb, void *cb_arg)
{
	if (!key) {
		LOG_ERR("Invalid key");
		return -EINVAL;
	}

	LOG_DBG("Settings key: %s, size: %d", log_strdup(key), len_rd);

	if (!strncmp(key, SETTINGS_KEY_STATE, strlen(SETTINGS_KEY_STATE))) {
		if (len_rd > sizeof(s_state_buffer)) {
			LOG_ERR("Setting too big: %d", len_rd);
			return -EMSGSIZE;
		}

		s_state_buffer_len = read_cb(cb_arg, s_state_buffer, len_rd);
		if (s_state_buffer_len > 0) {
			return 0;
		} else {
			LOG_ERR("No settings data read");
			return -ENODATA;
		}
	}
	return -ENOTSUP;
}

SETTINGS_STATIC_HANDLER_DEFINE(bsec, SETTINGS_NAME_BSEC, NULL, settings_set,
			       NULL, NULL);

static int enable_settings(void)
{
	int err;

	err = settings_subsys_init();
	if (err) {
		LOG_ERR("Settings init failed: %d", err);
		return err;
	}

	err = settings_load_subtree(settings_handler_bsec.name);
	if (err) {
		LOG_ERR("Cannot load settings: %d", err);
	}

	return err;
}

static int8_t bus_write(uint8_t dev_addr, uint8_t reg_addr,
			uint8_t *reg_data_ptr, uint16_t data_len)
{
	uint8_t buf[data_len+1];

	buf[0] = reg_addr;
	memcpy(&buf[1], reg_data_ptr, data_len);
	return i2c_write(i2c_master, buf, data_len+1, dev_addr);
}

static int8_t bus_read(uint8_t dev_addr, uint8_t reg_addr,
		     uint8_t *reg_data_ptr, uint16_t data_len)
{
	return i2c_write_read(i2c_master, dev_addr, &reg_addr,
			      1, reg_data_ptr, data_len);
}

static int64_t get_timestamp_us(void)
{
	return k_uptime_get()*1000;
}

static void delay_ms(uint32_t period)
{
	k_sleep(K_MSEC(period));
}

static void output_ready(int64_t timestamp, float iaq, uint8_t iaq_accuracy,
			float temperature, float humidity, float pressure,
			float raw_temperature, float raw_humidity, float gas,
			bsec_library_return_t bsec_status, float static_iaq,
			float co2_equivalent, float breath_voc_equivalent)
{
	int64_t ts = k_uptime_get();
	k_spinlock_key_t key = k_spin_lock(&(temp_sensor.lock));
	temp_sensor.sensor.value = temperature;
	temp_sensor.sensor.ts = ts;
	k_spin_unlock(&(temp_sensor.lock), key);
	key = k_spin_lock(&(humid_sensor.lock));
	humid_sensor.sensor.value = humidity;
	humid_sensor.sensor.ts = ts;
	k_spin_unlock(&(humid_sensor.lock), key);
	key = k_spin_lock(&(pressure_sensor.lock));
	pressure_sensor.sensor.value = pressure / 1000;
	pressure_sensor.sensor.ts = ts;
	k_spin_unlock(&(pressure_sensor.lock), key);
	key = k_spin_lock(&(air_quality_sensor.lock));
	air_quality_sensor.sensor.value = iaq;
	air_quality_sensor.sensor.ts = ts;
	k_spin_unlock(&(air_quality_sensor.lock), key);
}

static uint32_t state_load(uint8_t *state_buffer, uint32_t n_buffer)
{
	if ((s_state_buffer_len > 0) && (s_state_buffer_len <= n_buffer)) {
		memcpy(state_buffer, s_state_buffer, s_state_buffer_len);
		return s_state_buffer_len;
	} else {
		LOG_DBG("No stored state to load");
		return 0;
	}
}

static void state_save(const uint8_t *state_buffer, uint32_t length)
{
	LOG_INF("Storing state to flash");
	if (length > sizeof(s_state_buffer)) {
		LOG_ERR("State buffer too big to save: %d", length);
		return;
	}

	int err = settings_save_one(SETTINGS_BSEC_STATE, state_buffer, length);

	if (err) {
		LOG_ERR("Storing state to flash failed");
	}
}

static uint32_t config_load(uint8_t *config_buffer, uint32_t n_buffer)
{
	/* Not implemented */
	return 0;
}

static void bsec_thread(void)
{
	bsec_iot_loop(delay_ms, get_timestamp_us, output_ready,
			state_save, BSEC_STATE_SAVE_INTERVAL);
}

int env_sensors_get_temperature(env_sensor_data_t *sensor_data)
{
	if (sensor_data == NULL) {
		return -1;
	}
	k_spinlock_key_t key = k_spin_lock(&temp_sensor.lock);

	memcpy(sensor_data, &temp_sensor.sensor, sizeof(temp_sensor.sensor));
	k_spin_unlock(&temp_sensor.lock, key);
	return 0;
}

int env_sensors_get_humidity(env_sensor_data_t *sensor_data)
{
	if (sensor_data == NULL) {
		return -1;
	}
	k_spinlock_key_t key = k_spin_lock(&humid_sensor.lock);

	memcpy(sensor_data, &humid_sensor.sensor,
		sizeof(humid_sensor.sensor));
	k_spin_unlock(&humid_sensor.lock, key);
	return 0;
}

int env_sensors_get_pressure(env_sensor_data_t *sensor_data)
{
	if (sensor_data == NULL) {
		return -1;
	}
	k_spinlock_key_t key = k_spin_lock(&pressure_sensor.lock);

	memcpy(sensor_data, &pressure_sensor.sensor,
		sizeof(pressure_sensor.sensor));
	k_spin_unlock(&pressure_sensor.lock, key);
	return 0;
}

int env_sensors_get_air_quality(env_sensor_data_t *sensor_data)
{
	if (sensor_data == NULL) {
		return -1;
	}
	k_spinlock_key_t key = k_spin_lock(&air_quality_sensor.lock);

	memcpy(sensor_data, &air_quality_sensor.sensor,
		sizeof(air_quality_sensor.sensor));
	k_spin_unlock(&air_quality_sensor.lock, key);
	return 0;
}

static inline int submit_poll_work(const uint32_t delay_s)
{
	return k_work_reschedule_for_queue(env_sensors_work_q,
					      &env_sensors_poller,
					      K_SECONDS((uint32_t)delay_s));
}

int env_sensors_poll(void)
{
	return initialized ? submit_poll_work(0) : -ENXIO;
}

static void env_sensors_poll_fn(struct k_work *work)
{

	if (data_send_interval_s == 0) {
		return;
	}

	if (data_ready_cb) {
		data_ready_cb();
	}

	submit_poll_work(backoff_enabled ?
		CONFIG_ENVIRONMENT_DATA_BACKOFF_TIME : data_send_interval_s);
}

int env_sensors_init_and_start(struct k_work_q *work_q,
			       const env_sensors_data_ready_cb cb)
{
	return_values_init bsec_ret;
	int ret;

	if ((work_q == NULL) || (cb == NULL)) {
		return -EINVAL;
	}

	i2c_master = device_get_binding("I2C_2");
	if (!i2c_master) {
		LOG_ERR("cannot bind to BME680");
		return -EINVAL;
	}

	ret = enable_settings();
	if (ret) {
		LOG_ERR("Cannot enable settings err: %d", ret);
		return ret;
	}

	bsec_ret = bsec_iot_init(BSEC_SAMPLE_RATE, 1.2f, bus_write,
				bus_read, delay_ms, state_load,
				config_load);

	if (bsec_ret.bme680_status) {
		LOG_ERR("Could not initialize BME680: %d",
			(int)bsec_ret.bme680_status);
		return (int)bsec_ret.bme680_status;
	} else if (bsec_ret.bsec_status) {
		LOG_ERR("Could not initialize BSEC library: %d",
			(int)bsec_ret.bsec_status);
		if ((int)bsec_ret.bsec_status == -34) {
			LOG_ERR("Deleting state from flash");
			settings_delete(SETTINGS_BSEC_STATE);
		}
		return (int)bsec_ret.bsec_status;
	}

	k_thread_create(&thread, thread_stack, STACKSIZE,
			(k_thread_entry_t)bsec_thread, NULL, NULL, NULL,
			CONFIG_SYSTEM_WORKQUEUE_PRIORITY, 0, K_NO_WAIT);

	data_ready_cb = cb;

	env_sensors_work_q = work_q;

	k_work_init_delayable(&env_sensors_poller, env_sensors_poll_fn);

	initialized = true;

	return (data_send_interval_s > 0) ?
		submit_poll_work(ENV_INIT_DELAY_S) : 0;
}

void env_sensors_set_send_interval(const uint32_t interval_s)
{
	if (interval_s == data_send_interval_s) {
		return;
	}

	data_send_interval_s = MIN(interval_s, MAX_INTERVAL_S);

	if (!initialized) {
		return;
	}

	if (data_send_interval_s) {
		/* restart work for new interval to take effect */
		env_sensors_poll();
	} else {
		/* If cancel fails poller will return early when checking data_send_interval_s */
		k_work_cancel_delayable(&env_sensors_poller);
	}
}

uint32_t env_sensors_get_send_interval(void)
{
	return data_send_interval_s;
}

void env_sensors_set_backoff_enable(const bool enable)
{
	backoff_enabled = enable;
}
