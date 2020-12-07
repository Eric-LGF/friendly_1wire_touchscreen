#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/backlight.h>

#define TOUCH_DEVICE_NAME	    "touchscreen-1wire"
#define BACKLIGHT_DEVICE_NAME	"backlight-1wire"
#define SAMPLE_BPS 9600
#define BIT_NANOSECONDS     (1000000000UL / SAMPLE_BPS)
#define SAMPLE_PERIOD_NANO	(50 * 1000000)	// ms

#define REQ_TS   0x40U
#define REQ_INFO 0x60U

enum {
	IDLE,
	START,
	REQUEST,
	WAITING,
	RESPONSE,
	STOPING,
} one_wire_status = IDLE;

struct one_wire {
	struct device *dev;
	struct input_dev *input;
	struct platform_device *bl_dev;
	struct hrtimer hr_timer;
	int gpio;

	unsigned int lcd_type;
	unsigned int firmware_ver;
	
	unsigned long xp;
	unsigned long yp;

	unsigned char backlight_init_success;
	unsigned char backlight_req;
};

static struct one_wire dev_1wire;

static void onewire_bl_set_intensity(int intensity)
{
	if (intensity > 127 || intensity < 0)
		intensity = 127;
	
	dev_1wire.backlight_req = intensity;
}

static inline void notify_ts_data(unsigned x, unsigned y, unsigned down)
{
	if (down) {
		dev_1wire.xp = x;
		dev_1wire.yp = y;

		dev_dbg(dev_1wire.dev, "%s: X=%lu, Y=%lu\n",
			__func__, dev_1wire.xp, dev_1wire.yp);
		
		input_report_abs(dev_1wire.input, ABS_X, dev_1wire.xp);
		input_report_abs(dev_1wire.input, ABS_Y, dev_1wire.yp);

		input_report_key(dev_1wire.input, BTN_TOUCH, 1);
		input_sync(dev_1wire.input);
	} else {
		dev_1wire.xp = 0;
		dev_1wire.yp = 0;

		input_report_key(dev_1wire.input, BTN_TOUCH, 0);
		input_sync(dev_1wire.input);
	}
}


static inline void notify_bl_data(unsigned char a, unsigned char b, unsigned char c)
{
	dev_1wire.backlight_init_success = 1;
}

static inline void notify_info_data(unsigned char _lcd_type, unsigned char ver_year, unsigned char week)
{
	if (_lcd_type != 0xFF) {
		dev_1wire.lcd_type = _lcd_type;
		dev_1wire.firmware_ver = ver_year * 100 + week;
	}
}

static void set_pin_as_input(void)
{
	gpio_direction_input(dev_1wire.gpio);
}

static void set_pin_as_output(void)
{
}

static void set_pin_value(int v)
{
	gpio_direction_output(dev_1wire.gpio, v);
}

static int get_pin_value(void)
{
	return gpio_get_value(dev_1wire.gpio);
}


// CRC
//
static const unsigned char crc8_tab[] = {
0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15,
0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65,
0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5,
0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85,
0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2,
0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2,
0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32,
0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42,
0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C,
0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC,
0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C,
0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C,
0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B,
0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B,
0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB,
0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB,
0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3,
};

#define crc8_init(crc) ((crc) = 0XACU)
#define crc8(crc, v) ( (crc) = crc8_tab[(crc) ^(v)])

static volatile unsigned int io_bit_count;
static volatile unsigned int io_data;
static volatile unsigned char one_wire_request;


// once a session complete
static unsigned total_received, total_error;
static unsigned last_req, last_res;
static void one_wire_session_complete(unsigned char req, unsigned int res)
{
	static unsigned error_count = 0;
	unsigned char crc;
	const unsigned char *p = (const unsigned char*)&res;
	total_received ++;

	last_res = res;

	crc8_init(crc);
	crc8(crc, p[3]);
	crc8(crc, p[2]);
	crc8(crc, p[1]);
	if (crc != p[0]) {
		error_count++;
		// CRC dismatch
		if (error_count > 100) {
			total_received++;

			error_count = 0;
			printk("%x %x: crc error. %d",req, res, total_error);
		} 
		return;
	} else
		error_count = 0;

	switch(req) {
	case REQ_TS:
		{
			unsigned short x,y;
			unsigned pressed;
			x =  ((p[3] >>   4U) << 8U) + p[2];
			y =  ((p[3] &  0xFU) << 8U) + p[1];
			pressed = (x != 0xFFFU) && (y != 0xFFFU); 
			notify_ts_data(x, y, pressed);
		}
		break;
	
	case REQ_INFO:
		notify_info_data(p[3], p[2], p[1]);
		break;
	default:
		notify_bl_data(p[3], p[2], p[1]);
		break;
	}
}

static void one_wire_prepare(void)
{
    unsigned char req;

    if (dev_1wire.lcd_type == 0) {
        req = REQ_INFO;
    } else if (!dev_1wire.backlight_init_success) {
        req = 127;
    } else if (dev_1wire.backlight_req) {
        req = dev_1wire.backlight_req;
        dev_1wire.backlight_req = 0;
    } else {
        req = REQ_TS;
    }

    one_wire_status = START;

	set_pin_value(1);
	set_pin_as_output();
	// IDLE to START
	{
		unsigned char crc;
		crc8_init(crc);
		crc8(crc, req);
		io_data = (req << 8) + crc;
		io_data <<= 16;
	}
	last_req = (io_data >> 16);
	one_wire_request = req;
	io_bit_count = 1;
	set_pin_as_output();
	set_pin_value(0);
}

