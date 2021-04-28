#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/fb.h>
#include <linux/mutex.h>

#include <linux/input.h> //input subsys
#include <linux/cdev.h>
#include <linux/major.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/input/mt.h>
#include <linux/device.h>
//#include "myscreensaver.h" //image

#include <asm/irq.h>
#include <asm/io.h>
#include <linux/interrupt.h>

#include <linux/uaccess.h> // copy_from/to_user
#include <asm/uaccess.h> // ^same
#include <linux/sched.h> // for timers
#include <linux/jiffies.h> // for jiffies global variable
#include <linux/string.h> // for string manipulation functions
#include <linux/ctype.h> // for isdigit
#include <linux/delay.h> //msleep


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Draw a rectangle");

static char* name = "/root/images/m0.fb";
module_param(name,charp,0660);

#define CYG_FB_DEFAULT_PALETTE_BLUE         0x01
#define CYG_FB_DEFAULT_PALETTE_RED          0x04
#define CYG_FB_DEFAULT_PALETTE_WHITE        0x0F
#define CYG_FB_DEFAULT_PALETTE_LIGHTBLUE    0x09
#define CYG_FB_DEFAULT_PALETTE_BLACK        0x00

// Timer related stuff
#define SCREENSAVER_TIMEOUT 15 // timeout in sec
static struct timer_list timerl0;
static struct timer_list timerl1;
static bool terminate_timer1;
static bool terminate_ack;

static void *buffer = NULL;
static loff_t max_size = 131109, size = 0; //131109, 261120,
static int ccode;
static int state;
struct color {
	unsigned char r;
	unsigned char g;
	unsigned char b;
};
static struct color clut[256]; // num of colors = (2^3)*(2^3)*(2^2) = 256
#define FRAME_NUMBER0 10
static char* names0[10] = { "/root/images/vid3/frame0.bin",
			"/root/images/vid3/frame1.bin",
                        "/root/images/vid3/frame2.bin",
                        "/root/images/vid3/frame3.bin",
                        "/root/images/vid3/frame4.bin",
                        "/root/images/vid3/frame5.bin",
                        "/root/images/vid3/frame6.bin",
                        "/root/images/vid3/frame7.bin",
                        "/root/images/vid3/frame8.bin",
                        "/root/images/vid3/frame9.bin",
};


#define FRAME_NUMBER1 16
static char* names1[16] = { "/root/images/vid6/frame0.bin",
                        "/root/images/vid6/frame1.bin",
                        "/root/images/vid6/frame2.bin",
                        "/root/images/vid6/frame3.bin",
                        "/root/images/vid6/frame4.bin",
                        "/root/images/vid6/frame5.bin",
                        "/root/images/vid6/frame6.bin",
                        "/root/images/vid6/frame7.bin",
                        "/root/images/vid6/frame8.bin",
                        "/root/images/vid6/frame9.bin",
                        "/root/images/vid6/frame10.bin",
                        "/root/images/vid6/frame11.bin",
                        "/root/images/vid6/frame12.bin",
                        "/root/images/vid6/frame13.bin",
                        "/root/images/vid6/frame14.bin",
                        "/root/images/vid6/frame15.bin",
};

#define FRAME_NUMBER2 12
static char* names2[12] = { "/root/images/vid7/frame0.bin",
                        "/root/images/vid7/frame1.bin",
                        "/root/images/vid7/frame2.bin",
                        "/root/images/vid7/frame3.bin",
                        "/root/images/vid7/frame4.bin",
                        "/root/images/vid7/frame5.bin",
                        "/root/images/vid7/frame6.bin",
                        "/root/images/vid7/frame7.bin",
                        "/root/images/vid7/frame8.bin",
                        "/root/images/vid7/frame9.bin",
                        "/root/images/vid7/frame10.bin",
                        "/root/images/vid7/frame11.bin",
};


#define LCD_IRQ 66 //36 // taken from device-tree

// GRAPHICS VARIABLES
struct fb_info *info;
struct fb_fillrect *blank;
struct fb_image *image;

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#define EVDEV_MINOR_BASE	64
#define EVDEV_MINORS		32
#define EVDEV_MIN_BUFFER_SIZE	64U
#define EVDEV_BUF_PACKETS	8
//struct input_event lcd_event;

enum evdev_clock_type {
	EV_CLK_REAL = 0,
	EV_CLK_MONO,
	EV_CLK_BOOT,
	EV_CLK_MAX
};

struct evdev {
	int open;
	struct input_handle handle;
	wait_queue_head_t wait;
	struct evdev_client __rcu *grab;
	struct list_head client_list;
	spinlock_t client_lock; /* protects client_list */
	struct mutex mutex;
	struct device dev;
	struct cdev cdev;
	bool exist;
};


struct evdev_client {
	unsigned int head;
	unsigned int tail;
	unsigned int packet_head; /* [future] position of the first element of next packet */
	spinlock_t buffer_lock; /* protects access to buffer, head and tail */
	struct fasync_struct *fasync;
	struct evdev *evdev;
	struct list_head node;
	unsigned int clk_type;
	bool revoked;
	unsigned long *evmasks[EV_CNT];
	unsigned int bufsize;
	struct input_event buffer[];
};

static struct evdev_client *client;

static inline size_t input_event_size(void)
{
	return sizeof(struct input_event);
}

int input_event_from_user(const char __user *buffer,
			 struct input_event *event)
{
	if (copy_from_user(event, buffer, sizeof(struct input_event)))
		return -EFAULT;

	return 0;
}

int input_event_to_user(char __user *buffer,
			const struct input_event *event)
{
	if (copy_to_user(buffer, event, sizeof(struct input_event)))
		return -EFAULT;

	return 0;
}


int input_ff_effect_from_user(const char __user *buffer, size_t size,
			      struct ff_effect *effect)
{
	if (size != sizeof(struct ff_effect))
		return -EINVAL;

	if (copy_from_user(effect, buffer, sizeof(struct ff_effect)))
		return -EFAULT;

	return 0;
}

//chath


static size_t evdev_get_mask_cnt(unsigned int type)
{
	static const size_t counts[EV_CNT] = {
		/* EV_SYN==0 is EV_CNT, _not_ SYN_CNT, see EVIOCGBIT */
		[EV_SYN]	= EV_CNT,
		[EV_KEY]	= KEY_CNT,
		[EV_REL]	= REL_CNT,
		[EV_ABS]	= ABS_CNT,
		[EV_MSC]	= MSC_CNT,
		[EV_SW]		= SW_CNT,
		[EV_LED]	= LED_CNT,
		[EV_SND]	= SND_CNT,
		[EV_FF]		= FF_CNT,
	};

	return (type < EV_CNT) ? counts[type] : 0;
}

