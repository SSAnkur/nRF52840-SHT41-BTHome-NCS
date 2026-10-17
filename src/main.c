/*
 * BTHome v2 temperature / humidity beacon - ultra-low-power (Zephyr)
 *
 * Every 60 s: power up SHT41, measure T/RH, power it down, read VDDH,
 * update a non-connectable BTHome v2 advertisement sent every 10 s.
 * Sensor power gating, I2C pin handling, DC/DC and sleep are all
 * configured in app.overlay / prj.conf.
 */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/sys/byteorder.h>

#define MEAS_INTERVAL K_SECONDS(60)
#define ADV_INTERVAL  16000 /* 10000 ms / 0.625 ms (legacy max is 10.24 s) */
#define VDDH_DIV      5     /* SAADC input is VDDH/5 */

static const struct device *const sht = DEVICE_DT_GET(DT_NODELABEL(sht41));
static const struct adc_dt_spec vbat = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

/* BTHome v2 service data (objects in ascending ID order: 0x0C, 0x2E, 0x45) */
static uint8_t svc[] = {
	0xD2, 0xFC, /* UUID 0xFCD2 */
	0x40,       /* BTHome v2, unencrypted, regular interval */
	0x0C, 0, 0, /* voltage, 1 mV */
	0x2E, 0,    /* humidity, 1 % */
	0x45, 0, 0, /* temperature, 0.1 degC */
};

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA(BT_DATA_SVC_DATA16, svc, sizeof(svc)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static int read_sht41(void)
{
	struct sensor_value t, rh;
	int err = pm_device_runtime_get(sht); /* sensor power on */

	if (err) {
		return err;
	}
	err = sensor_sample_fetch(sht);
	pm_device_runtime_put(sht);           /* bus high-Z, then sensor power off */
	if (err) {
		return err;
	}

	sensor_channel_get(sht, SENSOR_CHAN_AMBIENT_TEMP, &t);
	sensor_channel_get(sht, SENSOR_CHAN_HUMIDITY, &rh);

	int32_t mc = sensor_value_to_milli(&t);
	int16_t t10 = (mc + (mc < 0 ? -50 : 50)) / 100;
	int32_t pct = (sensor_value_to_milli(&rh) + 500) / 1000;

	sys_put_le16(t10, &svc[9]);
	svc[7] = CLAMP(pct, 0, 100);
	return 0;
}

static void read_battery(void)
{
	static bool calibrated;
	int16_t raw;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
		.calibrate = !calibrated,
	};

	adc_sequence_init_dt(&vbat, &seq);
	if (adc_read_dt(&vbat, &seq) == 0) {
		/* Full-scale input voltage: 0.6 V ref / gain 1/2 = 1200 mV */
		int32_t full_mv = adc_ref_internal(vbat.dev);

		adc_gain_invert(vbat.channel_cfg.gain, &full_mv);

		/*
		 * Scale raw straight to VDDH in one step and round to nearest:
		 * VDDH_mV = raw * 1200 * 5 / 2^res. Multiplying by 5 before the
		 * division keeps 1 mV resolution instead of 5 mV steps.
		 * Max 16383 * 1200 * 5 = 98 M, fits in 32 bits.
		 */
		uint32_t num = (uint32_t)MAX(raw, 0) * (uint32_t)full_mv * VDDH_DIV;
		uint32_t mv = (num + BIT(vbat.resolution - 1)) >> vbat.resolution;

		sys_put_le16(mv, &svc[4]);
		calibrated = true;
	}
}

static bool measure(void)
{
	if (read_sht41()) {
		return false;   /* keep advertising last good values */
	}
	read_battery();         /* right after sensor load */
	return true;
}

int main(void)
{
	adc_channel_setup_dt(&vbat);
	bt_enable(NULL);
	measure();              /* advertise even if the first read fails */

	bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_USE_IDENTITY, ADV_INTERVAL,
					ADV_INTERVAL, NULL),
			ad, ARRAY_SIZE(ad), NULL, 0);

	for (;;) {
		k_sleep(MEAS_INTERVAL); /* CPU in System ON idle */
		if (measure()) {
			bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
		}
	}
}
