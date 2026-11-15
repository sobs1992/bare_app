/*
 * USB motion sensor: an IMU behind a Pico (the usb_motion_sensor project:
 * whichever chip its firmware drives, it says which), a CDC-ACM serial port (/dev/ttyACMn) that streams raw samples
 * in frames "A5 5A type len payload crc16" (protocol v3). Found
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
 * after each call. Protocol v3 (firmware 0.2 on, the only one read) says
 * what the sensor is (PKT_INFO: its name, units, rate) and sends samples in
 * batches (PKT_SAMPLES): nothing is assumed of the chip. Each PKT_INFO
 * that changes the sensor tells core/input at once, before its samples
 * (input_motion_sensor(): id "<name>-<serial>", channels, noise). The
 * sample period is measured by the sensor's own clock, from samples that
 * carry their own time: anew over each SPAN_US (a quarter second), all the
 * time over all the samples of the intervals within the times' jitter of
 * their median, taken within 0.5..2x of the info's rate
 * (beyond it the info's again). Missing samples by time count beyond the
 * period measured, or beyond twice the info's while there is none.
 */
#define SPAN_MAX 1024		/* intervals kept over SPAN_US */
#define SPAN_MIN 8		/* ...at least this many (a slow sensor's) */

struct imu_parser {
	uint8_t buf[512];
	size_t len;
	unsigned proto;			/* the status' or info's; only 3 is read; 0 not known yet */
	bool have_status, have_info;
	char serial[33];		/* the USB device's (set by the driver), for the id */
	char name[17];			/* the sensor, as the device names it; "" none */
	uint8_t imu_id;			/* 0 none */
	uint8_t channels;		/* PKT_INFO.channels: 1 gyro, 2 accel, 4 mag, 8 temp */
	uint32_t odr_mhz;		/* PKT_INFO's rate */
	unsigned info_seq;		/* +1 each time the sensor (or its units) changed */
	float gyro_scale, accel_scale;	/* deg/s, g per count */
	float gyro_noise;		/* deg/s rms at rest, 0 unknown */
	float accel_noise;		/* g rms at rest, 0 unknown */
	size_t batch_bytes;		/* the last PKT_SAMPLES frame's length (the driver's VMIN) */
	float nominal_us;		/* of a sample, as the device says */
	float period_us;		/* measured */
	bool period_measured;
	float period_refused;		/* the last measure, beyond 0.5..2x of the info's; 0 none */
	bool have_prev;
	uint16_t prev_seq;
	bool have_real;			/* the last sample with a time of its own */
	uint32_t real_t;
	uint16_t real_seq;
	uint64_t span_us;		/* measuring the period: time so far, */
	uint32_t span_n;		/* ...the intervals: their time, their samples */
	float span_el[SPAN_MAX];
	uint16_t span_cnt[SPAN_MAX];
	uint8_t frame_n;		/* samples in the last PKT_SAMPLES */
	uint32_t samples, bad_crc;
	uint8_t bad_info;		/* length of an info not understood (logged once) */
	bool streaming, imu_ok, fake;
};

void imu_parser_init(struct imu_parser *p);
/* The driver's: is a sensor live at now_ms whose last samples came at
 * last_ms (samples so far, its period)? LIVE_MS or 3 periods, the longer */
bool imu_live(uint64_t now_ms, uint64_t last_ms, uint32_t samples, float period_us);
/* The driver's watch: *live updated; true just when the samples stopped
 * (said once) */
bool imu_watch(bool *live, uint64_t now_ms, uint64_t last_ms, uint32_t samples, float period_us);
void imu_parser_feed(struct imu_parser *p, const uint8_t *data, size_t n);

/* What the driver logs of the rate: what it told so far, of which info */
enum { IMU_RATE_MEASURED = 1, IMU_RATE_REFUSED = 2 };
struct imu_rate_told {
	unsigned info_seq;
	int told;		/* IMU_RATE_*, 0 nothing yet */
};
/* IMU_RATE_* to log now (once each time it becomes so; anew for each
 * info), 0 nothing new */
int imu_rate_news(const struct imu_parser *p, struct imu_rate_told *t);
uint16_t imu_crc16(const uint8_t *data, size_t n);
