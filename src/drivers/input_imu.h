/*
 * USB motion sensor: an ICM-20948 behind a Pico (the usb_motion_sensor
 * project), a CDC-ACM serial port (/dev/ttyACMn) that streams raw samples
 * (~1.1 kHz) in frames "A5 5A type len payload crc16" (protocol v2). Found
 * by its USB ids whenever it is plugged in; its gyro and accelerometer go
 * to core/input (input_report_motion()).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal/hal.h"

struct imu_params {
	unsigned vid, pid;	/* USB ids */
	const char *product;	/* its product string, NULL: any */
};

/* The usb_motion_sensor's firmware: pid.codes test ids */
#define IMU_PARAMS_PICO (&(const struct imu_params){ \
	.vid = 0x1209, .pid = 0x0001, .product = "USB Motion Sensor" })

extern const struct hal_input_ops input_imu;

/*
 * The stream's parser (the driver's, open for tests): bytes in any pieces;
 * each sample goes to input_report_motion(), input_report_motion_end()
 * after each call. Protocol v3 says what the sensor is (PKT_INFO: its name,
 * units, rate) and sends samples in batches (PKT_SAMPLES); v2 (firmware
 * 0.1) is the ICM-20948 alone, its units from the status. The sample period
 * is measured by the sensor's own clock (its rate differs from the nominal
 * one by a few percent), from samples that carry their own time.
 */
struct imu_parser {
	uint8_t buf[512];
	size_t len;
	unsigned proto;			/* 2, 3; 0 not known yet */
	bool have_status, have_info;
	char name[17];			/* the sensor's chip, "" none */
	uint8_t imu_id;			/* 0 none */
	unsigned info_seq;		/* +1 each time the sensor (or its units) changed */
	float gyro_scale, accel_scale;	/* deg/s, g per count */
	float gyro_noise;		/* deg/s rms at rest, 0 unknown */
	float nominal_us;		/* of a sample, as the device says */
	float period_us;		/* measured */
	bool period_measured;
	bool have_prev;
	uint16_t prev_seq;
	bool have_real;			/* the last sample with a time of its own */
	uint32_t real_t;
	uint16_t real_seq;
	uint64_t span_us;		/* measuring the period: time and samples */
	uint32_t span_n;
	uint32_t samples, bad_crc;
	bool streaming, imu_ok, fake;
};

void imu_parser_init(struct imu_parser *p);
void imu_parser_feed(struct imu_parser *p, const uint8_t *data, size_t n);
uint16_t imu_crc16(const uint8_t *data, size_t n);