static enum hrtimer_restart timer_for_1wire_interrupt(struct hrtimer *hrtimer)
{
	io_bit_count--;
	switch(one_wire_status) {
    case IDLE:
        one_wire_prepare();
        break;
	case START:
		if (io_bit_count == 0) {
			io_bit_count = 16;
			one_wire_status = REQUEST;
		}
		break;

	case REQUEST:
		// Send a bit
		set_pin_value(io_data & (1U << 31));
		io_data <<= 1;
		if (io_bit_count == 0) {
			io_bit_count = 2;
			one_wire_status = WAITING;
		}
		break;
		
	case WAITING:
		if (io_bit_count == 0) {
			io_bit_count = 32;
			one_wire_status = RESPONSE;
		}
		if (io_bit_count == 1) {
			set_pin_value(1);
			set_pin_as_input();
		}
		break;
		
	case RESPONSE:
		// Get a bit
		io_data = (io_data << 1) | get_pin_value();
		if (io_bit_count == 0) {
			io_bit_count = 2;
			one_wire_status = STOPING;
			set_pin_value(1);
			set_pin_as_output();
			one_wire_session_complete(one_wire_request, io_data);
		}
		break;

	case STOPING:
		if (io_bit_count == 0) {
			one_wire_status = IDLE;
            hrtimer_forward_now(hrtimer, ktime_set(0, SAMPLE_PERIOD_NANO));
            return HRTIMER_RESTART;
		}
		break;
		
	default:
        break;
	}

	hrtimer_forward_now(hrtimer, ktime_set(0, BIT_NANOSECONDS));

	return HRTIMER_RESTART;
}


static struct generic_bl_info onewire_bl_info = {
	.name			= BACKLIGHT_DEVICE_NAME,
	.max_intensity	= 0x7f,
	.default_intensity = 0x7f,
	.set_bl_intensity	= onewire_bl_set_intensity,
};

static struct platform_device onewire_bl_dev = {
	.name	= "generic-bl",
	.id		= 1,
	.dev	= {
		.platform_data = &onewire_bl_info,
	},
};

static int one_wire_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct input_dev *input_dev;
	struct device_node *gpio_node = pdev->dev.of_node;
	int one_wire_gpio = 0;
	int ret = 0;

	memset(&dev_1wire, 0, sizeof(struct one_wire));

	dev_1wire.dev = dev;

	one_wire_gpio = of_get_named_gpio(gpio_node, "gpios", 0);
    if (!gpio_is_valid(one_wire_gpio)) {
		printk("gpio: %d is invalid\n", one_wire_gpio);

		ret = -ENODEV;
		goto err_exit;
	}

	// 请求控制获取的gpio
	if (gpio_request(one_wire_gpio, "one-wrie")) {
		printk("gpio %d request failed\n", one_wire_gpio);
		gpio_free(one_wire_gpio);
		
		ret = -ENODEV;
		goto err_exit;
	}

	dev_1wire.gpio = one_wire_gpio;

    hrtimer_init(&dev_1wire.hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    dev_1wire.hr_timer.function = timer_for_1wire_interrupt;

	input_dev = input_allocate_device();
	if (!input_dev) {
		dev_err(dev, "Unable to allocate the input device !!\n");
		ret = -ENOMEM;
		goto err_gpio;
	}
	dev_1wire.input = input_dev;
	dev_1wire.input->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	dev_1wire.input->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	input_set_abs_params(dev_1wire.input, ABS_X, 0, 0xFFF, 0, 0);
	input_set_abs_params(dev_1wire.input, ABS_Y, 0, 0xFFF, 0, 0);

	dev_1wire.input->name = TOUCH_DEVICE_NAME;
	dev_1wire.input->id.bustype = BUS_HOST;
	dev_1wire.input->id.vendor = 0xDEAD;
	dev_1wire.input->id.product = 0xBEEF;
	dev_1wire.input->id.version = 0x0102;

	ret = input_register_device(dev_1wire.input);
	if (ret < 0) {
		dev_err(dev, "failed to register input device\n");
		ret = -EIO;
		goto err_gpio;
	}

	dev_1wire.bl_dev = &onewire_bl_dev;
	ret = platform_device_register(dev_1wire.bl_dev);
	if (ret < 0) {
		dev_err(dev, "failed to register bl device\n");
		ret = -EIO;
		goto err_input;
	}

    // start hrtimer
    hrtimer_start(&dev_1wire.hr_timer, ktime_set(0,SAMPLE_PERIOD_NANO), HRTIMER_MODE_REL);

	return 0;

err_input:
	input_unregister_device(dev_1wire.input);
err_gpio:
	gpio_free(dev_1wire.gpio);
err_exit:
	return ret;
}

static int one_wire_remove(struct platform_device *pdev)
{
	printk("one wire remove\n");
    
    hrtimer_cancel(&dev_1wire.hr_timer);
	gpio_free(dev_1wire.gpio);
	input_unregister_device(dev_1wire.input);
	platform_device_unregister(dev_1wire.bl_dev);

	return 0;
}

static const struct of_device_id one_wire_dt_ids[] = {
	{ .compatible = "one-wire touchscreen", },
	{},
};


static struct platform_driver one_wire_driver = {
	.driver = {
		.name = "one-wire",
		.of_match_table = of_match_ptr(one_wire_dt_ids),
	},
	.probe = one_wire_probe,
	.remove = one_wire_remove
};

static int __init one_wire_init(void)
{
	printk("one wire init\n");
	return platform_driver_register(&one_wire_driver);
}

static void __exit one_wire_exit(void)
{
	printk("one wire exit\n");
	platform_driver_unregister(&one_wire_driver);
	// gpio_free(one_wire_gpio);
}
module_init(one_wire_init);
module_exit(one_wire_exit);
MODULE_LICENSE("GPL");