/* requires the buffer lock to be held */
static bool __evdev_is_filtered(unsigned int type,
				unsigned int code)
{
	unsigned long *mask;
	size_t cnt;

	/* EV_SYN and unknown codes are never filtered */
	if (type == EV_SYN || type >= EV_CNT)
		return false;

	/* first test whether the type is filtered */
	mask = client->evmasks[0];
	if (mask && !test_bit(type, mask))
		return true;

	/* unknown values are never filtered */
	cnt = evdev_get_mask_cnt(type);
	if (!cnt || code >= cnt)
		return false;

	mask = client->evmasks[type];
	return mask && !test_bit(code, mask);
}

/* flush queued events of type @type, caller must hold client->buffer_lock */
static void __evdev_flush_queue(unsigned int type)
{
	unsigned int i, head, num;
	unsigned int mask = client->bufsize - 1;
	bool is_report;
	struct input_event *ev;

	BUG_ON(type == EV_SYN);

	head = client->tail;
	client->packet_head = client->tail;

	/* init to 1 so a leading SYN_REPORT will not be dropped */
	num = 1;

	for (i = client->tail; i != client->head; i = (i + 1) & mask) {
		ev = &client->buffer[i];
		is_report = ev->type == EV_SYN && ev->code == SYN_REPORT;

		if (ev->type == type) {
			/* drop matched entry */
			continue;
		} else if (is_report && !num) {
			/* drop empty SYN_REPORT groups */
			continue;
		} else if (head != i) {
			/* move entry to fill the gap */
			client->buffer[head] = *ev;
		}

		num++;
		head = (head + 1) & mask;

		if (is_report) {
			num = 0;
			client->packet_head = head;
		}
	}

	client->head = head;
}

static void __evdev_queue_syn_dropped(void)
{
	struct input_event ev;
	ktime_t time;
	struct timespec64 ts;

	time = client->clk_type == EV_CLK_REAL ?
			ktime_get_real() :
			client->clk_type == EV_CLK_MONO ?
				ktime_get() :
				ktime_get_boottime();

	ts = ktime_to_timespec64(time);
	ev.input_event_sec = ts.tv_sec;
	ev.input_event_usec = ts.tv_nsec / NSEC_PER_USEC;
	ev.type = EV_SYN;
	ev.code = SYN_DROPPED;
	ev.value = 0;

	client->buffer[client->head++] = ev;
	client->head &= client->bufsize - 1;

	if (unlikely(client->head == client->tail)) {
		/* drop queue but keep our SYN_DROPPED event */
		client->tail = (client->head - 1) & (client->bufsize - 1);
		client->packet_head = client->tail;
	}
}

static void evdev_queue_syn_dropped(void)
{
	unsigned long flags;

	spin_lock_irqsave(&client->buffer_lock, flags);
	__evdev_queue_syn_dropped(); //chath
	spin_unlock_irqrestore(&client->buffer_lock, flags);
}

static int evdev_set_clk_type(unsigned int clkid)
{
	unsigned long flags;
	unsigned int clk_type;

	switch (clkid) {

	case CLOCK_REALTIME:
		clk_type = EV_CLK_REAL;
		break;
	case CLOCK_MONOTONIC:
		clk_type = EV_CLK_MONO;
		break;
	case CLOCK_BOOTTIME:
		clk_type = EV_CLK_BOOT;
		break;
	default:
		return -EINVAL;
	}

	if (client->clk_type != clk_type) {
		client->clk_type = clk_type;

		/*
		 * Flush pending events and queue SYN_DROPPED event,
		 * but only if the queue is not empty.
		 */
		spin_lock_irqsave(&client->buffer_lock, flags);

		if (client->head != client->tail) {
			client->packet_head = client->head = client->tail;
			__evdev_queue_syn_dropped();
		}

		spin_unlock_irqrestore(&client->buffer_lock, flags);
	}

	return 0;
}

static void __pass_event(const struct input_event *event)
{
	printk("PASS_EVENT\n");
	client->buffer[client->head++] = *event;
	client->head &= client->bufsize - 1;

	if (unlikely(client->head == client->tail)) {
		/*
		 * This effectively "drops" all unconsumed events, leaving
		 * EV_SYN/SYN_DROPPED plus the newest event in the queue.
		 */
		client->tail = (client->head - 2) & (client->bufsize - 1);

		client->buffer[client->tail].input_event_sec =
						event->input_event_sec;
		client->buffer[client->tail].input_event_usec =
						event->input_event_usec;
		client->buffer[client->tail].type = EV_SYN;
		client->buffer[client->tail].code = SYN_DROPPED;
		client->buffer[client->tail].value = 0;

		client->packet_head = client->tail;
	}

	if (event->type == EV_SYN && event->code == SYN_REPORT) {
		client->packet_head = client->head;
		kill_fasync(&client->fasync, SIGIO, POLL_IN);
	}
}

static void evdev_pass_values(const struct input_value *vals, unsigned int count,
			ktime_t *ev_time)
{
	printk("PASS_VALUES\n");
	struct evdev *evdev = client->evdev;
	const struct input_value *v;
	struct input_event event;
	struct timespec64 ts;
	bool wakeup = false;

	if (client->revoked)
		return;

	ts = ktime_to_timespec64(ev_time[client->clk_type]);
	event.input_event_sec = ts.tv_sec;
	event.input_event_usec = ts.tv_nsec / NSEC_PER_USEC;

	/* Interrupts are disabled, just acquire the lock. */
	spin_lock(&client->buffer_lock);

	for (v = vals; v != vals + count; v++) {
		if (__evdev_is_filtered( v->type, v->code)) //chath
			continue;

		if (v->type == EV_SYN && v->code == SYN_REPORT) {
			/* drop empty SYN_REPORT */
			if (client->packet_head == client->head)
				continue;

			wakeup = true;
		}

		event.type = v->type;
		event.code = v->code;
		event.value = v->value;
		__pass_event(&event); //chath
	}

	spin_unlock(&client->buffer_lock);

	if (wakeup)
		wake_up_interruptible(&evdev->wait);
}

/*
 * Pass incoming events to all connected clients.
 */
static void evdev_events(struct input_handle *handle,
			 const struct input_value *vals, unsigned int count)
{
	printk("EVENTS_INVOKED\n");
        printk("Touch detected\n");
        terminate_timer1 = true;
        del_timer_sync(&timerl1);
        mod_timer(&timerl1, jiffies + msecs_to_jiffies(15000));
	terminate_timer1 = false;
        printk("Timer updated\n");

	struct evdev *evdev = handle->private;
	// struct evdev_client *client;
	ktime_t ev_time[EV_CLK_MAX];

	ev_time[EV_CLK_MONO] = ktime_get();
	ev_time[EV_CLK_REAL] = ktime_mono_to_real(ev_time[EV_CLK_MONO]);
	ev_time[EV_CLK_BOOT] = ktime_mono_to_any(ev_time[EV_CLK_MONO],
						 TK_OFFS_BOOT);

	rcu_read_lock();

	// client = rcu_dereference(evdev->grab);

	if (client)
		evdev_pass_values(vals, count, ev_time); //chath
	else
		list_for_each_entry_rcu(client, &evdev->client_list, node) //chath
			evdev_pass_values(vals, count, ev_time);		//chath

	rcu_read_unlock();
}

