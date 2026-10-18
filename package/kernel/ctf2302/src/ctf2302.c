// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal driver for the Sensylink CTF2302 fan controller.
 *
 * No datasheet is available. The register addresses and the values written
 * at probe come from the stock UCG-Fiber firmware, which programs these six
 * registers with 16 bit SMBus word writes in this order and then leaves the
 * fan duty cycle in the low byte of register 0x40 (0..255). The temperature
 * (0x00-0x04) and tachometer (0x05, 0x06) registers are not exposed until
 * their formats are known.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>

#define CTF2302_REG_CONV_RATE		0x0d
#define CTF2302_REG_CH_DIS		0x0e
#define CTF2302_REG_CONFIG1		0x10
#define CTF2302_REG_CONFIG2		0x11
#define CTF2302_REG_FAN_SPIN_TIME	0x17
#define CTF2302_REG_FAN_DCY_CTRL	0x40

struct ctf2302_data {
	struct i2c_client *client;
	struct mutex lock;
};

struct ctf2302_init_reg {
	u8 reg;
	const char *prop;
	u16 def;
};

/*
 * Order, values and device tree property names as the stock firmware uses
 * them; each value can be overridden from the device tree.
 */
static const struct ctf2302_init_reg ctf2302_init[] = {
	{ CTF2302_REG_CH_DIS,		"ch-dis",		0x2000 },
	{ CTF2302_REG_CONV_RATE,	"conv-rate",		0x5000 },
	{ CTF2302_REG_CONFIG2,		"config2",		0x5511 },
	{ CTF2302_REG_FAN_DCY_CTRL,	"fan-dcy-ctrl",		0x006e },
	{ CTF2302_REG_FAN_SPIN_TIME,	"fan-spin-time",	0x4488 },
	{ CTF2302_REG_CONFIG1,		"config1",		0x0404 },
};

static umode_t ctf2302_is_visible(const void *data,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	if (type == hwmon_pwm && attr == hwmon_pwm_input)
		return 0644;

	return 0;
}

static int ctf2302_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct ctf2302_data *data = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_pwm || attr != hwmon_pwm_input)
		return -EOPNOTSUPP;

	ret = i2c_smbus_read_word_data(data->client, CTF2302_REG_FAN_DCY_CTRL);
	if (ret < 0)
		return ret;

	*val = ret & 0xff;
	return 0;
}

static int ctf2302_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	struct ctf2302_data *data = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_pwm || attr != hwmon_pwm_input)
		return -EOPNOTSUPP;

	val = clamp_val(val, 0, 255);

	mutex_lock(&data->lock);
	ret = i2c_smbus_write_word_data(data->client,
					CTF2302_REG_FAN_DCY_CTRL, val);
	mutex_unlock(&data->lock);

	return ret;
}

static const struct hwmon_ops ctf2302_hwmon_ops = {
	.is_visible = ctf2302_is_visible,
	.read = ctf2302_read,
	.write = ctf2302_write,
};

static const struct hwmon_channel_info * const ctf2302_info[] = {
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_chip_info ctf2302_chip_info = {
	.ops = &ctf2302_hwmon_ops,
	.info = ctf2302_info,
};

static int ctf2302_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ctf2302_data *data;
	struct device *hwmon_dev;
	unsigned int i;
	u32 val;
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_WORD_DATA))
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->lock);

	/* The stock firmware waits 50 ms before the first access. */
	msleep(50);

	for (i = 0; i < ARRAY_SIZE(ctf2302_init); i++) {
		const struct ctf2302_init_reg *r = &ctf2302_init[i];

		val = r->def;
		of_property_read_u32(dev->of_node, r->prop, &val);

		ret = i2c_smbus_write_word_data(client, r->reg, val);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "write 0x%04x to reg 0x%02x failed\n",
					     val, r->reg);
	}

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name,
							 data,
							 &ctf2302_chip_info,
							 NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id ctf2302_id[] = {
	{ "ctf2302" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ctf2302_id);

static const struct of_device_id ctf2302_of_match[] = {
	{ .compatible = "sensylink,ctf2302" },
	{ }
};
MODULE_DEVICE_TABLE(of, ctf2302_of_match);

static struct i2c_driver ctf2302_driver = {
	.driver = {
		.name = "ctf2302",
		.of_match_table = ctf2302_of_match,
	},
	.probe = ctf2302_probe,
	.id_table = ctf2302_id,
};
module_i2c_driver(ctf2302_driver);

MODULE_DESCRIPTION("Sensylink CTF2302 fan controller driver");
MODULE_LICENSE("GPL");
