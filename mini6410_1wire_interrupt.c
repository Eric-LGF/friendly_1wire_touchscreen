#include <linux/init.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/irq.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/timer.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>

#define PWM_CHANNEL		2

#undef DEBUG
#define DEBUG
#ifdef DEBUG
#define DPRINTK(x...) {printk("%s(%d): ",__FUNCTION__ ,__LINE__);printk(x);}
#else
#define DPRINTK(x...) (void)(0)
#endif


/*
 * Clocksource driver
 */

#define REG_TCFG0			0x00
#define REG_TCFG1			0x04
#define REG_TCON			0x08
#define REG_TINT_CSTAT			0x44

#define REG_TCNTB(chan)			(0x0c + 12 * (chan))
#define REG_TCMPB(chan)			(0x10 + 12 * (chan))

#define TCFG0_PRESCALER_MASK		0xff
#define TCFG0_PRESCALER1_SHIFT		8

#define TCFG1_SHIFT(x)	  		((x) * 4)
#define TCFG1_MUX_MASK	  		0xf


/*
 * Each channel occupies 4 bits in TCON register, but there is a gap of 4
 * bits (one channel) after channel 0, so channels have different numbering
 * when accessing TCON register.
 *
 * In addition, the location of autoreload bit for channel 4 (TCON channel 5)
 * in its set of bits is 2 as opposed to 3 for other channels.
 */
#define TCON_START(chan)		(1 << (4 * (chan) + 0))
#define TCON_MANUALUPDATE(chan)		(1 << (4 * (chan) + 1))
#define TCON_INVERT(chan)		(1 << (4 * (chan) + 2))
#define _TCON_AUTORELOAD(chan)		(1 << (4 * (chan) + 3))
#define _TCON_AUTORELOAD4(chan)		(1 << (4 * (chan) + 2))
#define TCON_AUTORELOAD(chan)		\
	((chan < 5) ? _TCON_AUTORELOAD(chan) : _TCON_AUTORELOAD4(chan))


#define TOUCH_DEVICE_NAME	    "touchscreen-1wire"
#define BACKLIGHT_DEVICE_NAME	"backlight-1wire"
#define SAMPLE_BPS 9600
#define BIT_NANOSECONDS     (1000000000UL / SAMPLE_BPS)

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

static void __iomem *base = 0;
static int irq_num = 0;
static int one_wire_gpio = 0;
static unsigned lcd_type, firmware_ver;

static unsigned long TCNT_FOR_SAMPLE_BIT;

static int bl_ready;
static unsigned char backlight_req = 0;
static unsigned char backlight_init_success;



static inline void notify_ts_data(unsigned x, unsigned y, unsigned down)
{
	printk("x = %d, y = %d, down= %d\n", x, y, down);

	// if (!down && !(ts_status &(1U << 31))) {
	// 	// up repeat, give it up
	// 	return;
	// }

	// ts_status = ((x << 16) | (y)) | (down << 31);
	// ts_ready = 1;
	// wake_up_interruptible(&ts_waitq);
}


static inline void notify_bl_data(unsigned char a, unsigned char b, unsigned char c)
{
	bl_ready = 1;
	backlight_init_success = 1;
	//wake_up_interruptible(&bl_waitq);
}
static inline void notify_info_data(unsigned char _lcd_type, unsigned char ver_year, unsigned char week)
{
	if (_lcd_type != 0xFF) {
		lcd_type = _lcd_type;
		firmware_ver = ver_year * 100 + week;
	}
}

static void set_pin_as_input(void)
{
	gpio_direction_input(one_wire_gpio);
}

static void set_pin_as_output(void)
{
	gpio_direction_output(one_wire_gpio, 1);
}

static void set_pin_value(int v)
{
	gpio_direction_output(one_wire_gpio, v);
}