/*
 * Pass incoming event to all connected clients.
 */
static void evdev_event(struct input_handle *handle,
			unsigned int type, unsigned int code, int value)
{
	printk("EVENT_INVOKED");
	struct input_value vals[] = { { type, code, value } };

	evdev_events(handle, vals, 1);
}

static int evdev_fasync(int fd, struct file *file, int on)
{
	// struct evdev_client *client = file->private_data;

	return fasync_helper(fd, file, on, &client->fasync);
}

static int evdev_flush(struct file *file, fl_owner_t id)
{
	// struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;

	mutex_lock(&evdev->mutex);

	if (evdev->exist && !client->revoked)
		input_flush_device(&evdev->handle, file);

	mutex_unlock(&evdev->mutex);
	return 0;
}

static void evdev_free(struct device *dev)
{
	struct evdev *evdev = container_of(dev, struct evdev, dev);

	input_put_device(evdev->handle.dev);
	kfree(evdev);
}

/*
 * Grabs an event device (along with underlying input device).
 * This function is called with evdev->mutex taken.
 */
static int evdev_grab(struct evdev *evdev) //chath
{
	int error;

	if (evdev->grab)
		return -EBUSY;

	error = input_grab_device(&evdev->handle);
	if (error)
		return error;

	rcu_assign_pointer(evdev->grab, client);

	return 0;
}

static int evdev_ungrab(struct evdev *evdev)
{
	struct evdev_client *grab = rcu_dereference_protected(evdev->grab,
					lockdep_is_held(&evdev->mutex));

	if (grab != client)
		return  -EINVAL;

	rcu_assign_pointer(evdev->grab, NULL);
	synchronize_rcu();
	input_release_device(&evdev->handle);

	return 0;
}

static void evdev_attach_client(struct evdev *evdev)
{
	spin_lock(&evdev->client_lock);
	list_add_tail_rcu(&client->node, &evdev->client_list);
	spin_unlock(&evdev->client_lock);
}

static void evdev_detach_client(struct evdev *evdev)
{
	spin_lock(&evdev->client_lock);
	list_del_rcu(&client->node);
	spin_unlock(&evdev->client_lock);
	synchronize_rcu();
}

static int evdev_open_device(struct evdev *evdev)
{
	int retval;

	retval = mutex_lock_interruptible(&evdev->mutex);
	if (retval)
		return retval;

	if (!evdev->exist)
		retval = -ENODEV;
	else if (!evdev->open++) {
		retval = input_open_device(&evdev->handle);
		if (retval)
			evdev->open--;
	}

	mutex_unlock(&evdev->mutex);
	return retval;
}

static void evdev_close_device(struct evdev *evdev)
{
	mutex_lock(&evdev->mutex);

	if (evdev->exist && !--evdev->open)
		input_close_device(&evdev->handle);

	mutex_unlock(&evdev->mutex);
}

/*
 * Wake up users waiting for IO so they can disconnect from
 * dead device.
 */
static void evdev_hangup(struct evdev *evdev)
{
	// struct evdev_client *client;

	spin_lock(&evdev->client_lock);
	list_for_each_entry(client, &evdev->client_list, node)
		kill_fasync(&client->fasync, SIGIO, POLL_HUP);
	spin_unlock(&evdev->client_lock);

	wake_up_interruptible(&evdev->wait);
}

static int evdev_release(struct inode *inode, struct file *file)
{
	// struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;
	unsigned int i;

	mutex_lock(&evdev->mutex);
	evdev_ungrab(evdev);
	mutex_unlock(&evdev->mutex);

	evdev_detach_client(evdev);

	for (i = 0; i < EV_CNT; ++i)
		bitmap_free(client->evmasks[i]);

	// kvfree(client);

	evdev_close_device(evdev);

	return 0;
}

static unsigned int evdev_compute_buffer_size(struct input_dev *dev)
{
	unsigned int n_events =
		max(dev->hint_events_per_packet * EVDEV_BUF_PACKETS,
		    EVDEV_MIN_BUFFER_SIZE);

	return roundup_pow_of_two(n_events);
}

static int evdev_open(struct inode *inode, struct file *file)
{
	struct evdev *evdev = container_of(inode->i_cdev, struct evdev, cdev);
	unsigned int bufsize = evdev_compute_buffer_size(evdev->handle.dev);
	unsigned int size = sizeof(struct evdev_client) +
					bufsize * sizeof(struct input_event);
	// struct evdev_client *client;
	int error;

	// client = kzalloc(size, GFP_KERNEL | __GFP_NOWARN);
	// if (!client)
	// 	client = vzalloc(size);
	// if (!client)
	// 	return -ENOMEM;

	// client->bufsize = bufsize;
	// spin_lock_init(&client->buffer_lock);
	// client->evdev = evdev;
	// evdev_attach_client(evdev);

	// error = evdev_open_device(evdev);
	// if (error)
	// 	goto err_free_client;

	file->private_data = client;
	nonseekable_open(inode, file);

	return 0;

 err_free_client:
	evdev_detach_client(evdev);
	kvfree(client);
	return error;
}

static ssize_t evdev_write(struct file *file, const char __user *buffer,
			   size_t count, loff_t *ppos)
{
	printk("WRITE_INVOKED\n");
	// struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;
	struct input_event event;
	int retval = 0;
	//printk("TYPE: %d\nCODE: %d\nVALUE:%d\n",event.type,event.code,event.value);

	if (count != 0 && count < input_event_size())
		return -EINVAL;

	retval = mutex_lock_interruptible(&evdev->mutex);
	if (retval)
		return retval;

	if (!evdev->exist || client->revoked) {
		retval = -ENODEV;
		goto out;
	}

	while (retval + input_event_size() <= count) {

		if (input_event_from_user(buffer + retval, &event)) {
			retval = -EFAULT;
			goto out;
		}
		retval += input_event_size();
		printk("TYPE: %d\nCODE: %d\nVALUE:%d\n",event.type,event.code,event.value);
		input_inject_event(&evdev->handle,
				   event.type, event.code, event.value);
		cond_resched();
	}

 out:
	mutex_unlock(&evdev->mutex);
	return retval;
}

static int evdev_fetch_next_event(struct input_event *event)
{
	int have_event;

	spin_lock_irq(&client->buffer_lock);

	have_event = client->packet_head != client->tail;
	if (have_event) {
		*event = client->buffer[client->tail++];
		client->tail &= client->bufsize - 1;
	}

	spin_unlock_irq(&client->buffer_lock);

	return have_event;
}

