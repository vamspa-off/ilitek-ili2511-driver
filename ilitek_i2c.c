#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/timer.h>

#include <asm/io.h>
#include <mach/w55fa92_gpio.h>

#define ILITEK_I2C_NAME             "ilitek_i2c"
#define ILITEK_GET_TOUCH_CMD        0x10
#define ILITEK_TOUCH_PACKET_SIZE    31

#define ILITEK_MAX_X                16384
#define ILITEK_MAX_Y                9600
#define POLL_INTERVAL_MS            15

// Screen size
#define SCREEN_SIZE_X				1024
#define SCREEN_SIZE_Y				600

// INT line
#define INT_GPIO_GROUP				GPIO_GROUP_G
#define INT_GPIO_PIN				3

// RST line
#define RST_GPIO_GROUP				GPIO_GROUP_G
#define RST_GPIO_PIN				4

// Driver params
// Swap X & Y
static bool swap_xy = false;
module_param(swap_xy, bool, 0644);
MODULE_PARM_DESC(swap_xy, "Swap X and Y axes");
// Invert X
static bool invert_x = false;
module_param(invert_x, bool, 0644);
MODULE_PARM_DESC(invert_x, "Invert X axis");
// Invert Y
static bool invert_y = false;
module_param(invert_y, bool, 0644);
MODULE_PARM_DESC(invert_y, "Invert Y axis");

struct ilitek_i2c {
    struct i2c_client *client;
    struct input_dev *input;
    struct timer_list poll_timer;
    bool was_touched;
	u16 last_x1, last_y1;
	u16 last_x2, last_y2;
};

static int ilitek_i2c_controller_write(struct ilitek_i2c *ts, u8 write_cmd)
{
    struct i2c_msg msg;
    int ret;

    msg.addr    = ts->client->addr;
    msg.flags   = 0;
    msg.len     = 1;
    msg.buf     = &write_cmd;

    ret = i2c_transfer(ts->client->adapter, &msg, 1);
    if (ret == 1) return 0;
    return ret < 0 ? ret : -EIO;
}

static int ilitek_i2c_controller_read(struct ilitek_i2c *ts, u8 *buf, u8 buf_size)
{
    struct i2c_msg msg;
    int ret;

    msg.addr    = ts->client->addr;
    msg.flags   = I2C_M_RD;
    msg.len     = buf_size;
    msg.buf     = buf;

    ret = i2c_transfer(ts->client->adapter, &msg, 1);
    if (ret == 1) return 0;
    return ret < 0 ? ret : -EIO;
}

static int ilitek_read_raw_touch_data(struct ilitek_i2c *ts, u8* touch_data, u8 td_size)
{
    int i2c_status = ilitek_i2c_controller_write(ts, ILITEK_GET_TOUCH_CMD);
    if (i2c_status == 0) {
        i2c_status = ilitek_i2c_controller_read(ts, touch_data, td_size);
    }
    return i2c_status;
}

static void ilitek_timer_callback(unsigned long data)
{
    struct ilitek_i2c *ts = (struct ilitek_i2c *)data;
    u8 packet[ILITEK_TOUCH_PACKET_SIZE];
    int ret;
    int int_level;
	u16 x1 = 0, y1 = 0;
	u16 x2 = 0, y2 = 0;
	bool touch1 = false, touch2 = false;

    int_level = w55fa92_gpio_get(INT_GPIO_GROUP, INT_GPIO_PIN);

    if (int_level == 0) {
        ret = ilitek_read_raw_touch_data(ts, packet, ILITEK_TOUCH_PACKET_SIZE);

        if (ret >= 0) {
			x1 = ((packet[1] << 8) | packet[2]) & 0x3FFF;
			y1 = ((packet[3] << 8) | packet[4]) & 0x3FFF;
			x2 = ((packet[6] << 8) | packet[7]) & 0x3FFF;
			y2 = ((packet[8] << 8) | packet[9]) & 0x3FFF;

			touch1 = (packet[1] & 0x80) != 0;
			touch2 = (packet[6] & 0x80) != 0;
		}
    }

	if (invert_x) {
		x1 = ILITEK_MAX_X - x1;
		x2 = ILITEK_MAX_X - x2;
	}
	if (invert_y) {
		y1 = ILITEK_MAX_X - y1;
		y2 = ILITEK_MAX_X - y2;
	}

	// Resize ILI2511 display size to physical display size
	x1 = x1 >> 4;
	y1 = y1 >> 4;
	x2 = x2 >> 4;
	y2 = y2 >> 4;

	if (touch1 || touch2) {
		if (touch1) {
			input_report_abs(ts->input, ABS_MT_POSITION_X, swap_xy ? y1 : x1);
			input_report_abs(ts->input, ABS_MT_POSITION_Y, swap_xy ? x1 : y1);
			input_mt_sync(ts->input);
		}
		if (touch2) {
			input_report_abs(ts->input, ABS_MT_POSITION_X, swap_xy ? y2 : x2);
			input_report_abs(ts->input, ABS_MT_POSITION_Y, swap_xy ? x2 : y2);
			input_mt_sync(ts->input);
		}

		if (touch1) {
			input_report_abs(ts->input, ABS_X, swap_xy ? y1 : x1);
			input_report_abs(ts->input, ABS_Y, swap_xy ? x1 : y1);
		} else if (touch2) {
			input_report_abs(ts->input, ABS_X, swap_xy ? y2 : x2);
			input_report_abs(ts->input, ABS_Y, swap_xy ? x2 : y2);
		}

		input_report_key(ts->input, BTN_TOUCH, 1);
		input_sync(ts->input);

		if (!ts->was_touched) {
			ts->was_touched = true;
		}

		ts->last_x1 = x1;
		ts->last_y1 = y1;
		ts->last_x2 = x2;
		ts->last_y2 = y2;
	} else if (ts->was_touched) {
		input_mt_sync(ts->input);
		input_report_key(ts->input, BTN_TOUCH, 0);
		input_sync(ts->input);

		ts->was_touched = false;
	}

    mod_timer(&ts->poll_timer, jiffies + msecs_to_jiffies(POLL_INTERVAL_MS));
}

