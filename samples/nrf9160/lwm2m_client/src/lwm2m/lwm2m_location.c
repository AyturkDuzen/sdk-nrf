/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>
#include <drivers/gps.h>
#include <net/lwm2m.h>
#include <net/lwm2m_path.h>

#include "gps_pvt_event.h"
#include "lwm2m_app_utils.h"

#define MODULE lwm2m_app_loc

#include <logging/log.h>
LOG_MODULE_REGISTER(MODULE, CONFIG_APP_LOG_LEVEL);

static bool event_handler(const struct event_header *eh)
{
	if (is_gps_pvt_event(eh)) {
		struct gps_pvt_event *event = cast_gps_pvt_event(eh);

		float32_value_t latitude = double_to_float32(event->pvt.latitude);
		float32_value_t longitude = double_to_float32(event->pvt.longitude);
		float32_value_t altitude = double_to_float32(event->pvt.altitude);
		float32_value_t speed = double_to_float32(event->pvt.speed);
		float32_value_t radius = double_to_float32(event->pvt.accuracy);

		lwm2m_engine_set_float32(LWM2M_PATH(LWM2M_OBJECT_LOCATION_ID, 0, LATITUDE_RID),
					 &latitude);
		lwm2m_engine_set_float32(LWM2M_PATH(LWM2M_OBJECT_LOCATION_ID, 0, LONGITUDE_RID),
					 &longitude);
		lwm2m_engine_set_float32(LWM2M_PATH(LWM2M_OBJECT_LOCATION_ID, 0, ALTITUDE_RID),
					 &altitude);
		lwm2m_engine_set_float32(
			LWM2M_PATH(LWM2M_OBJECT_LOCATION_ID, 0, LOCATION_RADIUS_RID), &radius);
		lwm2m_engine_set_float32(
			LWM2M_PATH(LWM2M_OBJECT_LOCATION_ID, 0, LOCATION_SPEED_RID), &speed);

		return true;
	}

	return false;
}

EVENT_LISTENER(MODULE, event_handler);
EVENT_SUBSCRIBE(MODULE, gps_pvt_event);