static ssize_t evdev_read(struct file *file, char __user *buffer,
			  size_t count, loff_t *ppos)
{
	printk("READ_INVOKED\n");
	// struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;
	struct input_event event;
	size_t read = 0;
	int error;

	if (count != 0 && count < input_event_size())
		return -EINVAL;

	for (;;) {
		if (!evdev->exist || client->revoked)
			return -ENODEV;

		if (client->packet_head == client->tail &&
		    (file->f_flags & O_NONBLOCK))
			return -EAGAIN;

		/*
		 * count == 0 is special - no IO is done but we check
		 * for error conditions (see above).
		 */
		if (count == 0)
			break;

		while (read + input_event_size() <= count &&
		       evdev_fetch_next_event(&event)) {
			printk("TYPE: %d\nCODE: %d\nVALUE:%d\n",event.type,event.code,event.value);			
			if (input_event_to_user(buffer + read, &event))
				return -EFAULT;

			read += input_event_size();
		}

		if (read)
			break;

		if (!(file->f_flags & O_NONBLOCK)) {
			error = wait_event_interruptible(evdev->wait,
					client->packet_head != client->tail ||
					!evdev->exist || client->revoked);
			if (error)
				return error;
		}
	}

	return read;
}

void timer1_callback(struct timer_list *timer)
{
        //printk("IN CALLBACK1\n");
	terminate_ack = false;
	int i;
	i = -1;
    	while (!terminate_timer1){
		i++;
		if (state==0){
                        if (i>=FRAME_NUMBER0){
                                i = 0;
			}
			ccode = kernel_read_file_from_path(names0[i], &buffer, &size, max_size, READING_KEXEC_IMAGE);
		}
                else if (state==1){
                        if (i>=FRAME_NUMBER1){
                                i = 0;
                        }
                        ccode = kernel_read_file_from_path(names1[i], &buffer, &size, max_size, READING_KEXEC_IMAGE);
                }
                //else {
                //        if (i>=FRAME_NUMBER2){
                //                i = 0;
                //        }
                //        ccode = kernel_read_file_from_path(names2[i], &buffer, &size, max_size, READING_KEXEC_IMAGE);
                //}
		image->data = buffer; 
    		lock_fb_info(info);
    		sys_imageblit(info, image);
    		unlock_fb_info(info);
        	msleep(100);
             	//blank->color = i;  //CYG_FB_DEFAULT_PALETTE_BLUE;
                //lock_fb_info(info);
                //sys_fillrect(info, blank);
                //unlock_fb_info(info);
		//i++;
		//if (i==FRAME_NUMBER){
		//	i = 0;
		//}
    	}
	blank->color = CYG_FB_DEFAULT_PALETTE_WHITE;
	lock_fb_info(info);
        sys_fillrect(info, blank);
        unlock_fb_info(info);
	state++;
	if (state>=2){
		state = 0;
	}
	//terminate_ack = true;
	//printk("EXIT SCSV\n");
}