static int ilitek_i2c_probe(struct i2c_client *client,
                const struct i2c_device_id *id)
{
    struct ilitek_i2c *ts;
    struct input_dev *input;
    int ret;
    int initial_level;
	u16 max_x, max_y;

	dev_info(&client->dev, "Reseting ILI2511...\n");
	w55fa92_gpio_configure(RST_GPIO_GROUP, RST_GPIO_PIN);
	w55fa92_gpio_set_output(RST_GPIO_GROUP, RST_GPIO_PIN);
	w55fa92_gpio_set(RST_GPIO_GROUP, RST_GPIO_PIN, 0);
	mdelay(20);
	w55fa92_gpio_set(RST_GPIO_GROUP, RST_GPIO_PIN, 1);
	mdelay(50);

    dev_info(&client->dev, "ILI2511 probe: addr=0x%02x\n", client->addr);

    if (!client->adapter)
        return -ENODEV;

    ts = kzalloc(sizeof(*ts), GFP_KERNEL);
    if (!ts)
        return -ENOMEM;

    ts->client = client;

    w55fa92_gpio_configure(INT_GPIO_GROUP, INT_GPIO_PIN);
    w55fa92_gpio_set_input(INT_GPIO_GROUP, INT_GPIO_PIN);
    initial_level = w55fa92_gpio_get(INT_GPIO_GROUP, INT_GPIO_PIN);
    dev_info(&client->dev, "INT line initial level: %d (0=Touch, 1=Free)\n", initial_level);

    input = input_allocate_device();
    if (!input) {
        kfree(ts);
        return -ENOMEM;
    }

    ts->input = input;
    input->name = "ILI2511 Touchscreen";
    input->phys = "i2c-ilitek/input0";
    input->id.bustype = BUS_I2C;
    input->dev.parent = &client->dev;

    set_bit(EV_KEY, input->evbit);
    set_bit(EV_ABS, input->evbit);
    set_bit(BTN_TOUCH, input->keybit);

	max_x = swap_xy ? SCREEN_SIZE_Y : SCREEN_SIZE_X;
	max_y = swap_xy ? SCREEN_SIZE_X : SCREEN_SIZE_Y;

    input_set_abs_params(input, ABS_X, 0, max_x, 0, 0);
    input_set_abs_params(input, ABS_Y, 0, max_y, 0, 0);

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, max_x, 0, 0);
    input_set_abs_params(input, ABS_MT_POSITION_Y, 0, max_y, 0, 0);

    i2c_set_clientdata(client, ts);

    ret = input_register_device(input);
    if (ret) {
        input_free_device(input);
        kfree(ts);
        return ret;
    }

    init_timer(&ts->poll_timer);
    ts->poll_timer.function = ilitek_timer_callback;
    ts->poll_timer.data = (unsigned long)ts;
    ts->poll_timer.expires = jiffies + msecs_to_jiffies(POLL_INTERVAL_MS);
    add_timer(&ts->poll_timer);

    dev_info(&client->dev, "ILI2511 driver loaded (GPG3 Multi-Touch Polling)\n");
    return 0;
}

static int ilitek_i2c_remove(struct i2c_client *client)
{
    struct ilitek_i2c *ts = i2c_get_clientdata(client);

    if (!ts)
        return 0;

    del_timer_sync(&ts->poll_timer);
    input_unregister_device(ts->input);
    i2c_set_clientdata(client, NULL);
    kfree(ts);

    return 0;
}

static const struct i2c_device_id ilitek_i2c_id[] = {
    { "ilitek_i2c", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, ilitek_i2c_id);

static struct i2c_driver ilitek_i2c_driver = {
    .driver = {
        .name = ILITEK_I2C_NAME,
    },
    .probe    = ilitek_i2c_probe,
    .remove   = ilitek_i2c_remove,
    .id_table = ilitek_i2c_id,
};

static int __init ilitek_i2c_init(void)
{
    return i2c_add_driver(&ilitek_i2c_driver);
}

static void __exit ilitek_i2c_exit(void)
{
    i2c_del_driver(&ilitek_i2c_driver);
}

module_init(ilitek_i2c_init);
module_exit(ilitek_i2c_exit);

MODULE_AUTHOR("vamspa-off");
MODULE_DESCRIPTION("ILI2511 I2C driver");
MODULE_LICENSE("GPL");