static int get_pin_value(void)
{
	return gpio_get_value(one_wire_gpio);
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


// once a session complete
static unsigned total_received, total_error;
static unsigned last_req, last_res;
static void one_wire_session_complete(unsigned char req, unsigned int res)
{
	unsigned char crc;
	const unsigned char *p = (const unsigned char*)&res;
	total_received ++;

	last_res = res;

	crc8_init(crc);
	crc8(crc, p[3]);
	crc8(crc, p[2]);
	crc8(crc, p[1]);
	if (crc != p[0]) {
		// CRC dismatch
		if (total_received > 100) {
			total_error++;
		}

        printk("%x %x: crc error. %d",req, res, total_error);
		return;
	}
	printk("req = %#x, res = %#x\n", req, res);
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

static inline void stop_timer_for_1wire(void)
{
	unsigned long tcon;
	unsigned int channel = PWM_CHANNEL;

	if (channel > 0)
		++channel;
    
	tcon = readl_relaxed(base + REG_TCON);
	tcon &= ~TCON_START(channel);
	writel_relaxed(tcon, base + REG_TCON);

	DPRINTK("1wire stop timer\n");
}

static volatile unsigned int io_bit_count;
static volatile unsigned int io_data;
static volatile unsigned char one_wire_request;
static irqreturn_t timer_for_1wire_interrupt(int irq, void *dev_id)
{
	u32 mask;
	u32 cstat;
	
	cstat = readl(base + REG_TINT_CSTAT);
	cstat &= 0x1f;
	mask = (1 << PWM_CHANNEL);
	writel(cstat | mask | (mask << 5), base + REG_TINT_CSTAT);

	io_bit_count--;
	switch(one_wire_status) {
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
			stop_timer_for_1wire();
		}
		break;
		
	default:
		stop_timer_for_1wire();
	}
	return IRQ_HANDLED;
}

static struct irqaction timer_for_1wire_irq = {
	.name    = "1-wire Timer Tick",
	.flags   = IRQF_TIMER | IRQF_IRQPOLL,
	.handler = timer_for_1wire_interrupt,
	.dev_id  = &timer_for_1wire_irq,
};


static void start_one_wire_session(unsigned char req)
{
	// u32 mask;
	// unsigned long cstat;
	unsigned long tcon;
	unsigned long flags;
	unsigned int tcon_channel = PWM_CHANNEL;

	if (tcon_channel > 0)
		tcon_channel++;

	if (one_wire_status != IDLE) {
		printk("one_wire_status: %d\n", one_wire_status);
		return;
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

	writel_relaxed(TCNT_FOR_SAMPLE_BIT, base + REG_TCNTB(PWM_CHANNEL));
	writel_relaxed(TCNT_FOR_SAMPLE_BIT, base + REG_TCMPB(PWM_CHANNEL));
	// init tranfer and start timer
	tcon = readl_relaxed(base + REG_TCON);
	tcon &= ~(0xF << (4*tcon_channel));
	tcon |= TCON_MANUALUPDATE(tcon_channel);
	writel_relaxed(tcon, base + REG_TCON);

	tcon |= TCON_START(tcon_channel);
	tcon |= TCON_AUTORELOAD(tcon_channel);
	tcon &= ~TCON_MANUALUPDATE(tcon_channel);

	local_irq_save(flags);
	writel(tcon, base + REG_TCON);
	set_pin_value(0);

	// cstat = readl(base + REG_TINT_CSTAT);
	// printk("cstat = %#X\n", (unsigned int)cstat);
	// cstat &= 0x1f;
	// mask = (1 << PWM_CHANNEL);
	// cstat = cstat | mask | (mask << 5);
	// printk("cstat = %#X\n", (unsigned int)cstat);
	// writel(cstat, base + REG_TINT_CSTAT);
	local_irq_restore(flags);
}

// poll the device
// following is Linux timer not HW timer
static int exitting;
//static struct timer_list one_wire_timer;
static struct hrtimer hr_timer;

//void one_wire_timer_proc(struct timer_list * v)
static enum hrtimer_restart one_wire_timer_proc(struct hrtimer *hrtimer)
{
	unsigned char req;
	if (exitting) {
		printk("hrtimer exitting.\n");
		return HRTIMER_NORESTART;
	}
	//printk("timer proc.\n");

	hrtimer_forward_now(&hr_timer, ktime_set(1, 0));
	// one_wire_timer.expires = jiffies + HZ;
	// add_timer(&one_wire_timer);
	if (lcd_type == 0) {
		req = REQ_INFO;
	} else if (!backlight_init_success) {
		req = 127;
	} else if (backlight_req) {
		req = backlight_req;
		backlight_req = 0;
	} else {
		req = REQ_TS;
	}
	start_one_wire_session(req);

	return HRTIMER_RESTART;
}


static int init_timer_for_1wire(void)
{
	u32 mask;
	u32 cstat;
	unsigned long tcfg1;
	unsigned long tcfg0;

	unsigned prescale1_value;

	unsigned long pclk;
	struct clk *clk;

	// get pclk
	clk = clk_get(NULL, "timers");
	if (IS_ERR(clk)) {
		DPRINTK("ERROR to get PCLK\n");
		return -EIO;
	}
	printk("get timers clock.\n");
	pclk = clk_get_rate(clk);
	printk("pclk = %ld\n", pclk);

	// get prescaler
	tcfg0 = __raw_readl(base + REG_TCFG0);
	// we use system prescaler value because timer 4 uses same one
	if (PWM_CHANNEL >= 2)
		prescale1_value = (tcfg0 >> 8) & 0xFF;
	else
		prescale1_value = (tcfg0 >> 0) & 0xFF;

	DPRINTK("tcfg0 = %#X;  prescale = %d\n", tcfg0, prescale1_value);

	// calc the TCNT_FOR_SAMPLE_BIT, that is one of the goal
	TCNT_FOR_SAMPLE_BIT = pclk / (prescale1_value + 1) / SAMPLE_BPS - 1;

	DPRINTK("SAMPLE_BIT = %#x", TCNT_FOR_SAMPLE_BIT);
	
    tcfg1 = __raw_readl(base + REG_TCFG1);
	tcfg1 &= ~(TCFG1_MUX_MASK << TCFG1_SHIFT(PWM_CHANNEL));
	writel(tcfg1, base + REG_TCFG1);

	cstat = readl(base + REG_TINT_CSTAT);
	printk("cstat = %#X\n", (unsigned int)cstat);
	cstat &= 0x1f;
	mask = (1 << PWM_CHANNEL);
	cstat = cstat | mask | (mask << 5);
	printk("cstat = %#X\n", (unsigned int)cstat);
	writel(cstat, base + REG_TINT_CSTAT);

	return 0;
}

static int one_wire_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device_node *gpio_node = pdev->dev.of_node;
	struct device_node *pwm_node = of_find_compatible_node(NULL, NULL, "samsung,s3c6400-pwm");

	if (!gpio_node) {
		printk("gpio node is null\n");
		return -1;
	}
	if (!pwm_node) {
		printk("pwm node is null\n");
		return -1;
	}

	printk("one wire probe\n");

	one_wire_gpio = of_get_named_gpio(gpio_node, "gpios", 0);
    if (!gpio_is_valid(one_wire_gpio)) {
		printk("gpio: %d is invalid\n", one_wire_gpio);

		return -ENODEV;
	}

	base = of_iomap(pwm_node, 0);
	irq_num = irq_of_parse_and_map(pwm_node, PWM_CHANNEL);
	printk("irq_num = %d\n", irq_num);

	// 请求控制获取的gpio
	if (gpio_request(one_wire_gpio, "one-wrie")) {
		printk("gpio %d request failed\n", one_wire_gpio);
		gpio_free(one_wire_gpio);
		
		return -ENODEV;
	}

	ret = init_timer_for_1wire();
	if (ret)
		goto __failed;
	// timer_setup(&one_wire_timer, one_wire_timer_proc, (unsigned long)"1wire timer");
	// one_wire_timer.expires = jiffies + HZ/10;

	setup_irq(irq_num, &timer_for_1wire_irq);
	printk("test ok.\n");

//	add_timer(&one_wire_timer);
//start_one_wire_session(127);
    hrtimer_init(&hr_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    hr_timer.function = one_wire_timer_proc;

    // start hrtimer
    hrtimer_start(&hr_timer, ktime_set(1,0), HRTIMER_MODE_REL);

	printk("probe func ok.\n");

	return 0;

__failed:
	gpio_free(one_wire_gpio);

	return -1;
}

static int one_wire_remove(struct platform_device *pdev)
{
	printk("one wire remove\n");
    
	//del_timer_sync(&one_wire_timer);
    hrtimer_cancel(&hr_timer);

	free_irq(irq_num, &timer_for_1wire_irq);
	gpio_free(one_wire_gpio);

	//remove_proc_entry("driver/one-wire-info", NULL);

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