/*
void timer0_callback(struct timer_list *timer)
{
	//printk("IN CALLBACK0\n");
        //if (!client)
        //        printk(KERN_ERR "CLIENT not allocated\n");
	struct evdev *evdev = client->evdev;
	struct input_event event;
	size_t read = 0;
	int error;
	bool touch_input;
	//printk("IN CALLBACK0\n");
	if (!client)
		printk(KERN_ERR "CLIENT not allocated\n");
        if (!evdev)
                printk(KERN_ERR "EVDEV not allocated\n");
	// if (count != 0 && count < input_event_size())
	// 	return -EINVAL;

	//for (;;) {
		touch_input = false;
		terminate_timer1 = false;
		if (!evdev->exist || client->revoked)
			error = -ENODEV;

		if (client->packet_head == client->tail)
			error = -EAGAIN;
		if (!error)
			printk(KERN_ERR "ERROR:Init failed\n");
		//  * count == 0 is special - no IO is done but we check
		//  * for error conditions (see above).
		 
		// if (count == 0)
		// 	break;

		while (evdev_fetch_next_event(&event)) {
			printk("TYPE: %d\nCODE: %d\nVALUE:%d\n",event.type,event.code,event.value);
			touch_input = true;
			//if (input_event_to_user(buffer + read, &event))
			//	return -EFAULT;

			//read += input_event_size();
		}
		if(touch_input){
			printk("Touch detected\n");
			terminate_timer1 = true;
			del_timer_sync(&timerl1);
			//while(!terminate_ack); // wait for termination ack
			printk("ACK received\n");
			mod_timer(&timerl1, jiffies + msecs_to_jiffies(15000));
			printk("Timer updated\n");
		}
		//msleep(100);
		mod_timer(&timerl0, jiffies + msecs_to_jiffies(100));
	//}
}
*/
/* No kernel lock - fine */
static __poll_t evdev_poll(struct file *file, poll_table *wait)
{
	// struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;
	__poll_t mask;

	poll_wait(file, &evdev->wait, wait);

	if (evdev->exist && !client->revoked)
		mask = EPOLLOUT | EPOLLWRNORM;
	else
		mask = EPOLLHUP | EPOLLERR;

	if (client->packet_head != client->tail)
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

#ifdef CONFIG_COMPAT

#define BITS_PER_LONG_COMPAT (sizeof(compat_long_t) * 8)
#define BITS_TO_LONGS_COMPAT(x) ((((x) - 1) / BITS_PER_LONG_COMPAT) + 1)

#ifdef __BIG_ENDIAN
static int bits_to_user(unsigned long *bits, unsigned int maxbit,
			unsigned int maxlen, void __user *p, int compat)
{
	int len, i;

	if (compat) {
		len = BITS_TO_LONGS_COMPAT(maxbit) * sizeof(compat_long_t);
		if (len > maxlen)
			len = maxlen;

		for (i = 0; i < len / sizeof(compat_long_t); i++)
			if (copy_to_user((compat_long_t __user *) p + i,
					 (compat_long_t *) bits +
						i + 1 - ((i % 2) << 1),
					 sizeof(compat_long_t)))
				return -EFAULT;
	} else {
		len = BITS_TO_LONGS(maxbit) * sizeof(long);
		if (len > maxlen)
			len = maxlen;

		if (copy_to_user(p, bits, len))
			return -EFAULT;
	}

	return len;
}

static int bits_from_user(unsigned long *bits, unsigned int maxbit,
			  unsigned int maxlen, const void __user *p, int compat)
{
	int len, i;

	if (compat) {
		if (maxlen % sizeof(compat_long_t))
			return -EINVAL;

		len = BITS_TO_LONGS_COMPAT(maxbit) * sizeof(compat_long_t);
		if (len > maxlen)
			len = maxlen;

		for (i = 0; i < len / sizeof(compat_long_t); i++)
			if (copy_from_user((compat_long_t *) bits +
						i + 1 - ((i % 2) << 1),
					   (compat_long_t __user *) p + i,
					   sizeof(compat_long_t)))
				return -EFAULT;
		if (i % 2)
			*((compat_long_t *) bits + i - 1) = 0;

	} else {
		if (maxlen % sizeof(long))
			return -EINVAL;

		len = BITS_TO_LONGS(maxbit) * sizeof(long);
		if (len > maxlen)
			len = maxlen;

		if (copy_from_user(bits, p, len))
			return -EFAULT;
	}

	return len;
}

#else

static int bits_to_user(unsigned long *bits, unsigned int maxbit,
			unsigned int maxlen, void __user *p, int compat)
{
	int len = compat ?
			BITS_TO_LONGS_COMPAT(maxbit) * sizeof(compat_long_t) :
			BITS_TO_LONGS(maxbit) * sizeof(long);

	if (len > maxlen)
		len = maxlen;

	return copy_to_user(p, bits, len) ? -EFAULT : len;
}

static int bits_from_user(unsigned long *bits, unsigned int maxbit,
			  unsigned int maxlen, const void __user *p, int compat)
{
	size_t chunk_size = compat ? sizeof(compat_long_t) : sizeof(long);
	int len;

	if (maxlen % chunk_size)
		return -EINVAL;

	len = compat ? BITS_TO_LONGS_COMPAT(maxbit) : BITS_TO_LONGS(maxbit);
	len *= chunk_size;
	if (len > maxlen)
		len = maxlen;

	return copy_from_user(bits, p, len) ? -EFAULT : len;
}

#endif /* __BIG_ENDIAN */

#else

static int bits_to_user(unsigned long *bits, unsigned int maxbit,
			unsigned int maxlen, void __user *p, int compat)
{
	int len = BITS_TO_LONGS(maxbit) * sizeof(long);

	if (len > maxlen)
		len = maxlen;

	return copy_to_user(p, bits, len) ? -EFAULT : len;
}

static int bits_from_user(unsigned long *bits, unsigned int maxbit,
			  unsigned int maxlen, const void __user *p, int compat)
{
	int len;

	if (maxlen % sizeof(long))
		return -EINVAL;

	len = BITS_TO_LONGS(maxbit) * sizeof(long);
	if (len > maxlen)
		len = maxlen;

	return copy_from_user(bits, p, len) ? -EFAULT : len;
}

#endif /* CONFIG_COMPAT */

static int str_to_user(const char *str, unsigned int maxlen, void __user *p)
{
	int len;

	if (!str)
		return -ENOENT;

	len = strlen(str) + 1;
	if (len > maxlen)
		len = maxlen;

	return copy_to_user(p, str, len) ? -EFAULT : len;
}

static int handle_eviocgbit(struct input_dev *dev,
			    unsigned int type, unsigned int size,
			    void __user *p, int compat_mode)
{
	unsigned long *bits;
	int len;

	switch (type) {

	case      0: bits = dev->evbit;  len = EV_MAX;  break;
	case EV_KEY: bits = dev->keybit; len = KEY_MAX; break;
	case EV_REL: bits = dev->relbit; len = REL_MAX; break;
	case EV_ABS: bits = dev->absbit; len = ABS_MAX; break;
	case EV_MSC: bits = dev->mscbit; len = MSC_MAX; break;
	case EV_LED: bits = dev->ledbit; len = LED_MAX; break;
	case EV_SND: bits = dev->sndbit; len = SND_MAX; break;
	case EV_FF:  bits = dev->ffbit;  len = FF_MAX;  break;
	case EV_SW:  bits = dev->swbit;  len = SW_MAX;  break;
	default: return -EINVAL;
	}

	return bits_to_user(bits, len, size, p, compat_mode);
}

static int evdev_handle_get_keycode(struct input_dev *dev, void __user *p)
{
	struct input_keymap_entry ke = {
		.len	= sizeof(unsigned int),
		.flags	= 0,
	};
	int __user *ip = (int __user *)p;
	int error;

	/* legacy case */
	if (copy_from_user(ke.scancode, p, sizeof(unsigned int)))
		return -EFAULT;

	error = input_get_keycode(dev, &ke);
	if (error)
		return error;

	if (put_user(ke.keycode, ip + 1))
		return -EFAULT;

	return 0;
}

static int evdev_handle_get_keycode_v2(struct input_dev *dev, void __user *p)
{
	struct input_keymap_entry ke;
	int error;

	if (copy_from_user(&ke, p, sizeof(ke)))
		return -EFAULT;

	error = input_get_keycode(dev, &ke);
	if (error)
		return error;

	if (copy_to_user(p, &ke, sizeof(ke)))
		return -EFAULT;

	return 0;
}

static int evdev_handle_set_keycode(struct input_dev *dev, void __user *p)
{
	struct input_keymap_entry ke = {
		.len	= sizeof(unsigned int),
		.flags	= 0,
	};
	int __user *ip = (int __user *)p;

	if (copy_from_user(ke.scancode, p, sizeof(unsigned int)))
		return -EFAULT;

	if (get_user(ke.keycode, ip + 1))
		return -EFAULT;

	return input_set_keycode(dev, &ke);
}

static int evdev_handle_set_keycode_v2(struct input_dev *dev, void __user *p)
{
	struct input_keymap_entry ke;

	if (copy_from_user(&ke, p, sizeof(ke)))
		return -EFAULT;

	if (ke.len > sizeof(ke.scancode))
		return -EINVAL;

	return input_set_keycode(dev, &ke);
}

/*
 * If we transfer state to the user, we should flush all pending events
 * of the same type from the client's queue. Otherwise, they might end up
 * with duplicate events, which can screw up client's state tracking.
 * If bits_to_user fails after flushing the queue, we queue a SYN_DROPPED
 * event so user-space will notice missing events.
 *
 * LOCKING:
 * We need to take event_lock before buffer_lock to avoid dead-locks. But we
 * need the even_lock only to guarantee consistent state. We can safely release
 * it while flushing the queue. This allows input-core to handle filters while
 * we flush the queue.
 */
static int evdev_handle_get_val(struct input_dev *dev, unsigned int type,
				unsigned long *bits, unsigned int maxbit,
				unsigned int maxlen, void __user *p,
				int compat)
{
	int ret;
	unsigned long *mem;

	mem = bitmap_alloc(maxbit, GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	spin_lock_irq(&dev->event_lock);
	spin_lock(&client->buffer_lock);

	bitmap_copy(mem, bits, maxbit);

	spin_unlock(&dev->event_lock);

	__evdev_flush_queue(type);

	spin_unlock_irq(&client->buffer_lock);

	ret = bits_to_user(mem, maxbit, maxlen, p, compat);
	if (ret < 0)
		evdev_queue_syn_dropped();

	bitmap_free(mem);

	return ret;
}

static int evdev_handle_mt_request(struct input_dev *dev,
				   unsigned int size,
				   int __user *ip)
{
	const struct input_mt *mt = dev->mt;
	unsigned int code;
	int max_slots;
	int i;

	if (get_user(code, &ip[0]))
		return -EFAULT;
	if (!mt || !input_is_mt_value(code))
		return -EINVAL;

	max_slots = (size - sizeof(__u32)) / sizeof(__s32);
	for (i = 0; i < mt->num_slots && i < max_slots; i++) {
		int value = input_mt_get_value(&mt->slots[i], code);
		if (put_user(value, &ip[1 + i]))
			return -EFAULT;
	}

	return 0;
}

static int evdev_revoke(struct evdev *evdev, 
			struct file *file)
{
	client->revoked = true;
	evdev_ungrab(evdev);
	input_flush_device(&evdev->handle, file);
	wake_up_interruptible(&evdev->wait);

	return 0;
}

/* must be called with evdev-mutex held */
static int evdev_set_mask(unsigned int type,
			  const void __user *codes,
			  u32 codes_size,
			  int compat)
{
	unsigned long flags, *mask, *oldmask;
	size_t cnt;
	int error;

	/* we allow unknown types and 'codes_size > size' for forward-compat */
	cnt = evdev_get_mask_cnt(type);
	if (!cnt)
		return 0;

	mask = bitmap_zalloc(cnt, GFP_KERNEL);
	if (!mask)
		return -ENOMEM;

	error = bits_from_user(mask, cnt - 1, codes_size, codes, compat);
	if (error < 0) {
		bitmap_free(mask);
		return error;
	}

	spin_lock_irqsave(&client->buffer_lock, flags);
	oldmask = client->evmasks[type];
	client->evmasks[type] = mask;
	spin_unlock_irqrestore(&client->buffer_lock, flags);

	bitmap_free(oldmask);

	return 0;
}

/* must be called with evdev-mutex held */
static int evdev_get_mask(unsigned int type,
			  void __user *codes,
			  u32 codes_size,
			  int compat)
{
	unsigned long *mask;
	size_t cnt, size, xfer_size;
	int i;
	int error;

	/* we allow unknown types and 'codes_size > size' for forward-compat */
	cnt = evdev_get_mask_cnt(type);
	size = sizeof(unsigned long) * BITS_TO_LONGS(cnt);
	xfer_size = min_t(size_t, codes_size, size);

	if (cnt > 0) {
		mask = client->evmasks[type];
		if (mask) {
			error = bits_to_user(mask, cnt - 1,
					     xfer_size, codes, compat);
			if (error < 0)
				return error;
		} else {
			/* fake mask with all bits set */
			for (i = 0; i < xfer_size; i++)
				if (put_user(0xffU, (u8 __user *)codes + i))
					return -EFAULT;
		}
	}

	if (xfer_size < codes_size)
		if (clear_user(codes + xfer_size, codes_size - xfer_size))
			return -EFAULT;

	return 0;
}

static long evdev_do_ioctl(struct file *file, unsigned int cmd,
			   void __user *p, int compat_mode)
{
	// struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;
	struct input_dev *dev = evdev->handle.dev;
	struct input_absinfo abs;
	struct input_mask mask;
	struct ff_effect effect;
	int __user *ip = (int __user *)p;
	unsigned int i, t, u, v;
	unsigned int size;
	int error;

	/* First we check for fixed-length commands */
	switch (cmd) {

	case EVIOCGVERSION:
		return put_user(EV_VERSION, ip);

	case EVIOCGID:
		if (copy_to_user(p, &dev->id, sizeof(struct input_id)))
			return -EFAULT;
		return 0;

	case EVIOCGREP:
		if (!test_bit(EV_REP, dev->evbit))
			return -ENOSYS;
		if (put_user(dev->rep[REP_DELAY], ip))
			return -EFAULT;
		if (put_user(dev->rep[REP_PERIOD], ip + 1))
			return -EFAULT;
		return 0;

	case EVIOCSREP:
		if (!test_bit(EV_REP, dev->evbit))
			return -ENOSYS;
		if (get_user(u, ip))
			return -EFAULT;
		if (get_user(v, ip + 1))
			return -EFAULT;

		input_inject_event(&evdev->handle, EV_REP, REP_DELAY, u);
		input_inject_event(&evdev->handle, EV_REP, REP_PERIOD, v);

		return 0;

	case EVIOCRMFF:
		return input_ff_erase(dev, (int)(unsigned long) p, file);

	case EVIOCGEFFECTS:
		i = test_bit(EV_FF, dev->evbit) ?
				dev->ff->max_effects : 0;
		if (put_user(i, ip))
			return -EFAULT;
		return 0;

	case EVIOCGRAB:
		if (p)
			return evdev_grab(evdev);
		else
			return evdev_ungrab(evdev);

	case EVIOCREVOKE:
		if (p)
			return -EINVAL;
		else
			return evdev_revoke(evdev, file);

	case EVIOCGMASK: {
		void __user *codes_ptr;

		if (copy_from_user(&mask, p, sizeof(mask)))
			return -EFAULT;

		codes_ptr = (void __user *)(unsigned long)mask.codes_ptr;
		return evdev_get_mask(
				      mask.type, codes_ptr, mask.codes_size,
				      compat_mode);
	}

	case EVIOCSMASK: {
		const void __user *codes_ptr;

		if (copy_from_user(&mask, p, sizeof(mask)))
			return -EFAULT;

		codes_ptr = (const void __user *)(unsigned long)mask.codes_ptr;
		return evdev_set_mask(
				      mask.type, codes_ptr, mask.codes_size,
				      compat_mode);
	}

	case EVIOCSCLOCKID:
		if (copy_from_user(&i, p, sizeof(unsigned int)))
			return -EFAULT;

		return evdev_set_clk_type( i);

	case EVIOCGKEYCODE:
		return evdev_handle_get_keycode(dev, p);

	case EVIOCSKEYCODE:
		return evdev_handle_set_keycode(dev, p);

	case EVIOCGKEYCODE_V2:
		return evdev_handle_get_keycode_v2(dev, p);

	case EVIOCSKEYCODE_V2:
		return evdev_handle_set_keycode_v2(dev, p);
	}

	size = _IOC_SIZE(cmd);

	/* Now check variable-length commands */
#define EVIOC_MASK_SIZE(nr)	((nr) & ~(_IOC_SIZEMASK << _IOC_SIZESHIFT))
	switch (EVIOC_MASK_SIZE(cmd)) {

	case EVIOCGPROP(0):
		return bits_to_user(dev->propbit, INPUT_PROP_MAX,
				    size, p, compat_mode);

	case EVIOCGMTSLOTS(0):
		return evdev_handle_mt_request(dev, size, ip);

	case EVIOCGKEY(0):
		return evdev_handle_get_val(dev, EV_KEY, dev->key,
					    KEY_MAX, size, p, compat_mode);

	case EVIOCGLED(0):
		return evdev_handle_get_val(dev, EV_LED, dev->led,
					    LED_MAX, size, p, compat_mode);

	case EVIOCGSND(0):
		return evdev_handle_get_val(dev, EV_SND, dev->snd,
					    SND_MAX, size, p, compat_mode);

	case EVIOCGSW(0):
		return evdev_handle_get_val(dev, EV_SW, dev->sw,
					    SW_MAX, size, p, compat_mode);

	case EVIOCGNAME(0):
		return str_to_user(dev->name, size, p);

	case EVIOCGPHYS(0):
		return str_to_user(dev->phys, size, p);

	case EVIOCGUNIQ(0):
		return str_to_user(dev->uniq, size, p);

	case EVIOC_MASK_SIZE(EVIOCSFF):
		if (input_ff_effect_from_user(p, size, &effect))
			return -EFAULT;

		error = input_ff_upload(dev, &effect, file);
		if (error)
			return error;

		if (put_user(effect.id, &(((struct ff_effect __user *)p)->id)))
			return -EFAULT;

		return 0;
	}

	/* Multi-number variable-length handlers */
	if (_IOC_TYPE(cmd) != 'E')
		return -EINVAL;

	if (_IOC_DIR(cmd) == _IOC_READ) {

		if ((_IOC_NR(cmd) & ~EV_MAX) == _IOC_NR(EVIOCGBIT(0, 0)))
			return handle_eviocgbit(dev,
						_IOC_NR(cmd) & EV_MAX, size,
						p, compat_mode);

		if ((_IOC_NR(cmd) & ~ABS_MAX) == _IOC_NR(EVIOCGABS(0))) {

			if (!dev->absinfo)
				return -EINVAL;

			t = _IOC_NR(cmd) & ABS_MAX;
			abs = dev->absinfo[t];

			if (copy_to_user(p, &abs, min_t(size_t,
					size, sizeof(struct input_absinfo))))
				return -EFAULT;

			return 0;
		}
	}

	if (_IOC_DIR(cmd) == _IOC_WRITE) {

		if ((_IOC_NR(cmd) & ~ABS_MAX) == _IOC_NR(EVIOCSABS(0))) {

			if (!dev->absinfo)
				return -EINVAL;

			t = _IOC_NR(cmd) & ABS_MAX;

			if (copy_from_user(&abs, p, min_t(size_t,
					size, sizeof(struct input_absinfo))))
				return -EFAULT;

			if (size < sizeof(struct input_absinfo))
				abs.resolution = 0;

			/* We can't change number of reserved MT slots */
			if (t == ABS_MT_SLOT)
				return -EINVAL;

			/*
			 * Take event lock to ensure that we are not
			 * changing device parameters in the middle
			 * of event.
			 */
			spin_lock_irq(&dev->event_lock);
			dev->absinfo[t] = abs;
			spin_unlock_irq(&dev->event_lock);

			return 0;
		}
	}

	return -EINVAL;
}

static long evdev_ioctl_handler(struct file *file, unsigned int cmd,
				void __user *p, int compat_mode)
{
	// struct evdev_client *client = file->private_data;
	struct evdev *evdev = client->evdev;
	int retval;

	retval = mutex_lock_interruptible(&evdev->mutex);
	if (retval)
		return retval;

	if (!evdev->exist || client->revoked) {
		retval = -ENODEV;
		goto out;
	}

	retval = evdev_do_ioctl(file, cmd, p, compat_mode);

 out:
	mutex_unlock(&evdev->mutex);
	return retval;
}

static long evdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return evdev_ioctl_handler(file, cmd, (void __user *)arg, 0);
}

#ifdef CONFIG_COMPAT
static long evdev_ioctl_compat(struct file *file,
				unsigned int cmd, unsigned long arg)
{
	return evdev_ioctl_handler(file, cmd, compat_ptr(arg), 1);
}
#endif

static const struct file_operations evdev_fops = {
	.owner		= THIS_MODULE,
	.read		= evdev_read,
	.write		= evdev_write,
	.poll		= evdev_poll,
	.open		= evdev_open,
	.release	= evdev_release,
	.unlocked_ioctl	= evdev_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= evdev_ioctl_compat,
#endif
	.fasync		= evdev_fasync,
	.flush		= evdev_flush,
	.llseek		= no_llseek,
};

/*
 * Mark device non-existent. This disables writes, ioctls and
 * prevents new users from opening the device. Already posted
 * blocking reads will stay, however new ones will fail.
 */
static void evdev_mark_dead(struct evdev *evdev)
{
	mutex_lock(&evdev->mutex);
	evdev->exist = false;
	mutex_unlock(&evdev->mutex);
}

static void evdev_cleanup(struct evdev *evdev)
{
	struct input_handle *handle = &evdev->handle;

	evdev_mark_dead(evdev);
	evdev_hangup(evdev);

	/* evdev is marked dead so no one else accesses evdev->open */
	if (evdev->open) {
		input_flush_device(handle, NULL);
		input_close_device(handle);
	}
}

/*
 * Create new evdev device. Note that input core serializes calls
 * to connect and disconnect.
 */
static int evdev_connect(struct input_handler *handler, struct input_dev *dev,
			 const struct input_device_id *id)
{
	struct evdev *evdev;
	int minor;
	int dev_no;
	int error;

	minor = input_get_new_minor(EVDEV_MINOR_BASE, EVDEV_MINORS, true);
	if (minor < 0) {
		error = minor;
		pr_err("failed to reserve new minor: %d\n", error);
		return error;
	}

	evdev = kzalloc(sizeof(struct evdev), GFP_KERNEL);
	if (!evdev) {
		error = -ENOMEM;
		goto err_free_minor;
	}

	INIT_LIST_HEAD(&evdev->client_list);
	spin_lock_init(&evdev->client_lock);
	mutex_init(&evdev->mutex);
	init_waitqueue_head(&evdev->wait);
	evdev->exist = true;

	dev_no = minor;
	/* Normalize device number if it falls into legacy range */
	if (dev_no < EVDEV_MINOR_BASE + EVDEV_MINORS)
		dev_no -= EVDEV_MINOR_BASE;
	dev_set_name(&evdev->dev, "event%d", dev_no);

	evdev->handle.dev = input_get_device(dev);
	evdev->handle.name = dev_name(&evdev->dev);
	evdev->handle.handler = handler;
	evdev->handle.private = evdev;

	evdev->dev.devt = MKDEV(INPUT_MAJOR, minor);
	evdev->dev.class = &input_class;
	evdev->dev.parent = &dev->dev;
	evdev->dev.release = evdev_free;
	device_initialize(&evdev->dev);

	error = input_register_handle(&evdev->handle);
	if (error)
		goto err_free_evdev;

	cdev_init(&evdev->cdev, &evdev_fops);

	error = cdev_device_add(&evdev->cdev, &evdev->dev);
	if (error)
		goto err_cleanup_evdev;

	//return 0;

	// struct evdev *evdev = container_of(inode->i_cdev, struct evdev, cdev);
	unsigned int bufsize = evdev_compute_buffer_size(evdev->handle.dev);
	unsigned int size = sizeof(struct evdev_client) +
					bufsize * sizeof(struct input_event);
	// struct evdev_client *client;
	// int error;

	client = kzalloc(size, GFP_KERNEL | __GFP_NOWARN);
	if (!client)
		client = vzalloc(size);
	if (!client)
		return -ENOMEM;

	client->bufsize = bufsize;
	spin_lock_init(&client->buffer_lock);
	client->evdev = evdev;
	evdev_attach_client(evdev);

	error = evdev_open_device(evdev);
	if (error)
		goto err_free_client;

	return 0;

 err_cleanup_evdev:
	evdev_cleanup(evdev);
	input_unregister_handle(&evdev->handle);
 err_free_evdev:
	put_device(&evdev->dev);
 err_free_minor:
	input_free_minor(minor);
	return error;
 err_free_client:
	evdev_detach_client(evdev);
	kvfree(client);
	return error;
}

static void evdev_disconnect(struct input_handle *handle)
{
	struct evdev *evdev = handle->private;

	cdev_device_del(&evdev->cdev, &evdev->dev);
	evdev_cleanup(evdev);
	input_free_minor(MINOR(evdev->dev.devt));
	input_unregister_handle(handle);
	put_device(&evdev->dev);
}

//chath

//static void lcd_event(struct input_handle *handle, unsigned int type, unsigned int code, int value){
//	printk("TYPE: %u\n",type);
//} 

//const struct input_device_id* id_table;

static const struct input_device_id evdev_ids[] = {
	{ .driver_info = 1 },	/* Matches all devices */
	{ },			/* Terminating zero entry */
};

static struct input_handler lcd_handler = {
  .event      = evdev_event,      /* Handle event reports sent by input device drivers that use
                                     this event driver's services */
  .events      = evdev_events,
  //.fops       = &mydev_fops,      /* Methods to manage
  //                                   /dev/input/mydev */
  .legacy_minors	= true,
  .minor      = EVDEV_MINOR_BASE, /* Minor number of
                                     /dev/input/mydev */
  .name       = "lcd",          /* Event driver name */
  .id_table   = evdev_ids,        /* This event driver can handle
                                     requests from these IDs */
  .connect    = evdev_connect,    /* Invoked if there is an
                                     ID match */
  .disconnect = evdev_disconnect, /* Called when the driver unregisters
                                   */
};

/*
static struct input_dev *lcd_dev;

static irqreturn_t lcd_interrupt(int irq, void *dummy)
{
        //input_report_key(lcd_dev, BTN_0, inb(BUTTON_PORT) & 1);
        input_report_abs(lcd_dev, ABS_X, 1);
        input_sync(lcd_dev);
	printk("IRQ HANDLED\n");
        return IRQ_HANDLED;
}
*/

/* Helper function borrowed from drivers/video/fbdev/core/fbmem.c */
static struct fb_info *get_fb_info(unsigned int idx)
{
    struct fb_info *fb_info;

    if (idx >= FB_MAX)
        return ERR_PTR(-ENODEV);

    fb_info = registered_fb[idx];
    if (fb_info)
        atomic_inc(&fb_info->count);

    return fb_info;
}

static int __init hellofb_init(void)
{
    int i, j, error, x, y, z;
    printk(KERN_INFO "Hello framebuffer!\n");
    //generate color map
    for(x=0; x<8; x++){
        for(y=0; y<8; y++){
            for(z=0; z<4; z++){
		i = (x*8*4)+(y*4)+z;
                clut[i].r = x;
		clut[i].g = y;
		clut[i].b = z;
            }
        }
    }

    blank = kmalloc(sizeof(struct fb_fillrect), GFP_KERNEL);
    image = kmalloc(sizeof(struct fb_image), GFP_KERNEL);
    blank->dx = 0;
    blank->dy = 0;
    blank->width = 482; //482
    blank->height = 272; //272
    blank->color = CYG_FB_DEFAULT_PALETTE_WHITE;
    blank->rop = ROP_COPY;
    info = get_fb_info(0);
    lock_fb_info(info);
    sys_fillrect(info, blank);
    unlock_fb_info(info);
    //ccode = kernel_read_file_from_path(name, &buffer, &size, max_size, READING_KEXEC_IMAGE);
    printk("Image read %d\n",ccode);
    struct fb_cmap palette_cmap;
    u8 palette_green[16];
    u8 palette_blue[16];
    u8 palette_red[16];
    palette_cmap.start = 0;
    palette_cmap.len = 8;
    palette_cmap.red = palette_red;
    palette_cmap.green = palette_green;
    palette_cmap.blue = palette_blue;
    palette_cmap.transp = NULL;
    //Uncomment the following block to set color map
    /*for(i=0; i<256; i+=16){
        palette_cmap.start = i;
        palette_cmap.len = 16;
        for (j = 0; j < 16; j++) {
            palette_cmap.red[j] =  clut[i+j].r;
            palette_cmap.green[j] = clut[i+j].g;
            palette_cmap.blue[j] = clut[i+j].b;
        }
	fb_set_cmap(&palette_cmap, info);
    }*/
    //fb_set_cmap(&info->cmap, info);
    image->cmap = info->cmap;    
    image->dx = 0;
    image->dy = 0;
    image->width = 482; //482
    image->height = 272; //272
    image->depth = 8;
/*
    image->data = buffer; //mando;
    //image->cmap = palette_cmap; //info->cmap;
    lock_fb_info(info);
    sys_imageblit(info, image);
    unlock_fb_info(info);
 //for (i=0; i<255; i++){
        msleep(20000);
	//name[14] = "1";
	//ccode = kernel_read_file_from_path(name, &buffer, &size, max_size, READING_KEXEC_IMAGE);
	image->data = mando;
	image->cmap = info->cmap;
    lock_fb_info(info);
    sys_imageblit(info, image);
    unlock_fb_info(info);
	msleep(20000);

        blank->color = CYG_FB_DEFAULT_PALETTE_BLUE;
        lock_fb_info(info);
        sys_fillrect(info, blank);
        unlock_fb_info(info);
*/
    //}
/*
    	if (request_irq(LCD_IRQ, lcd_interrupt, 0, "tilcdc", NULL)) {
                printk(KERN_ERR "ERROR: Can't allocate irq %d\n", LCD_IRQ);
                return -EBUSY;
        }

        lcd_dev = input_allocate_device();
        if (!lcd_dev) {
                printk(KERN_ERR "ERROR: Not enough memory\n");
                error = -ENOMEM;
                //goto err_free_irq;
        }

        lcd_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
        lcd_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
	input_set_abs_params(lcd_dev, ABS_X, 0, 482, 0, 0);
	input_set_abs_params(lcd_dev, ABS_Y, 0, 272, 0, 0);

        error = input_register_device(lcd_dev);
        if (error) {
                printk(KERN_ERR "button.c: Failed to register device\n");
                //goto err_free_dev;
        }
*/
	

	error = input_register_handler(&lcd_handler);
	state = 0;
        timer_setup(&timerl1, timer1_callback, 0);
        mod_timer(&timerl1, jiffies + msecs_to_jiffies(SCREENSAVER_TIMEOUT*1000+1000)); //added 1000 for now


	return error;
}

static void __exit hellofb_exit(void) {
	terminate_timer1 = true;
	del_timer_sync(&timerl0);
	del_timer_sync(&timerl1);
    // Cleanup framebuffer graphics
    kfree(blank);
	input_unregister_handler(&lcd_handler);
    printk(KERN_INFO "Goodbye framebuffer!\n");
    printk(KERN_INFO "Module exiting\n");
}

module_init(hellofb_init);
module_exit(hellofb_exit);
