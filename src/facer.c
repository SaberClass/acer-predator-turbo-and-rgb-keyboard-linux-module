// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Acer WMI Laptop Extras
 *
 *  Copyright (C) 2007-2009	Carlos Corbacho <carlos@strangeworlds.co.uk>
 *
 *  Based on acer_acpi:
 *    Copyright (C) 2005-2007	E.M. Smith
 *    Copyright (C) 2007-2008	Carlos Corbacho <cathectic@gmail.com>
 *
 *  Added support for Acer Predator hotkeys:
 *    Copyright (C) 2021        Bernhard Rosenkraenzer <bero@lindev.ch>
 */

/*
 * RTLNX_VER_MIN
 * Evaluates to true if the linux kernel version is equal or higher to the
 * one specfied.
 */
 #define RTLNX_VER_MIN(a_Major, a_Minor, a_Patch) \
 (LINUX_VERSION_CODE >= KERNEL_VERSION(a_Major, a_Minor, a_Patch))

/*
* RTLNX_VER_MAX
* Evaluates to true if the linux kernel version is less to the one specfied
* (exclusive). */
#define RTLNX_VER_MAX(a_Major, a_Minor, a_Patch) \
 (LINUX_VERSION_CODE < KERNEL_VERSION(a_Major, a_Minor, a_Patch))

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/dmi.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/acpi.h>
#include <linux/i8042.h>
#include <linux/rfkill.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/cdev.h>
#include <linux/input/sparse-keymap.h>
#include <acpi/video.h>
#include <linux/hwmon.h>
#include <linux/bitfield.h>
#include <linux/version.h>

#if RTLNX_VER_MIN(6, 14, 0)
#include <linux/unaligned.h>
#endif

MODULE_AUTHOR("Carlos Corbacho");
MODULE_DESCRIPTION("Acer Laptop WMI Extras Driver");
MODULE_LICENSE("GPL");

/*
 * Magic Number
 * Meaning is unknown - this number is required for writing to ACPI for AMW0
 * (it's also used in acerhk when directly accessing the BIOS)
 */
#define ACER_AMW0_WRITE	0x9610

/*
 * Bit masks for the AMW0 interface
 */
#define ACER_AMW0_WIRELESS_MASK  0x35
#define ACER_AMW0_BLUETOOTH_MASK 0x34
#define ACER_AMW0_MAILLED_MASK   0x31

/*
 * Method IDs for WMID interface
 */
#define ACER_WMID_GET_WIRELESS_METHODID		1
#define ACER_WMID_GET_BLUETOOTH_METHODID	2
#define ACER_WMID_GET_BRIGHTNESS_METHODID	3
#define ACER_WMID_SET_WIRELESS_METHODID		4
#define ACER_WMID_SET_BLUETOOTH_METHODID	5
#define ACER_WMID_SET_BRIGHTNESS_METHODID	6
#define ACER_WMID_GET_THREEG_METHODID		10
#define ACER_WMID_SET_THREEG_METHODID		11
#define ACER_WMID_SET_GAMINGKBBL_METHODID		20
#define ACER_WMID_GET_GAMINGKBBL_METHODID		21


#define ACER_WMID_SET_GAMING_LED_METHODID 2
#define ACER_WMID_GET_GAMING_LED_METHODID 4
#define ACER_WMID_GET_GAMING_SYS_INFO_METHODID 5
#define ACER_WMID_SET_GAMING_STATIC_LED_METHODID 6
#define ACER_WMID_SET_GAMING_FAN_BEHAVIOR 14
#define ACER_WMID_SET_GAMING_MISC_SETTING_METHODID 22
#define ACER_WMID_GET_GAMING_MISC_SETTING_METHODID 23

#define ACER_GAMING_MISC_SETTING_STATUS_MASK GENMASK_ULL(7, 0)
#define ACER_GAMING_MISC_SETTING_INDEX_MASK GENMASK_ULL(7, 0)
#define ACER_GAMING_MISC_SETTING_VALUE_MASK GENMASK_ULL(15, 8)

#define ACER_PREDATOR_V4_RETURN_STATUS_BIT_MASK GENMASK_ULL(7, 0)
#define ACER_PREDATOR_V4_SENSOR_INDEX_BIT_MASK GENMASK_ULL(15, 8)
#define ACER_PREDATOR_V4_SENSOR_READING_BIT_MASK GENMASK_ULL(23, 8)
#define ACER_PREDATOR_V4_SUPPORTED_SENSORS_BIT_MASK GENMASK_ULL(39, 24)

#define ACER_PREDATOR_V4_THERMAL_PROFILE_EC_OFFSET 0x54

#define ACER_PREDATOR_V4_FAN_SPEED_READ_BIT_MASK GENMASK(20, 8)

/*
 * Acer ACPI method GUIDs
 */
#define AMW0_GUID1		"67C3371D-95A3-4C37-BB61-DD47B491DAAB"
#define AMW0_GUID2		"431F16ED-0C2B-444C-B267-27DEB140CF9C"
#define WMID_GUID1		"6AF4F258-B401-42FD-BE91-3D4AC2D7C0D3"
#define WMID_GUID2		"95764E09-FB56-4E83-B31A-37761F60994A"
#define WMID_GUID3		"61EF69EA-865C-4BC3-A502-A0DEBA0CB531"
#define WMID_GUID4		"7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56"

/*
 * Acer ACPI event GUIDs
 */
#define ACERWMID_EVENT_GUID "676AA15E-6A47-4D9F-A2CC-1E6D18D14026"

MODULE_ALIAS("wmi:67C3371D-95A3-4C37-BB61-DD47B491DAAB");
MODULE_ALIAS("wmi:6AF4F258-B401-42FD-BE91-3D4AC2D7C0D3");
MODULE_ALIAS("wmi:676AA15E-6A47-4D9F-A2CC-1E6D18D14026");

enum acer_wmi_event_ids {
	WMID_HOTKEY_EVENT = 0x1,
	WMID_ACCEL_OR_KBD_DOCK_EVENT = 0x5,
	WMID_GAMING_TURBO_KEY_EVENT = 0x7,
};

enum acer_wmi_predator_v4_sys_info_command {
	ACER_WMID_CMD_GET_PREDATOR_V4_BAT_STATUS = 0x02,
	ACER_WMID_CMD_GET_PREDATOR_V4_CPU_FAN_SPEED = 0x0201,
	ACER_WMID_CMD_GET_PREDATOR_V4_GPU_FAN_SPEED = 0x0601,
};

enum acer_wmi_predator_v4_sensor_id {
	ACER_WMID_SENSOR_CPU_TEMPERATURE	= 0x01,
	ACER_WMID_SENSOR_CPU_FAN_SPEED		= 0x02,
	ACER_WMID_SENSOR_EXTERNAL_TEMPERATURE_2 = 0x03,
	ACER_WMID_SENSOR_GPU_FAN_SPEED		= 0x06,
	ACER_WMID_SENSOR_GPU_TEMPERATURE	= 0x0A,
};

enum acer_wmi_predator_v4_oc {
	ACER_WMID_OC_NORMAL			= 0x0000,
	ACER_WMID_OC_TURBO			= 0x0002,
};

enum acer_wmi_gaming_misc_setting {
	ACER_WMID_MISC_SETTING_OC_1			= 0x0005,
	ACER_WMID_MISC_SETTING_OC_2			= 0x0007,
	ACER_WMID_MISC_SETTING_SUPPORTED_PROFILES	= 0x000A,
	ACER_WMID_MISC_SETTING_PLATFORM_PROFILE		= 0x000B,
};

static const struct key_entry acer_wmi_keymap[] __initconst = {
	{KE_KEY, 0x01, {KEY_WLAN} },     /* WiFi */
	{KE_KEY, 0x03, {KEY_WLAN} },     /* WiFi */
	{KE_KEY, 0x04, {KEY_WLAN} },     /* WiFi */
	{KE_KEY, 0x12, {KEY_BLUETOOTH} },	/* BT */
	{KE_KEY, 0x21, {KEY_PROG1} },    /* Backup */
	{KE_KEY, 0x22, {KEY_PROG2} },    /* Arcade */
	{KE_KEY, 0x23, {KEY_PROG3} },    /* P_Key */
	{KE_KEY, 0x24, {KEY_PROG4} },    /* Social networking_Key */
	{KE_KEY, 0x27, {KEY_HELP} },
	{KE_KEY, 0x29, {KEY_PROG3} },    /* P_Key for TM8372 */
	{KE_IGNORE, 0x41, {KEY_MUTE} },
	{KE_IGNORE, 0x42, {KEY_PREVIOUSSONG} },
	{KE_IGNORE, 0x4d, {KEY_PREVIOUSSONG} },
	{KE_IGNORE, 0x43, {KEY_NEXTSONG} },
	{KE_IGNORE, 0x4e, {KEY_NEXTSONG} },
	{KE_IGNORE, 0x44, {KEY_PLAYPAUSE} },
	{KE_IGNORE, 0x4f, {KEY_PLAYPAUSE} },
	{KE_IGNORE, 0x45, {KEY_STOP} },
	{KE_IGNORE, 0x50, {KEY_STOP} },
	{KE_IGNORE, 0x48, {KEY_VOLUMEUP} },
	{KE_IGNORE, 0x49, {KEY_VOLUMEDOWN} },
	{KE_IGNORE, 0x4a, {KEY_VOLUMEDOWN} },
	/*
	 * 0x61 is KEY_SWITCHVIDEOMODE. Usually this is a duplicate input event
	 * with the "Video Bus" input device events. But sometimes it is not
	 * a dup. Map it to KEY_UNKNOWN instead of using KE_IGNORE so that
	 * udev/hwdb can override it on systems where it is not a dup.
	 */
	{KE_KEY, 0x61, {KEY_UNKNOWN} },
	{KE_IGNORE, 0x62, {KEY_BRIGHTNESSUP} },
	{KE_IGNORE, 0x63, {KEY_BRIGHTNESSDOWN} },
	{KE_KEY, 0x64, {KEY_SWITCHVIDEOMODE} },	/* Display Switch */
	{KE_IGNORE, 0x81, {KEY_SLEEP} },
	{KE_KEY, 0x82, {KEY_TOUCHPAD_TOGGLE} },	/* Touch Pad Toggle */
	{KE_IGNORE, 0x84, {KEY_KBDILLUMTOGGLE} }, /* Automatic Keyboard background light toggle */
	{KE_KEY, KEY_TOUCHPAD_ON, {KEY_TOUCHPAD_ON} },
	{KE_KEY, KEY_TOUCHPAD_OFF, {KEY_TOUCHPAD_OFF} },
	{KE_IGNORE, 0x83, {KEY_TOUCHPAD_TOGGLE} },
	{KE_KEY, 0x85, {KEY_TOUCHPAD_TOGGLE} },
	{KE_KEY, 0x86, {KEY_WLAN} },
	{KE_KEY, 0x87, {KEY_POWER} },
	/* Acer Predator macro keys:
		* 0xdaXY:
		*   da   - magic value (preDAtor)
		*     X  - macro key selector state (0: red, 1: blue, 2: green)
		*      Y - key pressed (0: 1, 1: 2, ...) */
	{KE_KEY, 0xda00, {KEY_PROG1} },
	{KE_KEY, 0xda01, {KEY_PROG2} },
	{KE_KEY, 0xda02, {KEY_PROG3} },
	{KE_KEY, 0xda03, {KEY_PROG4} },
	{KE_KEY, 0xda04, {KEY_F13} },
	{KE_KEY, 0xda10, {KEY_F14} },
	{KE_KEY, 0xda11, {KEY_F15} },
	{KE_KEY, 0xda12, {KEY_F16} },
	{KE_KEY, 0xda13, {KEY_F17} },
	{KE_KEY, 0xda14, {KEY_F18} },
	{KE_KEY, 0xda20, {KEY_F19} },
	{KE_KEY, 0xda21, {KEY_F20} },
	{KE_KEY, 0xda22, {KEY_F21} },
	{KE_KEY, 0xda23, {KEY_F22} },
	{KE_KEY, 0xda24, {KEY_F23} },
	{KE_END, 0}
};

static struct input_dev *acer_wmi_input_dev;
static struct input_dev *acer_wmi_accel_dev;

struct event_return_value {
	u8 function;
	u8 key_num;
	u16 device_state;
	u16 reserved1;
	u8 kbd_dock_state;
	u8 reserved2;
} __packed;

/*
 * GUID3 Get Device Status device flags
 */
#define ACER_WMID3_GDS_WIRELESS		(1<<0)	/* WiFi */
#define ACER_WMID3_GDS_THREEG		(1<<6)	/* 3G */
#define ACER_WMID3_GDS_WIMAX		(1<<7)	/* WiMAX */
#define ACER_WMID3_GDS_BLUETOOTH	(1<<11)	/* BT */
#define ACER_WMID3_GDS_RFBTN		(1<<14)	/* RF Button */

#define ACER_WMID3_GDS_TOUCHPAD		(1<<1)	/* Touchpad */

/*
 * Gaming functions user-space communication
 * A character drive will be exposed in /dev/acer-gkbbl as char block for keyboard dynamic backlight config
 * Config length is 16 bytes
 */

#define GAMING_KBBL_CHR "acer-gkbbl"
#define GAMING_KBBL_CONFIG_LEN 16


/*
 * Gaming functions user-space communication
 * A character drive will be exposed in /dev/acer-gkbbl-static as char block for keyboard static backlight config
 * Config length is 16 bytes
 */

#define GAMING_KBBL_STATIC_CHR "acer-gkbbl-static"
#define GAMING_KBBL_STATIC_CONFIG_LEN 4

/* Hotkey Customized Setting and Acer Application Status.
 * Set Device Default Value and Report Acer Application Status.
 * When Acer Application starts, it will run this method to inform
 * BIOS/EC that Acer Application is on.
 * App Status
 *	Bit[0]: Launch Manager Status
 *	Bit[1]: ePM Status
 *	Bit[2]: Device Control Status
 *	Bit[3]: Acer Power Button Utility Status
 *	Bit[4]: RF Button Status
 *	Bit[5]: ODD PM Status
 *	Bit[6]: Device Default Value Control
 *	Bit[7]: Hall Sensor Application Status
 */
struct func_input_params {
	u8 function_num;        /* Function Number */
	u16 commun_devices;     /* Communication type devices default status */
	u16 devices;            /* Other type devices default status */
	u8 app_status;          /* Acer Device Status. LM, ePM, RF Button... */
	u8 app_mask;		/* Bit mask to app_status */
	u8 reserved;
} __packed;

struct func_return_value {
	u8 error_code;          /* Error Code */
	u8 ec_return_value;     /* EC Return Value */
	u16 reserved;
} __packed;

struct wmid3_gds_set_input_param {     /* Set Device Status input parameter */
	u8 function_num;        /* Function Number */
	u8 hotkey_number;       /* Hotkey Number */
	u16 devices;            /* Set Device */
	u8 volume_value;        /* Volume Value */
} __packed;

struct wmid3_gds_get_input_param {     /* Get Device Status input parameter */
	u8 function_num;	/* Function Number */
	u8 hotkey_number;	/* Hotkey Number */
	u16 devices;		/* Get Device */
} __packed;

struct wmid3_gds_return_value {	/* Get Device Status return value*/
	u8 error_code;		/* Error Code */
	u8 ec_return_value;	/* EC Return Value */
	u16 devices;		/* Current Device Status */
	u32 reserved;
} __packed;

struct hotkey_function_type_aa {
	u8 type;
	u8 length;
	u16 handle;
	u16 commun_func_bitmap;
	u16 application_func_bitmap;
	u16 media_func_bitmap;
	u16 display_func_bitmap;
	u16 others_func_bitmap;
	u8 commun_fn_key_number;
} __packed;

/*
 * Interface capability flags
 */
#define ACER_CAP_MAILLED		BIT(0)
#define ACER_CAP_WIRELESS		BIT(1)
#define ACER_CAP_BLUETOOTH		BIT(2)
#define ACER_CAP_BRIGHTNESS		BIT(3)
#define ACER_CAP_THREEG			BIT(4)
#define ACER_CAP_SET_FUNCTION_MODE	BIT(5)
#define ACER_CAP_KBD_DOCK		BIT(6)
#define ACER_CAP_TURBO_OC		BIT(7)
#define ACER_CAP_TURBO_LED		BIT(8)
#define ACER_CAP_TURBO_FAN		BIT(9)
#define ACER_CAP_PLATFORM_PROFILE	BIT(10)
#define ACER_CAP_FAN_SPEED_READ		BIT(11)
#define ACER_CAP_GAMINGKB		BIT(12)
#define ACER_CAP_GAMINGKB_STATIC	BIT(13)

/*
 * Interface type flags
 */
enum interface_flags {
	ACER_AMW0,
	ACER_AMW0_V2,
	ACER_WMID,
	ACER_WMID_v2,
	ACER_WMID_GAMING,
};

#define ACER_DEFAULT_WIRELESS  0
#define ACER_DEFAULT_BLUETOOTH 0
#define ACER_DEFAULT_MAILLED   0
#define ACER_DEFAULT_THREEG    0

static int max_brightness = 0xF;

static int mailled = -1;
static int brightness = -1;
static int threeg = -1;
static int force_series;
static int force_caps = -1;
static bool ec_raw_mode;
static bool has_type_aa;
static int turbo_state = 0;
static u16 commun_func_bitmap;
static u8 commun_fn_key_number;
static u8 macro_key_state = 0;
static bool cycle_gaming_thermal_profile = true;
static bool predator_v4;

module_param(mailled, int, 0444);
module_param(brightness, int, 0444);
module_param(threeg, int, 0444);
module_param(force_series, int, 0444);
module_param(force_caps, int, 0444);
module_param(ec_raw_mode, bool, 0444);
module_param(cycle_gaming_thermal_profile, bool, 0644);
module_param(predator_v4, bool, 0444);
MODULE_PARM_DESC(mailled, "Set initial state of Mail LED");
MODULE_PARM_DESC(brightness, "Set initial LCD backlight brightness");
MODULE_PARM_DESC(threeg, "Set initial state of 3G hardware");
MODULE_PARM_DESC(force_series, "Force a different laptop series");
MODULE_PARM_DESC(force_caps, "Force the capability bitmask to this value");
MODULE_PARM_DESC(ec_raw_mode, "Enable EC raw mode");
MODULE_PARM_DESC(cycle_gaming_thermal_profile,
	"Set thermal mode key in cycle mode. Disabling it sets the mode key in turbo toggle mode");
MODULE_PARM_DESC(predator_v4,
	"Enable features for predator laptops that use predator sense v4");

#ifdef lts
int platform_profile_remove()
{
	return 0;
}
int platform_profile_register(struct platform_profile_handler* platform_profile_handler)
{
	return 0;
}
void platform_profile_notify()
{
}
#endif

struct acer_data {
	int mailled;
	int threeg;
	int brightness;
};

struct acer_debug {
	struct dentry *root;
	u32 wmid_devices;
};

static struct rfkill *wireless_rfkill;
static struct rfkill *bluetooth_rfkill;
static struct rfkill *threeg_rfkill;
static bool rfkill_inited;

/* Each low-level interface must define at least some of the following */
struct wmi_interface {
	/* The WMI device type */
	u32 type;

	/* The capabilities this interface provides */
	u32 capability;

	/* Private data for the current interface */
	struct acer_data data;

	/* debugfs entries associated with this interface */
	struct acer_debug debug;
};

/* The static interface pointer, points to the currently detected interface */
static struct wmi_interface *interface;

/* The static gaming interface pointer, points to the currently detected gaming interface */
static struct wmi_interface *gaming_interface;

/*
 * Character device registration
 * GAMING_KBBL_MINOR -> used to configure gaming rgb keyboard backlights from user-space
 */

#define GAMING_KBBL_MINOR 0
#define GAMING_KBBL_STATIC_MINOR 0


static dev_t gkbbl_static_dev; // Global variable to store the static kbbl device
static dev_t gkbbl_dynamic_dev; // Global variable to store the dynamic kbbl device


/*
 * Embedded Controller quirks
 * Some laptops require us to directly access the EC to either enable or query
 * features that are not available through WMI.
 */

struct quirk_entry {
	u8 wireless;
	u8 mailled;
	s8 brightness;
	u8 bluetooth;
	u8 turbo;
	u8 cpu_fans;
	u8 gpu_fans;
	u8 predator_v4;
};

static struct quirk_entry *quirks;

static void __init set_quirks(void)
{
	if (quirks->mailled)
		interface->capability |= ACER_CAP_MAILLED;

	if (quirks->brightness)
		interface->capability |= ACER_CAP_BRIGHTNESS;

	if (quirks->turbo)
		interface->capability |= ACER_CAP_TURBO_OC | ACER_CAP_TURBO_LED
					 | ACER_CAP_TURBO_FAN;

	if (quirks->predator_v4)
		interface->capability |= ACER_CAP_PLATFORM_PROFILE |
					 ACER_CAP_FAN_SPEED_READ;
}

static int __init dmi_matched(const struct dmi_system_id *dmi)
{
	quirks = dmi->driver_data;
	return 1;
}

static int __init set_force_caps(const struct dmi_system_id *dmi)
{
	if (force_caps == -1) {
		force_caps = (uintptr_t)dmi->driver_data;
		pr_info("Found %s, set force_caps to 0x%x\n", dmi->ident, force_caps);
	}
	return 1;
}

static struct quirk_entry quirk_unknown = {
};

static struct quirk_entry quirk_acer_aspire_1520 = {
	.brightness = -1,
};

static struct quirk_entry quirk_acer_travelmate_2490 = {
	.mailled = 1,
};

static struct quirk_entry quirk_acer_predator_ph315_51s = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_predator_ph315_52s = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_predator_ph315_52 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_predator_ph16_71 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};


static struct quirk_entry quirk_acer_predator_phn16_71 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_predator_phn18_71 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_predator_phn18_72 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_predator_ph315_53 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_predator_ph315_54 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_predator_ph315_55 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_predator_ph317_53 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_ph317_54 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_ph317_56 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_ph517_51 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_ph517_52 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_ph517_61 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_ph717_71 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_ph717_72 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_pt314_52s = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_pt315_51 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_pt315_52 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_pt316_51 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};
static struct quirk_entry quirk_acer_predator_pt515_51 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 2,
};
static struct quirk_entry quirk_acer_predator_pt515_52 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 2,
};
static struct quirk_entry quirk_acer_predator_pt516_52s = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 2,
};
static struct quirk_entry quirk_acer_predator_pt917_71 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_nitro_an515_58 = {
	.turbo = 1,
	.cpu_fans = 1,
	.gpu_fans = 1,
};

static struct quirk_entry quirk_acer_predator_v4 = {
	.predator_v4 = 1,
};

/* This AMW0 laptop has no bluetooth */
static struct quirk_entry quirk_medion_md_98300 = {
	.wireless = 1,
};

static struct quirk_entry quirk_fujitsu_amilo_li_1718 = {
	.wireless = 2,
};

static struct quirk_entry quirk_lenovo_ideapad_s205 = {
	.wireless = 3,
};

/* The Aspire One has a dummy ACPI-WMI interface - disable it */
static const struct dmi_system_id acer_blacklist[] __initconst = {
	{
		.ident = "Acer Aspire One (SSD)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "AOA110"),
		},
	},
	{
		.ident = "Acer Aspire One (HDD)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "AOA150"),
		},
	},
	{}
};

static const struct dmi_system_id amw0_whitelist[] __initconst = {
	{
		.ident = "Acer",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
		},
	},
	{
		.ident = "Gateway",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Gateway"),
		},
	},
	{
		.ident = "Packard Bell",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Packard Bell"),
		},
	},
	{}
};

/*
 * This quirk table is only for Acer/Gateway/Packard Bell family
 * that those machines are supported by acer-wmi driver.
 */
static const struct dmi_system_id acer_quirks[] __initconst = {
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 1360",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 1360"),
		},
		.driver_data = &quirk_acer_aspire_1520,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH16-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,"Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME,"Predator PH16-71"),
		},
		.driver_data = &quirk_acer_predator_ph16_71,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PHN16-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,"Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME,"Predator PHN16-71"),
		},
		.driver_data = &quirk_acer_predator_phn16_71,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PHN18-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,"Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME,"Predator PHN18-71"),
		},
		.driver_data = &quirk_acer_predator_phn18_71,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PHN18-72",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,"Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME,"Predator PHN18-72"),
		},
		.driver_data = &quirk_acer_predator_phn18_72,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 1520",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 1520"),
		},
		.driver_data = &quirk_acer_aspire_1520,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 3100",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 3100"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 3610",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 3610"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 5100",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5100"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 5610",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5610"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 5630",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5630"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 5650",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5650"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 5680",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5680"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Aspire 9110",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 9110"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer TravelMate 2490",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 2490"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer TravelMate 4200",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 4200"),
		},
		.driver_data = &quirk_acer_travelmate_2490,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH314-51s",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH314-51s"),
		},
		.driver_data = &quirk_acer_predator_ph315_51s,
	},
		{
		.callback = dmi_matched,
		.ident = "Acer Predator PH314-52s",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH314-52s"),
		},
		.driver_data = &quirk_acer_predator_ph315_52s,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH315-52",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH315-52"),
		},
		.driver_data = &quirk_acer_predator_ph315_52,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH315-53",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH315-53"),
		},
		.driver_data = &quirk_acer_predator_ph315_53,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH315-54",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH315-54"),
		},
		.driver_data = &quirk_acer_predator_ph315_54,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH315-55",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH315-55"),
		},
		.driver_data = &quirk_acer_predator_ph315_55,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH317-53",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH317-53"),
		},
		.driver_data = &quirk_acer_predator_ph317_53,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH317-54",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH317-54"),
		},
		.driver_data = &quirk_acer_predator_ph317_54,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH317-56",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH317-56"),
		},
		.driver_data = &quirk_acer_predator_ph317_56,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH517-51",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH517-51"),
		},
		.driver_data = &quirk_acer_predator_ph517_51,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH517-52",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH517-52"),
		},
		.driver_data = &quirk_acer_predator_ph517_52,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH517-61",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH517-61"),
		},
		.driver_data = &quirk_acer_predator_ph517_61,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH717-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH717-71"),
		},
		.driver_data = &quirk_acer_predator_ph717_71,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH717-72",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH717-72"),
		},
		.driver_data = &quirk_acer_predator_ph717_72,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PT315-51",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PT315-51"),
		},
		.driver_data = &quirk_acer_predator_pt315_51,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PT314-52S",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PT314-52s"),
		},
		.driver_data = &quirk_acer_predator_pt314_52s,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PT315-52",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PT315-52"),
		},
		.driver_data = &quirk_acer_predator_pt315_52,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PT515-51",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PT515-51"),
		},
		.driver_data = &quirk_acer_predator_pt515_51,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PT316-51",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PT316-51"),
		},
		.driver_data = &quirk_acer_predator_pt316_51,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PT515-52",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PT515-52"),
		},
		.driver_data = &quirk_acer_predator_pt515_52,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PT516-52s",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PT516-52s"),
		},
		.driver_data = &quirk_acer_predator_pt516_52s,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PT917-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PT917-71"),
		},
		.driver_data = &quirk_acer_predator_pt917_71,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Nitro AN515-58",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Nitro AN515-58"),
		},
		.driver_data = &quirk_acer_nitro_an515_58,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PHN16-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PHN16-71"),
		},
		.driver_data = &quirk_acer_predator_v4,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH16-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH16-71"),
		},
		.driver_data = &quirk_acer_predator_v4,
	},
	{
		.callback = dmi_matched,
		.ident = "Acer Predator PH18-71",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Predator PH18-71"),
		},
		.driver_data = &quirk_acer_predator_v4,
	},
	{
		.callback = set_force_caps,
		.ident = "Acer Aspire Switch 10E SW3-016",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire SW3-016"),
		},
		.driver_data = (void *)ACER_CAP_KBD_DOCK,
	},
	{
		.callback = set_force_caps,
		.ident = "Acer Aspire Switch 10 SW5-012",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire SW5-012"),
		},
		.driver_data = (void *)ACER_CAP_KBD_DOCK,
	},
	{
		.callback = set_force_caps,
		.ident = "Acer Aspire Switch V 10 SW5-017",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "SW5-017"),
		},
		.driver_data = (void *)ACER_CAP_KBD_DOCK,
	},
	{
		.callback = set_force_caps,
		.ident = "Acer One 10 (S1003)",
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "One S1003"),
		},
		.driver_data = (void *)ACER_CAP_KBD_DOCK,
	},
	{}
};

/*
 * This quirk list is for those non-acer machines that have AMW0_GUID1
 * but supported by acer-wmi in past days. Keeping this quirk list here
 * is only for backward compatible. Please do not add new machine to
 * here anymore. Those non-acer machines should be supported by
 * appropriate wmi drivers.
 */
static const struct dmi_system_id non_acer_quirks[] __initconst = {
	{
		.callback = dmi_matched,
		.ident = "Fujitsu Siemens Amilo Li 1718",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU SIEMENS"),
			DMI_MATCH(DMI_PRODUCT_NAME, "AMILO Li 1718"),
		},
		.driver_data = &quirk_fujitsu_amilo_li_1718,
	},
	{
		.callback = dmi_matched,
		.ident = "Medion MD 98300",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "MEDION"),
			DMI_MATCH(DMI_PRODUCT_NAME, "WAM2030"),
		},
		.driver_data = &quirk_medion_md_98300,
	},
	{
		.callback = dmi_matched,
		.ident = "Lenovo Ideapad S205",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "10382LG"),
		},
		.driver_data = &quirk_lenovo_ideapad_s205,
	},
	{
		.callback = dmi_matched,
		.ident = "Lenovo Ideapad S205 (Brazos)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Brazos"),
		},
		.driver_data = &quirk_lenovo_ideapad_s205,
	},
	{
		.callback = dmi_matched,
		.ident = "Lenovo 3000 N200",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "0687A31"),
		},
		.driver_data = &quirk_fujitsu_amilo_li_1718,
	},
	{
		.callback = dmi_matched,
		.ident = "Lenovo Ideapad S205-10382JG",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "10382JG"),
		},
		.driver_data = &quirk_lenovo_ideapad_s205,
	},
	{
		.callback = dmi_matched,
		.ident = "Lenovo Ideapad S205-1038DPG",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "1038DPG"),
		},
		.driver_data = &quirk_lenovo_ideapad_s205,
	},
	{}
};
#if RTLNX_VER_MIN(6, 14, 0)
static struct device *platform_profile_device;
#else
static struct platform_profile_handler platform_profile_handler;
#endif
static bool platform_profile_support;

/*
 * The profile used before turbo mode. This variable is needed for
 * returning from turbo mode when the mode key is in toggle mode.
 */
static int last_non_turbo_profile = INT_MIN;

/*
 * The profile used before turbo mode. This variable is needed for
 * returning from turbo mode when the mode key is in toggle mode.
 */
static int last_non_turbo_profile;

/* The most performant supported profile */
static int acer_predator_v4_max_perf;

enum acer_predator_v4_thermal_profile_ec {
	ACER_PREDATOR_V4_THERMAL_PROFILE_ECO = 0x04,
	ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO = 0x03,
	ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE = 0x02,
	ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET = 0x01,
	ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED = 0x00,
};

enum acer_predator_v4_thermal_profile_wmi {
	ACER_PREDATOR_V4_THERMAL_PROFILE_ECO_WMI = 0x060B,
	ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO_WMI = 0x050B,
	ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE_WMI = 0x040B,
	ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET_WMI = 0x0B,
	ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED_WMI = 0x010B,
};

/* Find which quirks are needed for a particular vendor/ model pair */
static void __init find_quirks(void)
{
	if (predator_v4) {
		quirks = &quirk_acer_predator_v4;
	} else if (!force_series) {
		dmi_check_system(acer_quirks);
		dmi_check_system(non_acer_quirks);
	} else if (force_series == 2490) {
		quirks = &quirk_acer_travelmate_2490;
	}

	if (quirks == NULL)
		quirks = &quirk_unknown;
}

/*
 * General interface convenience methods
 */

static bool has_cap(u32 cap)
{
	return interface->capability & cap;
}

/*
 * AMW0 (V1) interface
 */
struct wmab_args {
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;
};

struct wmab_ret {
	u32 eax;
	u32 ebx;
	u32 ecx;
	u32 edx;
	u32 eex;
};

static acpi_status wmab_execute(struct wmab_args *regbuf,
struct acpi_buffer *result)
{
	struct acpi_buffer input;
	acpi_status status;
	input.length = sizeof(struct wmab_args);
	input.pointer = (u8 *)regbuf;

	status = wmi_evaluate_method(AMW0_GUID1, 0, 1, &input, result);

	return status;
}

static acpi_status AMW0_get_u32(u32 *value, u32 cap)
{
	int err;
	u8 result;

	switch (cap) {
	case ACER_CAP_MAILLED:
		switch (quirks->mailled) {
		default:
			err = ec_read(0xA, &result);
			if (err)
				return AE_ERROR;
			*value = (result >> 7) & 0x1;
			return AE_OK;
		}
		break;
	case ACER_CAP_WIRELESS:
		switch (quirks->wireless) {
		case 1:
			err = ec_read(0x7B, &result);
			if (err)
				return AE_ERROR;
			*value = result & 0x1;
			return AE_OK;
		case 2:
			err = ec_read(0x71, &result);
			if (err)
				return AE_ERROR;
			*value = result & 0x1;
			return AE_OK;
		case 3:
			err = ec_read(0x78, &result);
			if (err)
				return AE_ERROR;
			*value = result & 0x1;
			return AE_OK;
		default:
			err = ec_read(0xA, &result);
			if (err)
				return AE_ERROR;
			*value = (result >> 2) & 0x1;
			return AE_OK;
		}
		break;
	case ACER_CAP_BLUETOOTH:
		switch (quirks->bluetooth) {
		default:
			err = ec_read(0xA, &result);
			if (err)
				return AE_ERROR;
			*value = (result >> 4) & 0x1;
			return AE_OK;
		}
		break;
	case ACER_CAP_BRIGHTNESS:
		switch (quirks->brightness) {
		default:
			err = ec_read(0x83, &result);
			if (err)
				return AE_ERROR;
			*value = result;
			return AE_OK;
		}
		break;
	default:
		return AE_ERROR;
	}
	return AE_OK;
}

static acpi_status AMW0_set_u32(u32 value, u32 cap)
{
	struct wmab_args args;

	args.eax = ACER_AMW0_WRITE;
	args.ebx = value ? (1<<8) : 0;
	args.ecx = args.edx = 0;

	switch (cap) {
	case ACER_CAP_MAILLED:
		if (value > 1)
			return AE_BAD_PARAMETER;
		args.ebx |= ACER_AMW0_MAILLED_MASK;
		break;
	case ACER_CAP_WIRELESS:
		if (value > 1)
			return AE_BAD_PARAMETER;
		args.ebx |= ACER_AMW0_WIRELESS_MASK;
		break;
	case ACER_CAP_BLUETOOTH:
		if (value > 1)
			return AE_BAD_PARAMETER;
		args.ebx |= ACER_AMW0_BLUETOOTH_MASK;
		break;
	case ACER_CAP_BRIGHTNESS:
		if (value > max_brightness)
			return AE_BAD_PARAMETER;
		switch (quirks->brightness) {
		default:
			return ec_write(0x83, value);
		}
	default:
		return AE_ERROR;
	}

	/* Actually do the set */
	return wmab_execute(&args, NULL);
}

static acpi_status __init AMW0_find_mailled(void)
{
	struct wmab_args args;
	struct wmab_ret ret;
	acpi_status status = AE_OK;
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;

	args.eax = 0x86;
	args.ebx = args.ecx = args.edx = 0;

	status = wmab_execute(&args, &out);
	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *) out.pointer;
	if (obj && obj->type == ACPI_TYPE_BUFFER &&
	obj->buffer.length == sizeof(struct wmab_ret)) {
		ret = *((struct wmab_ret *) obj->buffer.pointer);
	} else {
		kfree(out.pointer);
		return AE_ERROR;
	}

	if (ret.eex & 0x1)
		interface->capability |= ACER_CAP_MAILLED;

	kfree(out.pointer);

	return AE_OK;
}

static const struct acpi_device_id norfkill_ids[] __initconst = {
	{ "VPC2004", 0},
	{ "IBM0068", 0},
	{ "LEN0068", 0},
	{ "SNY5001", 0},	/* sony-laptop in charge */
	{ "HPQ6601", 0},
	{ "", 0},
};

static int __init AMW0_set_cap_acpi_check_device(void)
{
	const struct acpi_device_id *id;

	for (id = norfkill_ids; id->id[0]; id++)
		if (acpi_dev_found(id->id))
			return true;

	return false;
}

static acpi_status __init AMW0_set_capabilities(void)
{
	struct wmab_args args;
	struct wmab_ret ret;
	acpi_status status;
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;

	/*
	 * On laptops with this strange GUID (non Acer), normal probing doesn't
	 * work.
	 */
	if (wmi_has_guid(AMW0_GUID2)) {
		if ((quirks != &quirk_unknown) ||
		    !AMW0_set_cap_acpi_check_device())
			interface->capability |= ACER_CAP_WIRELESS;
		return AE_OK;
	}

	args.eax = ACER_AMW0_WRITE;
	args.ecx = args.edx = 0;

	args.ebx = 0xa2 << 8;
	args.ebx |= ACER_AMW0_WIRELESS_MASK;

	status = wmab_execute(&args, &out);
	if (ACPI_FAILURE(status))
		return status;

	obj = out.pointer;
	if (obj && obj->type == ACPI_TYPE_BUFFER &&
	obj->buffer.length == sizeof(struct wmab_ret)) {
		ret = *((struct wmab_ret *) obj->buffer.pointer);
	} else {
		status = AE_ERROR;
		goto out;
	}

	if (ret.eax & 0x1)
		interface->capability |= ACER_CAP_WIRELESS;

	args.ebx = 2 << 8;
	args.ebx |= ACER_AMW0_BLUETOOTH_MASK;

	/*
	 * It's ok to use existing buffer for next wmab_execute call.
	 * But we need to kfree(out.pointer) if next wmab_execute fail.
	 */
	status = wmab_execute(&args, &out);
	if (ACPI_FAILURE(status))
		goto out;

	obj = (union acpi_object *) out.pointer;
	if (obj && obj->type == ACPI_TYPE_BUFFER
	&& obj->buffer.length == sizeof(struct wmab_ret)) {
		ret = *((struct wmab_ret *) obj->buffer.pointer);
	} else {
		status = AE_ERROR;
		goto out;
	}

	if (ret.eax & 0x1)
		interface->capability |= ACER_CAP_BLUETOOTH;

	/*
	 * This appears to be safe to enable, since all Wistron based laptops
	 * appear to use the same EC register for brightness, even if they
	 * differ for wireless, etc
	 */
	if (quirks->brightness >= 0)
		interface->capability |= ACER_CAP_BRIGHTNESS;

	status = AE_OK;
out:
	kfree(out.pointer);
	return status;
}

static struct wmi_interface AMW0_interface = {
	.type = ACER_AMW0,
};

static struct wmi_interface AMW0_V2_interface = {
	.type = ACER_AMW0_V2,
};

/*
 * New interface (The WMID interface)
 */
static acpi_status
WMI_execute_u32(u32 method_id, u32 in, u32 *out)
{
	struct acpi_buffer input = { (acpi_size) sizeof(u32), (void *)(&in) };
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	u32 tmp = 0;
	acpi_status status;

	status = wmi_evaluate_method(WMID_GUID1, 0, method_id, &input, &result);

	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *) result.pointer;
	if (obj) {
		if (obj->type == ACPI_TYPE_BUFFER &&
			(obj->buffer.length == sizeof(u32) ||
			obj->buffer.length == sizeof(u64))) {
			tmp = *((u32 *) obj->buffer.pointer);
		} else if (obj->type == ACPI_TYPE_INTEGER) {
			tmp = (u32) obj->integer.value;
		}
	}

	if (out)
		*out = tmp;

	kfree(result.pointer);

	return status;
}

static acpi_status WMID_get_u32(u32 *value, u32 cap)
{
	acpi_status status;
	u8 tmp;
	u32 result, method_id = 0;

	switch (cap) {
	case ACER_CAP_WIRELESS:
		method_id = ACER_WMID_GET_WIRELESS_METHODID;
		break;
	case ACER_CAP_BLUETOOTH:
		method_id = ACER_WMID_GET_BLUETOOTH_METHODID;
		break;
	case ACER_CAP_BRIGHTNESS:
		method_id = ACER_WMID_GET_BRIGHTNESS_METHODID;
		break;
	case ACER_CAP_THREEG:
		method_id = ACER_WMID_GET_THREEG_METHODID;
		break;
	case ACER_CAP_MAILLED:
		if (quirks->mailled == 1) {
			ec_read(0x9f, &tmp);
			*value = tmp & 0x1;
			return 0;
		}
		fallthrough;
	default:
		return AE_ERROR;
	}
	status = WMI_execute_u32(method_id, 0, &result);

	if (ACPI_SUCCESS(status))
		*value = (u8)result;

	return status;
}

static acpi_status WMID_set_u32(u32 value, u32 cap)
{
	u32 method_id = 0;
	char param;

	switch (cap) {
	case ACER_CAP_BRIGHTNESS:
		if (value > max_brightness)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_BRIGHTNESS_METHODID;
		break;
	case ACER_CAP_WIRELESS:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_WIRELESS_METHODID;
		break;
	case ACER_CAP_BLUETOOTH:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_BLUETOOTH_METHODID;
		break;
	case ACER_CAP_THREEG:
		if (value > 1)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_THREEG_METHODID;
		break;
	case ACER_CAP_MAILLED:
		if (value > 1)
			return AE_BAD_PARAMETER;
		if (quirks->mailled == 1) {
			param = value ? 0x92 : 0x93;
			i8042_lock_chip();
			i8042_command(&param, 0x1059);
			i8042_unlock_chip();
			return 0;
		}
		break;
	default:
		return AE_ERROR;
	}
	return WMI_execute_u32(method_id, (u32)value, NULL);
}

static acpi_status wmid3_get_device_status(u32 *value, u16 device)
{
	struct wmid3_gds_return_value return_value;
	acpi_status status;
	union acpi_object *obj;
	struct wmid3_gds_get_input_param params = {
		.function_num = 0x1,
		.hotkey_number = commun_fn_key_number,
		.devices = device,
	};
	struct acpi_buffer input = {
		sizeof(struct wmid3_gds_get_input_param),
		&params
	};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmi_evaluate_method(WMID_GUID3, 0, 0x2, &input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;

	if (!obj)
		return AE_ERROR;
	else if (obj->type != ACPI_TYPE_BUFFER) {
		kfree(obj);
		return AE_ERROR;
	}
	if (obj->buffer.length != 8) {
		pr_warn("Unknown buffer length %d\n", obj->buffer.length);
		kfree(obj);
		return AE_ERROR;
	}

	return_value = *((struct wmid3_gds_return_value *)obj->buffer.pointer);
	kfree(obj);

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Get 0x%x Device Status failed: 0x%x - 0x%x\n",
			device,
			return_value.error_code,
			return_value.ec_return_value);
	else
		*value = !!(return_value.devices & device);

	return status;
}

static acpi_status wmid_v2_get_u32(u32 *value, u32 cap)
{
	u16 device;

	switch (cap) {
	case ACER_CAP_WIRELESS:
		device = ACER_WMID3_GDS_WIRELESS;
		break;
	case ACER_CAP_BLUETOOTH:
		device = ACER_WMID3_GDS_BLUETOOTH;
		break;
	case ACER_CAP_THREEG:
		device = ACER_WMID3_GDS_THREEG;
		break;
	default:
		return AE_ERROR;
	}
	return wmid3_get_device_status(value, device);
}

static acpi_status wmid3_set_device_status(u32 value, u16 device)
{
	struct wmid3_gds_return_value return_value;
	acpi_status status;
	union acpi_object *obj;
	u16 devices;
	struct wmid3_gds_get_input_param get_params = {
		.function_num = 0x1,
		.hotkey_number = commun_fn_key_number,
		.devices = commun_func_bitmap,
	};
	struct acpi_buffer get_input = {
		sizeof(struct wmid3_gds_get_input_param),
		&get_params
	};
	struct wmid3_gds_set_input_param set_params = {
		.function_num = 0x2,
		.hotkey_number = commun_fn_key_number,
		.devices = commun_func_bitmap,
	};
	struct acpi_buffer set_input = {
		sizeof(struct wmid3_gds_set_input_param),
		&set_params
	};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer output2 = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmi_evaluate_method(WMID_GUID3, 0, 0x2, &get_input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;

	if (!obj)
		return AE_ERROR;
	else if (obj->type != ACPI_TYPE_BUFFER) {
		kfree(obj);
		return AE_ERROR;
	}
	if (obj->buffer.length != 8) {
		pr_warn("Unknown buffer length %d\n", obj->buffer.length);
		kfree(obj);
		return AE_ERROR;
	}

	return_value = *((struct wmid3_gds_return_value *)obj->buffer.pointer);
	kfree(obj);

	if (return_value.error_code || return_value.ec_return_value) {
		pr_warn("Get Current Device Status failed: 0x%x - 0x%x\n",
			return_value.error_code,
			return_value.ec_return_value);
		return status;
	}

	devices = return_value.devices;
	set_params.devices = (value) ? (devices | device) : (devices & ~device);

	status = wmi_evaluate_method(WMID_GUID3, 0, 0x1, &set_input, &output2);
	if (ACPI_FAILURE(status))
		return status;

	obj = output2.pointer;

	if (!obj)
		return AE_ERROR;
	else if (obj->type != ACPI_TYPE_BUFFER) {
		kfree(obj);
		return AE_ERROR;
	}
	if (obj->buffer.length != 4) {
		pr_warn("Unknown buffer length %d\n", obj->buffer.length);
		kfree(obj);
		return AE_ERROR;
	}

	return_value = *((struct wmid3_gds_return_value *)obj->buffer.pointer);
	kfree(obj);

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Set Device Status failed: 0x%x - 0x%x\n",
			return_value.error_code,
			return_value.ec_return_value);

	return status;
}

static acpi_status wmid_v2_set_u32(u32 value, u32 cap)
{
	u16 device;

	switch (cap) {
	case ACER_CAP_WIRELESS:
		device = ACER_WMID3_GDS_WIRELESS;
		break;
	case ACER_CAP_BLUETOOTH:
		device = ACER_WMID3_GDS_BLUETOOTH;
		break;
	case ACER_CAP_THREEG:
		device = ACER_WMID3_GDS_THREEG;
		break;
	default:
		return AE_ERROR;
	}
	return wmid3_set_device_status(value, device);
}

static void __init type_aa_dmi_decode(const struct dmi_header *header, void *d)
{
	struct hotkey_function_type_aa *type_aa;

	/* We are looking for OEM-specific Type AAh */
	if (header->type != 0xAA)
		return;

	has_type_aa = true;
	type_aa = (struct hotkey_function_type_aa *) header;

	pr_info("Function bitmap for Communication Button: 0x%x\n",
		type_aa->commun_func_bitmap);
	commun_func_bitmap = type_aa->commun_func_bitmap;

	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_WIRELESS)
		interface->capability |= ACER_CAP_WIRELESS;
	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_THREEG)
		interface->capability |= ACER_CAP_THREEG;
	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_BLUETOOTH)
		interface->capability |= ACER_CAP_BLUETOOTH;
	if (type_aa->commun_func_bitmap & ACER_WMID3_GDS_RFBTN)
		commun_func_bitmap &= ~ACER_WMID3_GDS_RFBTN;

	commun_fn_key_number = type_aa->commun_fn_key_number;
}

static acpi_status __init WMID_set_capabilities(void)
{
	struct acpi_buffer out = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *obj;
	acpi_status status;
	u32 devices;

	status = wmi_query_block(WMID_GUID2, 0, &out);
	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *) out.pointer;
	if (obj) {
		if (obj->type == ACPI_TYPE_BUFFER &&
			(obj->buffer.length == sizeof(u32) ||
			obj->buffer.length == sizeof(u64))) {
			devices = *((u32 *) obj->buffer.pointer);
		} else if (obj->type == ACPI_TYPE_INTEGER) {
			devices = (u32) obj->integer.value;
		} else {
			kfree(out.pointer);
			return AE_ERROR;
		}
	} else {
		kfree(out.pointer);
		return AE_ERROR;
	}

	pr_info("Function bitmap for Communication Device: 0x%x\n", devices);
	if (devices & 0x07)
		interface->capability |= ACER_CAP_WIRELESS;
	if (devices & 0x40)
		interface->capability |= ACER_CAP_THREEG;
	if (devices & 0x10)
		interface->capability |= ACER_CAP_BLUETOOTH;

	if (!(devices & 0x20))
		max_brightness = 0x9;

	kfree(out.pointer);
	return status;
}

static struct wmi_interface wmid_interface = {
	.type = ACER_WMID,
};

static struct wmi_interface wmid_v2_interface = {
	.type = ACER_WMID_v2,
};

/*
 * WMID Gaming interface
 */

static struct wmi_interface wmid_gaming_interface = {
	.type = ACER_WMID_GAMING
};

static acpi_status
WMI_gaming_execute_u8_array(u32 method_id, u8 array[], size_t array_size, u32 *out)
{
	struct acpi_buffer input = { (acpi_size) array_size, (void *)(array) };
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	u32 tmp = 0;
	acpi_status status;

	status = wmi_evaluate_method(WMID_GUID4, 0, method_id, &input, &result);

	if (ACPI_FAILURE(status))
		return status;

	obj = (union acpi_object *) result.pointer;
	if (obj) {
		if (obj->type == ACPI_TYPE_BUFFER &&
			(obj->buffer.length == sizeof(u32) ||
			obj->buffer.length == sizeof(u64))) {
			tmp = *((u32 *) obj->buffer.pointer);
		} else if (obj->type == ACPI_TYPE_INTEGER) {
			tmp = (u32) obj->integer.value;
		}
	}

	if (out)
		*out = tmp;

	kfree(result.pointer);

	return status;
}


static acpi_status
WMI_gaming_execute_u64(u32 method_id, u64 in, u64 *out)
{
	struct acpi_buffer input = { (acpi_size) sizeof(u64), (void *)(&in) };
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	u64 tmp = 0;
	acpi_status status;

	status = wmi_evaluate_method(WMID_GUID4, 0, method_id, &input, &result);

	if (ACPI_FAILURE(status))
		return status;
	obj = (union acpi_object *) result.pointer;

	if (obj) {
		if (obj->type == ACPI_TYPE_BUFFER) {
			if (obj->buffer.length == sizeof(u32))
				tmp = *((u32 *) obj->buffer.pointer);
			else if (obj->buffer.length == sizeof(u64))
				tmp = *((u64 *) obj->buffer.pointer);
		} else if (obj->type == ACPI_TYPE_INTEGER) {
			tmp = (u64) obj->integer.value;
		}
	}

	if (out)
		*out = tmp;

	kfree(result.pointer);

	return status;
}
#if RTLNX_VER_MIN(6, 14, 0)
static int WMI_gaming_execute_u32_u64(u32 method_id, u32 in, u64 *out)
{
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer input = {
		.length = sizeof(in),
		.pointer = &in,
	};
	union acpi_object *obj;
	acpi_status status;
	int ret = 0;

	status = wmi_evaluate_method(WMID_GUID4, 0, method_id, &input, &result);
	if (ACPI_FAILURE(status))
		return -EIO;

	obj = result.pointer;
	if (obj && out) {
		switch (obj->type) {
		case ACPI_TYPE_INTEGER:
			*out = obj->integer.value;
			break;
		case ACPI_TYPE_BUFFER:
			if (obj->buffer.length < sizeof(*out))
				ret = -ENOMSG;
			else
				*out = get_unaligned_le64(obj->buffer.pointer);

			break;
		default:
			ret = -ENOMSG;
			break;
		}
	}

	kfree(obj);

	return ret;
}
#endif

static acpi_status WMID_gaming_set_u64(u64 value, u32 cap)
{
	u32 method_id = 0;

	if (!(interface->capability & cap))
		return AE_BAD_PARAMETER;

	switch (cap) {
	case ACER_CAP_TURBO_LED:
		method_id = ACER_WMID_SET_GAMING_LED_METHODID;
		break;
	case ACER_CAP_TURBO_FAN:
		method_id = ACER_WMID_SET_GAMING_FAN_BEHAVIOR;
		break;
	case ACER_CAP_TURBO_OC:
		method_id = ACER_WMID_SET_GAMING_MISC_SETTING_METHODID;
		break;
	case ACER_CAP_GAMINGKB_STATIC:
		method_id = ACER_WMID_SET_GAMING_STATIC_LED_METHODID;
		return WMI_gaming_execute_u64(method_id, value, NULL);
	default:
		return AE_BAD_PARAMETER;
	}

	return WMI_gaming_execute_u64(method_id, value, NULL);
}

static acpi_status WMID_gaming_set_u8_array(u8 array[], size_t array_size, u32 cap)
{
	u32 method_id = 0;

	switch (cap) {
	case ACER_CAP_GAMINGKB:
		if (array_size != GAMING_KBBL_CONFIG_LEN)
			return AE_BAD_PARAMETER;
		method_id = ACER_WMID_SET_GAMINGKBBL_METHODID;
		break;
	default:
		return AE_ERROR;
	}
	return WMI_gaming_execute_u8_array(method_id, array, array_size, NULL);
}

static acpi_status WMID_gaming_get_u64(u64 *value, u32 cap)
{
	acpi_status status;
	u64 result;
	u64 input;
	u32 method_id;

	if (!(interface->capability & cap))
		return AE_BAD_PARAMETER;

	switch (cap) {
	case ACER_CAP_TURBO_LED:
		method_id = ACER_WMID_GET_GAMING_LED_METHODID;
		input = 0x1;
		break;
	default:
		return AE_BAD_PARAMETER;
	}
	status = WMI_gaming_execute_u64(method_id, input, &result);
	if (ACPI_SUCCESS(status))
		*value = (u64) result;

	return status;
}

static void WMID_gaming_set_fan_mode(u8 fan_mode)
{
	/* fan_mode = 1 is used for auto, fan_mode = 2 used for turbo*/
	u64 gpu_fan_config1 = 0, gpu_fan_config2 = 0;
	int i;

	if (quirks->cpu_fans > 0)
		gpu_fan_config2 |= 1;
	for (i = 0; i < (quirks->cpu_fans + quirks->gpu_fans); ++i)
		gpu_fan_config2 |= 1 << (i + 1);
	for (i = 0; i < quirks->gpu_fans; ++i)
		gpu_fan_config2 |= 1 << (i + 3);
	if (quirks->cpu_fans > 0)
		gpu_fan_config1 |= fan_mode;
	for (i = 0; i < (quirks->cpu_fans + quirks->gpu_fans); ++i)
		gpu_fan_config1 |= fan_mode << (2 * i + 2);
	for (i = 0; i < quirks->gpu_fans; ++i)
		gpu_fan_config1 |= fan_mode << (2 * i + 6);
	WMID_gaming_set_u64(gpu_fan_config2 | gpu_fan_config1 << 16, ACER_CAP_TURBO_FAN);
}
 
static int WMID_gaming_set_misc_setting(enum acer_wmi_gaming_misc_setting setting, u8 value)
{
	acpi_status status;
	u64 input = 0;
	u64 result;

	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK, setting);
	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_VALUE_MASK, value);

	status = WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_MISC_SETTING_METHODID, input, &result);
	if (ACPI_FAILURE(status))
		return -EIO;

	/* The return status must be zero for the operation to have succeeded */
	if (FIELD_GET(ACER_GAMING_MISC_SETTING_STATUS_MASK, result))
		return -EIO;

	return 0;
}

#if RTLNX_VER_MIN(6, 14, 0)
static int WMID_gaming_get_misc_setting(enum acer_wmi_gaming_misc_setting setting, u8 *value)
{
	u64 input = 0;
	u64 result;
	int ret;

	input |= FIELD_PREP(ACER_GAMING_MISC_SETTING_INDEX_MASK, setting);

	ret = WMI_gaming_execute_u32_u64(ACER_WMID_GET_GAMING_MISC_SETTING_METHODID, input,
					 &result);
	if (ret < 0)
		return ret;

	/* The return status must be zero for the operation to have succeeded */
	if (FIELD_GET(ACER_GAMING_MISC_SETTING_STATUS_MASK, result))
		return -EIO;

	*value = FIELD_GET(ACER_GAMING_MISC_SETTING_VALUE_MASK, result);

	return 0;
}
#endif
/*
 * Generic Device (interface-independent)
 */

static acpi_status get_u32(u32 *value, u32 cap)
{
	acpi_status status = AE_ERROR;

	switch (interface->type) {
	case ACER_AMW0:
		status = AMW0_get_u32(value, cap);
		break;
	case ACER_AMW0_V2:
		if (cap == ACER_CAP_MAILLED) {
			status = AMW0_get_u32(value, cap);
			break;
		}
		fallthrough;
	case ACER_WMID:
		status = WMID_get_u32(value, cap);
		break;
	case ACER_WMID_v2:
		if (cap & (ACER_CAP_WIRELESS |
			   ACER_CAP_BLUETOOTH |
			   ACER_CAP_THREEG))
			status = wmid_v2_get_u32(value, cap);
		else if (wmi_has_guid(WMID_GUID2))
			status = WMID_get_u32(value, cap);
		break;
	}

	return status;
}

static acpi_status set_u32(u32 value, u32 cap)
{
	acpi_status status;

	if (interface->capability & cap) {
		switch (interface->type) {
		case ACER_AMW0:
			return AMW0_set_u32(value, cap);
		case ACER_AMW0_V2:
			if (cap == ACER_CAP_MAILLED)
				return AMW0_set_u32(value, cap);

			/*
			 * On some models, some WMID methods don't toggle
			 * properly. For those cases, we want to run the AMW0
			 * method afterwards to be certain we've really toggled
			 * the device state.
			 */
			if (cap == ACER_CAP_WIRELESS ||
				cap == ACER_CAP_BLUETOOTH) {
				status = WMID_set_u32(value, cap);
				if (ACPI_FAILURE(status))
					return status;

				return AMW0_set_u32(value, cap);
			}
			fallthrough;
		case ACER_WMID:
			return WMID_set_u32(value, cap);
		case ACER_WMID_v2:
			if (cap & (ACER_CAP_WIRELESS |
				   ACER_CAP_BLUETOOTH |
				   ACER_CAP_THREEG))
				return wmid_v2_set_u32(value, cap);
			else if (wmi_has_guid(WMID_GUID2))
				return WMID_set_u32(value, cap);
			fallthrough;
		default:
			return AE_BAD_PARAMETER;
		}
	}
	return AE_BAD_PARAMETER;
}

static acpi_status set_u8_array(u8 array[], size_t array_size, u32 cap)
{
	acpi_status status;

	if (interface->capability & cap) {
		switch (interface->type) {
		default:
			return AE_BAD_PARAMETER;
		}
	} else if (gaming_interface->capability & cap) {
		switch (gaming_interface->type) {
		case ACER_WMID_GAMING:
			status = WMID_gaming_set_u8_array(array, array_size, cap);
			if (ACPI_FAILURE(status))
				return status;
			fallthrough;
		default:
			return AE_BAD_PARAMETER;
		}
	}
	return AE_BAD_PARAMETER;
}

static void __init acer_commandline_init(void)
{
	/*
	 * These will all fail silently if the value given is invalid, or the
	 * capability isn't available on the given interface
	 */
	if (mailled >= 0)
		set_u32(mailled, ACER_CAP_MAILLED);
	if (!has_type_aa && threeg >= 0)
		set_u32(threeg, ACER_CAP_THREEG);
	if (brightness >= 0)
		set_u32(brightness, ACER_CAP_BRIGHTNESS);
}

/*
 * LED device (Mail LED only, no other LEDs known yet)
 */
static void mail_led_set(struct led_classdev *led_cdev,
enum led_brightness value)
{
	set_u32(value, ACER_CAP_MAILLED);
}

static struct led_classdev mail_led = {
	.name = "acer-wmi::mail",
	.brightness_set = mail_led_set,
};

static int acer_led_init(struct device *dev)
{
	return led_classdev_register(dev, &mail_led);
}

static void acer_led_exit(void)
{
	set_u32(LED_OFF, ACER_CAP_MAILLED);
	led_classdev_unregister(&mail_led);
}

/*
 * Keyboard RGB backlight character device handler.
 * On systems supporting Acer gaming functions, a char device
 * will be exposed to communicate with user space
 * for keyboard RGB backlight configurations.
 */

static ssize_t gkbbl_drv_write(struct file *file,
		const char __user *buf, size_t count, loff_t *offset)
{
	u8 config_buf[GAMING_KBBL_CONFIG_LEN];
	unsigned long err;

	if (count != GAMING_KBBL_CONFIG_LEN) {
		pr_err("Invalid data given to gaming keyboard backlight");
		return 0;
	}
	err = copy_from_user(config_buf, buf, GAMING_KBBL_CONFIG_LEN);
	if (err < 0)
		pr_err("Copying data from userspace failed with code: %lu\n", err);

	set_u8_array(config_buf, GAMING_KBBL_CONFIG_LEN, ACER_CAP_GAMINGKB);
	return count;
}


static const struct file_operations gkbbl_dev_fops = {
		.owner      = THIS_MODULE,
		.write       = gkbbl_drv_write
};

struct gkbbl_device_data {
	struct cdev cdev;
};

static struct class *gkbbl_dev_class;
static struct gkbbl_device_data gkbbl_dev_data;

static int gkbbl_dev_uevent(
#if RTLNX_VER_MIN(6, 2, 0)
const
#endif
struct device *dev, struct kobj_uevent_env *env)
{
	add_uevent_var(env, "DEVMODE=%#o", 0666);
	return 0;
}

static int __init gaming_kbbl_cdev_init(void)
{
	dev_t dev;
	int err;

	err = alloc_chrdev_region(&dev, 0, 1, GAMING_KBBL_CHR);
	if (err < 0) {
		pr_err("Char drive registering for gaming keyboard backlight failed: %d\n", err);
		return err;
	}

	gkbbl_dynamic_dev = dev;

	#if RTLNX_VER_MIN(6, 4, 0)
		gkbbl_dev_class = class_create(GAMING_KBBL_CHR);
	#else
		gkbbl_dev_class = class_create(THIS_MODULE, GAMING_KBBL_CHR);
	#endif

	gkbbl_dev_class->dev_uevent = gkbbl_dev_uevent;

	cdev_init(&gkbbl_dev_data.cdev, &gkbbl_dev_fops);
	gkbbl_dev_data.cdev.owner = THIS_MODULE;

	cdev_add(&gkbbl_dev_data.cdev, gkbbl_dynamic_dev, 1);

	device_create(gkbbl_dev_class, NULL, gkbbl_dynamic_dev, NULL, "%s-%d",
			   GAMING_KBBL_CHR,
			   GAMING_KBBL_MINOR);

	return 0;
}

static void __exit gaming_kbbl_cdev_exit(void)
{
	device_destroy(gkbbl_dev_class, gkbbl_dynamic_dev);

	class_unregister(gkbbl_dev_class);
	class_destroy(gkbbl_dev_class);
	cdev_del(&gkbbl_dev_data.cdev);

	unregister_chrdev_region(gkbbl_dynamic_dev, 1);
}


/*
 * Keyboard RGB backlight character device handler.
 * On systems supporting Acer gaming functions, a char device
 * will be exposed to communicate with user space
 * for keyboard RGB backlight configurations.
 * Similar to above, but for handling static coloring
 */


struct led_zone_set_param {
		u8 zone;
		u8 red;
		u8 green;
		u8 blue;
} __packed;

static ssize_t gkbbl_static_drv_write(struct file *file, const char __user *buf, size_t count, loff_t *offset)
{
	u8 config_buf[4]={0,0,0,0};
	unsigned long err;
	struct led_zone_set_param set_params;
	struct acpi_buffer set_input;
	err = copy_from_user(config_buf, buf, GAMING_KBBL_STATIC_CONFIG_LEN);
	set_params = (struct led_zone_set_param) {
		.zone = config_buf[0],
		.red = config_buf[1],
		.green = config_buf[2],
		.blue = config_buf[3],
	};
	set_input = (struct acpi_buffer) {
		sizeof(set_params),
		&set_params
	};

	if (count != GAMING_KBBL_STATIC_CONFIG_LEN) {
		pr_err("Invalid data given to gaming keyboard static backlight");
		return 0;
	}

	if (err < 0)
		pr_err("Copying data from userspace failed with code: %lu\n", err);

	wmi_evaluate_method( WMID_GUID4, 0, ACER_WMID_SET_GAMING_STATIC_LED_METHODID, &set_input, NULL);
	return count;
}


static const struct file_operations gkbbl_static_dev_fops = {
		.owner      = THIS_MODULE,
		.write       = gkbbl_static_drv_write
};

static struct class *gkbbl_static_dev_class;
static struct gkbbl_device_data gkbbl_static_dev_data;

static int gkbbl_static_dev_uevent(
#if RTLNX_VER_MIN(6, 2, 0)
const
#endif
struct device *dev, struct kobj_uevent_env *env)
{
	add_uevent_var(env, "DEVMODE=%#o", 0666);
	return 0;
}

static int __init gaming_kbbl_static_cdev_init(void)
{
	dev_t dev;
	int err;

	err = alloc_chrdev_region(&dev, 0, 1, GAMING_KBBL_STATIC_CHR);
	if (err < 0) {
		pr_err("Char drive registering for gaming keyboard static backlight failed: %d\n", err);
		return err;
	}

	gkbbl_static_dev = dev;

	#if RTLNX_VER_MIN(6, 4, 0)
		gkbbl_static_dev_class = class_create(GAMING_KBBL_STATIC_CHR);
	#else
		gkbbl_static_dev_class = class_create(THIS_MODULE, GAMING_KBBL_STATIC_CHR);
	#endif

	gkbbl_static_dev_class->dev_uevent = gkbbl_static_dev_uevent;

	cdev_init(&gkbbl_static_dev_data.cdev, &gkbbl_static_dev_fops);
	gkbbl_static_dev_data.cdev.owner = THIS_MODULE;

	cdev_add(&gkbbl_static_dev_data.cdev, gkbbl_static_dev, 1);

	device_create(gkbbl_static_dev_class, NULL, gkbbl_static_dev, NULL, "%s-%d",
				  GAMING_KBBL_STATIC_CHR,
				  GAMING_KBBL_STATIC_MINOR);

	return 0;
}

static int __init gaming_kbbl_poll_and_enable_zones(void)
{
	u64 gaming_sysinfo;
	/*
	 * Querying GetGamingSysInfo appears to be required to enable Nitro AN515-57
	 * and possibly other Acer (Predator/Nitro) 4 zone LED keyboards.
	 */
	WMI_gaming_execute_u64(ACER_WMID_GET_GAMING_SYS_INFO_METHODID, 0, &gaming_sysinfo);
	/* Turn on all 4 zones */
	WMI_gaming_execute_u64(ACER_WMID_SET_GAMING_LED_METHODID, 8L | (15UL<<40), NULL);
	return 0;
}

static void __exit gaming_kbbl_static_cdev_exit(void)
{
	device_destroy(gkbbl_static_dev_class, gkbbl_static_dev);

	class_unregister(gkbbl_static_dev_class);
	class_destroy(gkbbl_static_dev_class);
	cdev_del(&gkbbl_static_dev_data.cdev);

	unregister_chrdev_region(gkbbl_static_dev, 1);
}

/*
 * Backlight device
 */
static struct backlight_device *acer_backlight_device;

static int read_brightness(struct backlight_device *bd)
{
	u32 value;
	get_u32(&value, ACER_CAP_BRIGHTNESS);
	return value;
}

static int update_bl_status(struct backlight_device *bd)
{
	int intensity = backlight_get_brightness(bd);

	set_u32(intensity, ACER_CAP_BRIGHTNESS);

	return 0;
}

static const struct backlight_ops acer_bl_ops = {
	.get_brightness = read_brightness,
	.update_status = update_bl_status,
};

static int acer_backlight_init(struct device *dev)
{
	struct backlight_properties props;
	struct backlight_device *bd;

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_PLATFORM;
	props.max_brightness = max_brightness;
	bd = backlight_device_register("acer-wmi", dev, NULL, &acer_bl_ops,
				       &props);
	if (IS_ERR(bd)) {
		pr_err("Could not register Acer backlight device\n");
		acer_backlight_device = NULL;
		return PTR_ERR(bd);
	}

	acer_backlight_device = bd;

	bd->props.power = FB_BLANK_UNBLANK;
	bd->props.brightness = read_brightness(bd);
	backlight_update_status(bd);
	return 0;
}

static void acer_backlight_exit(void)
{
	backlight_device_unregister(acer_backlight_device);
}

/*
 * Accelerometer device
 */
static acpi_handle gsensor_handle;

static int acer_gsensor_init(void)
{
	acpi_status status;
	struct acpi_buffer output;
	union acpi_object out_obj;

	output.length = sizeof(out_obj);
	output.pointer = &out_obj;
	status = acpi_evaluate_object(gsensor_handle, "_INI", NULL, &output);
	if (ACPI_FAILURE(status))
		return -1;

	return 0;
}

static int acer_gsensor_open(struct input_dev *input)
{
	return acer_gsensor_init();
}

static int acer_gsensor_event(void)
{
	acpi_status status;
	struct acpi_buffer output;
	union acpi_object out_obj[5];

	if (!acer_wmi_accel_dev)
		return -1;

	output.length = sizeof(out_obj);
	output.pointer = out_obj;

	status = acpi_evaluate_object(gsensor_handle, "RDVL", NULL, &output);
	if (ACPI_FAILURE(status))
		return -1;

	if (out_obj->package.count != 4)
		return -1;

	input_report_abs(acer_wmi_accel_dev, ABS_X,
		(s16)out_obj->package.elements[0].integer.value);
	input_report_abs(acer_wmi_accel_dev, ABS_Y,
		(s16)out_obj->package.elements[1].integer.value);
	input_report_abs(acer_wmi_accel_dev, ABS_Z,
		(s16)out_obj->package.elements[2].integer.value);
	input_sync(acer_wmi_accel_dev);
	return 0;
}

static int acer_get_fan_speed(int fan)
{
	if (quirks->predator_v4) {
		acpi_status status;
		u64 fanspeed;

		status = WMI_gaming_execute_u64(
			ACER_WMID_GET_GAMING_SYS_INFO_METHODID,
			fan == 0 ? ACER_WMID_CMD_GET_PREDATOR_V4_CPU_FAN_SPEED :
				   ACER_WMID_CMD_GET_PREDATOR_V4_GPU_FAN_SPEED,
			&fanspeed);

		if (ACPI_FAILURE(status))
			return -EIO;

		return FIELD_GET(ACER_PREDATOR_V4_FAN_SPEED_READ_BIT_MASK, fanspeed);
	}
	return -EOPNOTSUPP;
}

/*
 *  Predator series turbo button
 */
static void acer_toggle_turbo(void)
{
	if (turbo_state) {
		turbo_state = 0;
		/* Turn off turbo led */
		WMID_gaming_set_u64(0x1, ACER_CAP_TURBO_LED);

		/* Set FAN mode to auto */
		WMID_gaming_set_fan_mode(0x1);

		/* Set OC to normal */
		WMID_gaming_set_u64(0x5, ACER_CAP_TURBO_OC);
		WMID_gaming_set_u64(0x7, ACER_CAP_TURBO_OC);
	} else {
		turbo_state = 1;
		/* Turn on turbo led */
		WMID_gaming_set_u64(0x10001, ACER_CAP_TURBO_LED);

		/* Set FAN mode to turbo */
		WMID_gaming_set_fan_mode(0x2);

		/* Set OC to turbo mode */
		WMID_gaming_set_u64(0x205, ACER_CAP_TURBO_OC);
		WMID_gaming_set_u64(0x207, ACER_CAP_TURBO_OC);
	}
}

#if RTLNX_VER_MIN(6, 14, 0)
static int
acer_predator_v4_platform_profile_get(struct device *dev,
				    	enum platform_profile_option *profile)
#else
static int
acer_predator_v4_platform_profile_get(struct platform_profile_handler *pprof,
						enum platform_profile_option *profile)
#endif
{
	u8 tp;
	int err;

#if RTLNX_VER_MIN(6, 14, 0)
	err = WMID_gaming_get_misc_setting(ACER_WMID_MISC_SETTING_PLATFORM_PROFILE, &tp);
	if (err)
		return err;
#else
	err = ec_read(ACER_PREDATOR_V4_THERMAL_PROFILE_EC_OFFSET, &tp);

	if (err < 0)
		return err;
#endif

	switch (tp) {
	case ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE:
		*profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
		break;
	case ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET:
		*profile = PLATFORM_PROFILE_QUIET;
		break;
	case ACER_PREDATOR_V4_THERMAL_PROFILE_ECO:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

#if RTLNX_VER_MIN(6, 14, 0)
static int
acer_predator_v4_platform_profile_set(struct device *dev,
				      enum platform_profile_option profile)
#else
static int
acer_predator_v4_platform_profile_set(struct platform_profile_handler *pprof,
				      enum platform_profile_option profile)
#endif
{
	int err ,tp;

	switch (profile) {
	case PLATFORM_PROFILE_PERFORMANCE:
		tp = ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO;
		break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		tp = ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_BALANCED:
		tp = ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
		break;
	case PLATFORM_PROFILE_QUIET:
		tp = ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET;
		break;
	case PLATFORM_PROFILE_LOW_POWER:
		tp = ACER_PREDATOR_V4_THERMAL_PROFILE_ECO;
		break;
	default:
		return -EOPNOTSUPP;
	}

	#if RTLNX_VER_MIN(6, 14, 0)
	err = WMID_gaming_set_misc_setting(ACER_WMID_MISC_SETTING_PLATFORM_PROFILE, tp);
	if (err)
		return err;
	#else
	acpi_status status;
	status = WMI_gaming_execute_u64(
		ACER_WMID_SET_GAMING_MISC_SETTING_METHODID, tp, NULL);

	if (ACPI_FAILURE(status))
		return -EIO;
	#endif

	if (tp != acer_predator_v4_max_perf)
		last_non_turbo_profile = tp;

	return 0;
}

#if RTLNX_VER_MIN(6, 14, 0)
static int
acer_predator_v4_platform_profile_probe(void *drvdata, unsigned long *choices)
{
	unsigned long supported_profiles;
	int err;

	err = WMID_gaming_get_misc_setting(ACER_WMID_MISC_SETTING_SUPPORTED_PROFILES,
					   (u8 *)&supported_profiles);
	if (err)
		return err;

	/* Iterate through supported profiles in order of increasing performance */
	if (test_bit(ACER_PREDATOR_V4_THERMAL_PROFILE_ECO, &supported_profiles)) {
		set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
		acer_predator_v4_max_perf = ACER_PREDATOR_V4_THERMAL_PROFILE_ECO;
		last_non_turbo_profile = ACER_PREDATOR_V4_THERMAL_PROFILE_ECO;
	}

	if (test_bit(ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET, &supported_profiles)) {
		set_bit(PLATFORM_PROFILE_QUIET, choices);
		acer_predator_v4_max_perf = ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET;
		last_non_turbo_profile = ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET;
	}

	if (test_bit(ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED, &supported_profiles)) {
		set_bit(PLATFORM_PROFILE_BALANCED, choices);
		acer_predator_v4_max_perf = ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
		last_non_turbo_profile = ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED;
	}

	if (test_bit(ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE, &supported_profiles)) {
		set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);
		acer_predator_v4_max_perf = ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE;

		/* We only use this profile as a fallback option in case no prior
		 * profile is supported.
		 */
		if (last_non_turbo_profile < 0)
			last_non_turbo_profile = ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE;
	}

	if (test_bit(ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO, &supported_profiles)) {
		set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
		acer_predator_v4_max_perf = ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO;

		/* We need to handle the hypothetical case where only the turbo profile
		 * is supported. In this case the turbo toggle will essentially be a
		 * no-op.
		 */
		if (last_non_turbo_profile < 0)
			last_non_turbo_profile = ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO;
	}

	return 0;
}

static const struct platform_profile_ops acer_predator_v4_platform_profile_ops = {
	.probe = acer_predator_v4_platform_profile_probe,
	.profile_get = acer_predator_v4_platform_profile_get,
	.profile_set = acer_predator_v4_platform_profile_set,
};

static int acer_platform_profile_setup(struct platform_device *device)
{
	if (quirks->predator_v4) {
		platform_profile_device = devm_platform_profile_register(
			&device->dev, "acer-wmi", NULL, &acer_predator_v4_platform_profile_ops);
		if (IS_ERR(platform_profile_device))
			return PTR_ERR(platform_profile_device);
	
		platform_profile_support = true;

		/* Set default non-turbo profile  */
		last_non_turbo_profile =
			ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED_WMI;
	}
	return 0;
}

#else
static int acer_platform_profile_setup(void)
{
	if (quirks->predator_v4) {
		int err;

		platform_profile_handler.profile_get =
			acer_predator_v4_platform_profile_get;
		platform_profile_handler.profile_set =
			acer_predator_v4_platform_profile_set;

		set_bit(PLATFORM_PROFILE_PERFORMANCE,
			platform_profile_handler.choices);
		set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE,
			platform_profile_handler.choices);
		set_bit(PLATFORM_PROFILE_BALANCED,
			platform_profile_handler.choices);
		set_bit(PLATFORM_PROFILE_QUIET,
			platform_profile_handler.choices);
		set_bit(PLATFORM_PROFILE_LOW_POWER,
			platform_profile_handler.choices);

		err = platform_profile_register(&platform_profile_handler);
		if (err)
			return err;

		platform_profile_support = true;

		/* Set default non-turbo profile  */
		last_non_turbo_profile =
			ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED_WMI;
	}
	return 0;
}
#endif

static int acer_thermal_profile_change(void)
{
	/*
	 * This mode key can rotate each mode or toggle turbo mode.
	 * On battery, only ECO and BALANCED mode are available.
	 */
	if (quirks->predator_v4) {
		u8 current_tp;
		int tp, err;
		u64 on_AC;
		acpi_status status;

		err = ec_read(ACER_PREDATOR_V4_THERMAL_PROFILE_EC_OFFSET,
			      &current_tp);

		if (err < 0)
			return err;

		/* Check power source */
		status = WMI_gaming_execute_u64(
			ACER_WMID_GET_GAMING_SYS_INFO_METHODID,
			ACER_WMID_CMD_GET_PREDATOR_V4_BAT_STATUS, &on_AC);

		if (ACPI_FAILURE(status))
			return -EIO;

		switch (current_tp) {
		case ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO:
			if (!on_AC)
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED_WMI;
			else if (cycle_gaming_thermal_profile)
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_ECO_WMI;
			else
				tp = last_non_turbo_profile;
			break;
		case ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE:
			if (!on_AC)
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED_WMI;
			else
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO_WMI;
			break;
		case ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED:
			if (!on_AC)
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_ECO_WMI;
			else if (cycle_gaming_thermal_profile)
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_PERFORMANCE_WMI;
			else
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO_WMI;
			break;
		case ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET:
			if (!on_AC)
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED_WMI;
			else if (cycle_gaming_thermal_profile)
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED_WMI;
			else
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO_WMI;
			break;
		case ACER_PREDATOR_V4_THERMAL_PROFILE_ECO:
			if (!on_AC)
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_BALANCED_WMI;
			else if (cycle_gaming_thermal_profile)
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_QUIET_WMI;
			else
				tp = ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO_WMI;
			break;
		default:
			return -EOPNOTSUPP;
		}

		status = WMI_gaming_execute_u64(
			ACER_WMID_SET_GAMING_MISC_SETTING_METHODID, tp, NULL);

		if (ACPI_FAILURE(status))
			return -EIO;

		/* Store non-turbo profile for turbo mode toggle*/
		if (tp != ACER_PREDATOR_V4_THERMAL_PROFILE_TURBO_WMI)
			last_non_turbo_profile = tp;
		
		#if RTLNX_VER_MIN(6, 14, 0)
		platform_profile_notify(platform_profile_device);
		#else
		platform_profile_notify();
		#endif
	}

	return 0;
}

/*
 * Switch series keyboard dock status
 */
static int acer_kbd_dock_state_to_sw_tablet_mode(u8 kbd_dock_state)
{
	switch (kbd_dock_state) {
	case 0x01: /* Docked, traditional clamshell laptop mode */
		return 0;
	case 0x04: /* Stand-alone tablet */
	case 0x40: /* Docked, tent mode, keyboard not usable */
		return 1;
	default:
		pr_warn("Unknown kbd_dock_state 0x%02x\n", kbd_dock_state);
	}

	return 0;
}

static void acer_kbd_dock_get_initial_state(void)
{
	u8 *output, input[8] = { 0x05, 0x00, };
	struct acpi_buffer input_buf = { sizeof(input), input };
	struct acpi_buffer output_buf = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int sw_tablet_mode;

	status = wmi_evaluate_method(WMID_GUID3, 0, 0x2, &input_buf, &output_buf);
	if (ACPI_FAILURE(status)) {
		pr_err("Error getting keyboard-dock initial status: %s\n",
		       acpi_format_exception(status));
		return;
	}

	obj = output_buf.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER || obj->buffer.length != 8) {
		pr_err("Unexpected output format getting keyboard-dock initial status\n");
		goto out_free_obj;
	}

	output = obj->buffer.pointer;
	if (output[0] != 0x00 || (output[3] != 0x05 && output[3] != 0x45)) {
		pr_err("Unexpected output [0]=0x%02x [3]=0x%02x getting keyboard-dock initial status\n",
		       output[0], output[3]);
		goto out_free_obj;
	}

	sw_tablet_mode = acer_kbd_dock_state_to_sw_tablet_mode(output[4]);
	input_report_switch(acer_wmi_input_dev, SW_TABLET_MODE, sw_tablet_mode);

out_free_obj:
	kfree(obj);
}

static void acer_kbd_dock_event(const struct event_return_value *event)
{
	int sw_tablet_mode;

	if (!has_cap(ACER_CAP_KBD_DOCK))
		return;

	sw_tablet_mode = acer_kbd_dock_state_to_sw_tablet_mode(event->kbd_dock_state);
	input_report_switch(acer_wmi_input_dev, SW_TABLET_MODE, sw_tablet_mode);
	input_sync(acer_wmi_input_dev);
}

/*
 * Rfkill devices
 */
static void acer_rfkill_update(struct work_struct *ignored);
static DECLARE_DELAYED_WORK(acer_rfkill_work, acer_rfkill_update);
static void acer_rfkill_update(struct work_struct *ignored)
{
	u32 state;
	acpi_status status;

	if (has_cap(ACER_CAP_WIRELESS)) {
		status = get_u32(&state, ACER_CAP_WIRELESS);
		if (ACPI_SUCCESS(status)) {
			if (quirks->wireless == 3)
				rfkill_set_hw_state(wireless_rfkill, !state);
			else
				rfkill_set_sw_state(wireless_rfkill, !state);
		}
	}

	if (has_cap(ACER_CAP_BLUETOOTH)) {
		status = get_u32(&state, ACER_CAP_BLUETOOTH);
		if (ACPI_SUCCESS(status))
			rfkill_set_sw_state(bluetooth_rfkill, !state);
	}

	if (has_cap(ACER_CAP_THREEG) && wmi_has_guid(WMID_GUID3)) {
		status = get_u32(&state, ACER_WMID3_GDS_THREEG);
		if (ACPI_SUCCESS(status))
			rfkill_set_sw_state(threeg_rfkill, !state);
	}

	schedule_delayed_work(&acer_rfkill_work, round_jiffies_relative(HZ));
}

static int acer_rfkill_set(void *data, bool blocked)
{
	acpi_status status;
	u32 cap = (unsigned long)data;

	if (rfkill_inited) {
		status = set_u32(!blocked, cap);
		if (ACPI_FAILURE(status))
			return -ENODEV;
	}

	return 0;
}

static const struct rfkill_ops acer_rfkill_ops = {
	.set_block = acer_rfkill_set,
};

static struct rfkill *acer_rfkill_register(struct device *dev,
					   enum rfkill_type type,
					   char *name, u32 cap)
{
	int err;
	struct rfkill *rfkill_dev;
	u32 state;
	acpi_status status;

	rfkill_dev = rfkill_alloc(name, dev, type,
				  &acer_rfkill_ops,
				  (void *)(unsigned long)cap);
	if (!rfkill_dev)
		return ERR_PTR(-ENOMEM);

	status = get_u32(&state, cap);

	err = rfkill_register(rfkill_dev);
	if (err) {
		rfkill_destroy(rfkill_dev);
		return ERR_PTR(err);
	}

	if (ACPI_SUCCESS(status))
		rfkill_set_sw_state(rfkill_dev, !state);

	return rfkill_dev;
}

static int acer_rfkill_init(struct device *dev)
{
	int err;

	if (has_cap(ACER_CAP_WIRELESS)) {
		wireless_rfkill = acer_rfkill_register(dev, RFKILL_TYPE_WLAN,
			"acer-wireless", ACER_CAP_WIRELESS);
		if (IS_ERR(wireless_rfkill)) {
			err = PTR_ERR(wireless_rfkill);
			goto error_wireless;
		}
	}

	if (has_cap(ACER_CAP_BLUETOOTH)) {
		bluetooth_rfkill = acer_rfkill_register(dev,
			RFKILL_TYPE_BLUETOOTH, "acer-bluetooth",
			ACER_CAP_BLUETOOTH);
		if (IS_ERR(bluetooth_rfkill)) {
			err = PTR_ERR(bluetooth_rfkill);
			goto error_bluetooth;
		}
	}

	if (has_cap(ACER_CAP_THREEG)) {
		threeg_rfkill = acer_rfkill_register(dev,
			RFKILL_TYPE_WWAN, "acer-threeg",
			ACER_CAP_THREEG);
		if (IS_ERR(threeg_rfkill)) {
			err = PTR_ERR(threeg_rfkill);
			goto error_threeg;
		}
	}

	rfkill_inited = true;

	if ((ec_raw_mode || !wmi_has_guid(ACERWMID_EVENT_GUID)) &&
	    has_cap(ACER_CAP_WIRELESS | ACER_CAP_BLUETOOTH | ACER_CAP_THREEG))
		schedule_delayed_work(&acer_rfkill_work,
			round_jiffies_relative(HZ));

	return 0;

error_threeg:
	if (has_cap(ACER_CAP_BLUETOOTH)) {
		rfkill_unregister(bluetooth_rfkill);
		rfkill_destroy(bluetooth_rfkill);
	}
error_bluetooth:
	if (has_cap(ACER_CAP_WIRELESS)) {
		rfkill_unregister(wireless_rfkill);
		rfkill_destroy(wireless_rfkill);
	}
error_wireless:
	return err;
}

static void acer_rfkill_exit(void)
{
	if ((ec_raw_mode || !wmi_has_guid(ACERWMID_EVENT_GUID)) &&
	    has_cap(ACER_CAP_WIRELESS | ACER_CAP_BLUETOOTH | ACER_CAP_THREEG))
		cancel_delayed_work_sync(&acer_rfkill_work);

	if (has_cap(ACER_CAP_WIRELESS)) {
		rfkill_unregister(wireless_rfkill);
		rfkill_destroy(wireless_rfkill);
	}

	if (has_cap(ACER_CAP_BLUETOOTH)) {
		rfkill_unregister(bluetooth_rfkill);
		rfkill_destroy(bluetooth_rfkill);
	}

	if (has_cap(ACER_CAP_THREEG)) {
		rfkill_unregister(threeg_rfkill);
		rfkill_destroy(threeg_rfkill);
	}
}

static void acer_wmi_notify(
#if RTLNX_VER_MIN(6, 12, 0)
	union acpi_object *obj
#else
	u32 value
#endif
	, void *context)
{
	struct event_return_value return_value;
	u16 device_state;
	const struct key_entry *key;
	u32 scancode;

#if RTLNX_VER_MAX(6, 12, 0)
	struct acpi_buffer response = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status = wmi_get_event_data(value, &response);
	if (status != AE_OK) {
		pr_warn("bad event status 0x%x\n", status);
		return;
	}
	union acpi_object *obj = (union acpi_object *)response.pointer;
#endif

	if (!obj)
		return;
	if (obj->type != ACPI_TYPE_BUFFER) {
		pr_warn("Unknown response received %d\n", obj->type);
#if RTLNX_VER_MAX(6, 12, 0)
		kfree(obj);
#endif
		return;
	}
	if (obj->buffer.length != 8) {
		pr_warn("Unknown buffer length %d\n", obj->buffer.length);
#if RTLNX_VER_MAX(6, 12, 0)
		kfree(obj);
#endif
		return;
	}

	return_value = *((struct event_return_value *)obj->buffer.pointer);
#if RTLNX_VER_MAX(6, 12, 0)
	kfree(obj);
#endif

	switch (return_value.function) {
	case WMID_HOTKEY_EVENT:
		device_state = return_value.device_state;
		pr_debug("device state: 0x%x\n", device_state);

		key = sparse_keymap_entry_from_scancode(acer_wmi_input_dev,
							return_value.key_num);
		if (!key) {
			pr_warn("Unknown key number - 0x%x\n",
				return_value.key_num);
		} else {
			scancode = return_value.key_num;
			switch (key->keycode) {
			case KEY_WLAN:
			case KEY_BLUETOOTH:
				if (has_cap(ACER_CAP_WIRELESS))
					rfkill_set_sw_state(wireless_rfkill,
						!(device_state & ACER_WMID3_GDS_WIRELESS));
				if (has_cap(ACER_CAP_THREEG))
					rfkill_set_sw_state(threeg_rfkill,
						!(device_state & ACER_WMID3_GDS_THREEG));
				if (has_cap(ACER_CAP_BLUETOOTH))
					rfkill_set_sw_state(bluetooth_rfkill,
						!(device_state & ACER_WMID3_GDS_BLUETOOTH));
				break;
			case KEY_TOUCHPAD_TOGGLE:
				scancode = (device_state & ACER_WMID3_GDS_TOUCHPAD) ?
						KEY_TOUCHPAD_ON : KEY_TOUCHPAD_OFF;
			}
			sparse_keymap_report_event(acer_wmi_input_dev, scancode, 1, true);
		}
		break;
	case WMID_ACCEL_OR_KBD_DOCK_EVENT:
		acer_gsensor_event();
		acer_kbd_dock_event(&return_value);
		break;
	case WMID_GAMING_TURBO_KEY_EVENT:
		if (return_value.key_num == 0x1) {
			/*
			 * This is the macro toggle key on Acer Predator
			 * laptops (it switches colors and selects which
			 * events are generated by the actual macro keys,
			 * key_num = 0x2)
			 */
			if(return_value.device_state >= 1 && return_value.device_state <= 3)
				macro_key_state = return_value.device_state - 1;
			else
				pr_warn("macro key state %d requested (only values 1 to 3 are known)\n", return_value.device_state);
			break;
		}
		else if (return_value.key_num == 0x2) {
			if(return_value.device_state >= 1 && return_value.device_state <= 5)
				sparse_keymap_report_event(acer_wmi_input_dev, 0xda00 + (macro_key_state<<4) + return_value.device_state-1, 1, true);
			else
				pr_warn("macro key %d pressed (only 1 to 5 are known)\n", return_value.device_state);
			break;
		}
		else if (return_value.key_num == 0x4)
			acer_toggle_turbo();
		else if (return_value.key_num == 0x5 && has_cap(ACER_CAP_PLATFORM_PROFILE))
			acer_thermal_profile_change();
		break;
	default:
		pr_warn("Unknown function number - %d - %d\n",
			return_value.function, return_value.key_num);
		break;
	}
}

static acpi_status __init
wmid3_set_function_mode(struct func_input_params *params,
			struct func_return_value *return_value)
{
	acpi_status status;
	union acpi_object *obj;

	struct acpi_buffer input = { sizeof(struct func_input_params), params };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmi_evaluate_method(WMID_GUID3, 0, 0x1, &input, &output);
	if (ACPI_FAILURE(status))
		return status;

	obj = output.pointer;

	if (!obj)
		return AE_ERROR;
	else if (obj->type != ACPI_TYPE_BUFFER) {
		kfree(obj);
		return AE_ERROR;
	}
	if (obj->buffer.length != 4) {
		pr_warn("Unknown buffer length %d\n", obj->buffer.length);
		kfree(obj);
		return AE_ERROR;
	}

	*return_value = *((struct func_return_value *)obj->buffer.pointer);
	kfree(obj);

	return status;
}

static int __init acer_wmi_enable_ec_raw(void)
{
	struct func_return_value return_value;
	acpi_status status;
	struct func_input_params params = {
		.function_num = 0x1,
		.commun_devices = 0xFFFF,
		.devices = 0xFFFF,
		.app_status = 0x00,		/* Launch Manager Deactive */
		.app_mask = 0x01,
	};

	status = wmid3_set_function_mode(&params, &return_value);

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Enabling EC raw mode failed: 0x%x - 0x%x\n",
			return_value.error_code,
			return_value.ec_return_value);
	else
		pr_info("Enabled EC raw mode\n");

	return status;
}

static int __init acer_wmi_enable_lm(void)
{
	struct func_return_value return_value;
	acpi_status status;
	struct func_input_params params = {
		.function_num = 0x1,
		.commun_devices = 0xFFFF,
		.devices = 0xFFFF,
		.app_status = 0x01,            /* Launch Manager Active */
		.app_mask = 0x01,
	};

	status = wmid3_set_function_mode(&params, &return_value);

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Enabling Launch Manager failed: 0x%x - 0x%x\n",
			return_value.error_code,
			return_value.ec_return_value);

	return status;
}

static int __init acer_wmi_enable_rf_button(void)
{
	struct func_return_value return_value;
	acpi_status status;
	struct func_input_params params = {
		.function_num = 0x1,
		.commun_devices = 0xFFFF,
		.devices = 0xFFFF,
		.app_status = 0x10,            /* RF Button Active */
		.app_mask = 0x10,
	};

	status = wmid3_set_function_mode(&params, &return_value);

	if (return_value.error_code || return_value.ec_return_value)
		pr_warn("Enabling RF Button failed: 0x%x - 0x%x\n",
			return_value.error_code,
			return_value.ec_return_value);

	return status;
}

static int __init acer_wmi_accel_setup(void)
{
	struct acpi_device *adev;
	int err;

	adev = acpi_dev_get_first_match_dev("BST0001", NULL, -1);
	if (!adev)
		return -ENODEV;

	gsensor_handle = acpi_device_handle(adev);
	acpi_dev_put(adev);

	acer_wmi_accel_dev = input_allocate_device();
	if (!acer_wmi_accel_dev)
		return -ENOMEM;

	acer_wmi_accel_dev->open = acer_gsensor_open;

	acer_wmi_accel_dev->name = "Acer BMA150 accelerometer";
	acer_wmi_accel_dev->phys = "wmi/input1";
	acer_wmi_accel_dev->id.bustype = BUS_HOST;
	acer_wmi_accel_dev->evbit[0] = BIT_MASK(EV_ABS);
	input_set_abs_params(acer_wmi_accel_dev, ABS_X, -16384, 16384, 0, 0);
	input_set_abs_params(acer_wmi_accel_dev, ABS_Y, -16384, 16384, 0, 0);
	input_set_abs_params(acer_wmi_accel_dev, ABS_Z, -16384, 16384, 0, 0);

	err = input_register_device(acer_wmi_accel_dev);
	if (err)
		goto err_free_dev;

	return 0;

err_free_dev:
	input_free_device(acer_wmi_accel_dev);
	return err;
}

static int __init acer_wmi_input_setup(void)
{
	acpi_status status;
	int err;

	acer_wmi_input_dev = input_allocate_device();
	if (!acer_wmi_input_dev)
		return -ENOMEM;

	acer_wmi_input_dev->name = "Acer WMI hotkeys";
	acer_wmi_input_dev->phys = "wmi/input0";
	acer_wmi_input_dev->id.bustype = BUS_HOST;

	err = sparse_keymap_setup(acer_wmi_input_dev, acer_wmi_keymap, NULL);
	if (err)
		goto err_free_dev;

	if (has_cap(ACER_CAP_KBD_DOCK))
		input_set_capability(acer_wmi_input_dev, EV_SW, SW_TABLET_MODE);

	status = wmi_install_notify_handler(ACERWMID_EVENT_GUID,
						acer_wmi_notify, NULL);
	if (ACPI_FAILURE(status)) {
		err = -EIO;
		goto err_free_dev;
	}

	if (has_cap(ACER_CAP_KBD_DOCK))
		acer_kbd_dock_get_initial_state();

	err = input_register_device(acer_wmi_input_dev);
	if (err)
		goto err_uninstall_notifier;

	return 0;

err_uninstall_notifier:
	wmi_remove_notify_handler(ACERWMID_EVENT_GUID);
err_free_dev:
	input_free_device(acer_wmi_input_dev);
	return err;
}

static void acer_wmi_input_destroy(void)
{
	wmi_remove_notify_handler(ACERWMID_EVENT_GUID);
	input_unregister_device(acer_wmi_input_dev);
}

/*
 * debugfs functions
 */
static u32 get_wmid_devices(void)
{
	struct acpi_buffer out = {ACPI_ALLOCATE_BUFFER, NULL};
	union acpi_object *obj;
	acpi_status status;
	u32 devices = 0;

	status = wmi_query_block(WMID_GUID2, 0, &out);
	if (ACPI_FAILURE(status))
		return 0;

	obj = (union acpi_object *) out.pointer;
	if (obj) {
		if (obj->type == ACPI_TYPE_BUFFER &&
			(obj->buffer.length == sizeof(u32) ||
			obj->buffer.length == sizeof(u64))) {
			devices = *((u32 *) obj->buffer.pointer);
		} else if (obj->type == ACPI_TYPE_INTEGER) {
			devices = (u32) obj->integer.value;
		}
	}

	kfree(out.pointer);
	return devices;
}

static int acer_wmi_hwmon_init(void);

/*
 * Platform device
 */
static int acer_platform_probe(struct platform_device *device)
{
	int err;

	if (has_cap(ACER_CAP_MAILLED)) {
		err = acer_led_init(&device->dev);
		if (err)
			goto error_mailled;
	}

	if (has_cap(ACER_CAP_BRIGHTNESS)) {
		err = acer_backlight_init(&device->dev);
		if (err)
			goto error_brightness;
	}

	err = acer_rfkill_init(&device->dev);
	if (err)
		goto error_rfkill;

	if (has_cap(ACER_CAP_PLATFORM_PROFILE)) {
		#if RTLNX_VER_MIN(6, 14, 0)
		err = acer_platform_profile_setup(device);
		#else
		err = acer_platform_profile_setup();
		#endif
		if (err)
			goto error_platform_profile;
	}

	if (has_cap(ACER_CAP_FAN_SPEED_READ)) {
		err = acer_wmi_hwmon_init();
		if (err)
			goto error_hwmon;
	}

	return 0;

	error_hwmon:
	error_platform_profile:
		acer_rfkill_exit();
	error_rfkill:
		if (has_cap(ACER_CAP_BRIGHTNESS))
			acer_backlight_exit();
	error_brightness:
		if (has_cap(ACER_CAP_MAILLED))
			acer_led_exit();
	error_mailled:
		return err;
}

static void acer_platform_remove(struct platform_device *device)
{
	if (has_cap(ACER_CAP_MAILLED))
		acer_led_exit();
	if (has_cap(ACER_CAP_BRIGHTNESS))
		acer_backlight_exit();

	acer_rfkill_exit();
}

#ifdef CONFIG_PM_SLEEP
static int acer_suspend(struct device *dev)
{
	u32 value;
	struct acer_data *data = &interface->data;

	if (!data)
		return -ENOMEM;

	if (has_cap(ACER_CAP_MAILLED)) {
		get_u32(&value, ACER_CAP_MAILLED);
		set_u32(LED_OFF, ACER_CAP_MAILLED);
		data->mailled = value;
	}

	if (has_cap(ACER_CAP_BRIGHTNESS)) {
		get_u32(&value, ACER_CAP_BRIGHTNESS);
		data->brightness = value;
	}

	return 0;
}

static int acer_resume(struct device *dev)
{
	struct acer_data *data = &interface->data;

	if (!data)
		return -ENOMEM;

	if (has_cap(ACER_CAP_MAILLED))
		set_u32(data->mailled, ACER_CAP_MAILLED);

	if (has_cap(ACER_CAP_BRIGHTNESS))
		set_u32(data->brightness, ACER_CAP_BRIGHTNESS);

	if (acer_wmi_accel_dev)
		acer_gsensor_init();

	return 0;
}
#else
#define acer_suspend	NULL
#define acer_resume	NULL
#endif

static SIMPLE_DEV_PM_OPS(acer_pm, acer_suspend, acer_resume);

static void acer_platform_shutdown(struct platform_device *device)
{
	struct acer_data *data = &interface->data;

	if (!data)
		return;

	if (has_cap(ACER_CAP_MAILLED))
		set_u32(LED_OFF, ACER_CAP_MAILLED);
}

static struct platform_driver acer_platform_driver = {
		.driver = {
				.name = "acer-wmi",
				.pm = &acer_pm,
		},
		.probe = acer_platform_probe,
		#if RTLNX_VER_MIN(6, 14, 0)
		.remove = acer_platform_remove,
		#else
		.remove = (void*)acer_platform_remove,
		#endif
		.shutdown = acer_platform_shutdown,
};

static struct platform_device *acer_platform_device;

static void remove_debugfs(void)
{
	debugfs_remove_recursive(interface->debug.root);
}

static void __init create_debugfs(void)
{
	interface->debug.root = debugfs_create_dir("acer-wmi", NULL);

	debugfs_create_u32("devices", S_IRUGO, interface->debug.root,
			   &interface->debug.wmid_devices);
}

static umode_t acer_wmi_hwmon_is_visible(const void *data,
					 enum hwmon_sensor_types type, u32 attr,
					 int channel)
{
	switch (type) {
	case hwmon_fan:
		if (acer_get_fan_speed(channel) >= 0)
			return 0444;
		break;
	default:
		return 0;
	}

	return 0;
}

static int acer_wmi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *val)
{
	int ret;

	switch (type) {
	case hwmon_fan:
		ret = acer_get_fan_speed(channel);
		if (ret < 0)
			return ret;
		*val = ret;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct hwmon_channel_info *const acer_wmi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT), NULL
};

static const struct hwmon_ops acer_wmi_hwmon_ops = {
	.read = acer_wmi_hwmon_read,
	.is_visible = acer_wmi_hwmon_is_visible,
};

static const struct hwmon_chip_info acer_wmi_hwmon_chip_info = {
	.ops = &acer_wmi_hwmon_ops,
	.info = acer_wmi_hwmon_info,
};

static int acer_wmi_hwmon_init(void)
{
	struct device *dev = &acer_platform_device->dev;
	struct device *hwmon;

	hwmon = devm_hwmon_device_register_with_info(dev, "acer",
						     &acer_platform_driver,
						     &acer_wmi_hwmon_chip_info,
						     NULL);

	if (IS_ERR(hwmon)) {
		dev_err(dev, "Could not register acer hwmon device\n");
		return PTR_ERR(hwmon);
	}

	return 0;
}

static int __init acer_wmi_init(void)
{
	int err;

	pr_info("Acer Laptop ACPI-WMI Extras\n");

	if (dmi_check_system(acer_blacklist)) {
		pr_info("Blacklisted hardware detected - not loading\n");
		return -ENODEV;
	}

	find_quirks();

	/*
	 * The AMW0_GUID1 wmi is not only found on Acer family but also other
	 * machines like Lenovo, Fujitsu and Medion. In the past days,
	 * acer-wmi driver handled those non-Acer machines by quirks list.
	 * But actually acer-wmi driver was loaded on any machines that have
	 * AMW0_GUID1. This behavior is strange because those machines should
	 * be supported by appropriate wmi drivers. e.g. fujitsu-laptop,
	 * ideapad-laptop. So, here checks the machine that has AMW0_GUID1
	 * should be in Acer/Gateway/Packard Bell white list, or it's already
	 * in the past quirk list.
	 */
	if (wmi_has_guid(AMW0_GUID1) &&
	    !dmi_check_system(amw0_whitelist) &&
	    quirks == &quirk_unknown) {
		pr_debug("Unsupported machine has AMW0_GUID1, unable to load\n");
		return -ENODEV;
	}

	/*
	 * Detect which ACPI-WMI interface we're using.
	 */
	if (wmi_has_guid(AMW0_GUID1) && wmi_has_guid(WMID_GUID1))
		interface = &AMW0_V2_interface;

	if (!wmi_has_guid(AMW0_GUID1) && wmi_has_guid(WMID_GUID1))
		interface = &wmid_interface;

	if (wmi_has_guid(WMID_GUID3)) {
		interface = &wmid_v2_interface;
		if (wmi_has_guid(WMID_GUID4))
			gaming_interface = &wmid_gaming_interface;
	}

	if (interface)
		dmi_walk(type_aa_dmi_decode, NULL);

	if (wmi_has_guid(WMID_GUID2) && interface) {
		if (!has_type_aa && ACPI_FAILURE(WMID_set_capabilities())) {
			pr_err("Unable to detect available WMID devices\n");
			return -ENODEV;
		}
		/* WMID always provides brightness methods */
		interface->capability |= ACER_CAP_BRIGHTNESS;
	} else if (!wmi_has_guid(WMID_GUID2) && interface && !has_type_aa && force_caps == -1) {
		pr_err("No WMID device detection method found\n");
		return -ENODEV;
	}

	if (wmi_has_guid(AMW0_GUID1) && !wmi_has_guid(WMID_GUID1)) {
		interface = &AMW0_interface;

		if (ACPI_FAILURE(AMW0_set_capabilities())) {
			pr_err("Unable to detect available AMW0 devices\n");
			return -ENODEV;
		}
	}

	if (wmi_has_guid(AMW0_GUID1))
		AMW0_find_mailled();

	if (!interface) {
		pr_err("No or unsupported WMI interface, unable to load\n");
		return -ENODEV;
	}

	set_quirks();

	if (acpi_video_get_backlight_type() != acpi_backlight_vendor)
		interface->capability &= ~ACER_CAP_BRIGHTNESS;

	if (wmi_has_guid(WMID_GUID3)) {
		interface->capability |= ACER_CAP_SET_FUNCTION_MODE;
		if (wmi_has_guid(WMID_GUID4)) {
			gaming_interface->capability |= ACER_CAP_GAMINGKB | ACER_CAP_GAMINGKB_STATIC;
			gaming_kbbl_cdev_init();
			gaming_kbbl_static_cdev_init();
			gaming_kbbl_poll_and_enable_zones();
		}
	}

	if (force_caps != -1)
		interface->capability = force_caps;

	if (wmi_has_guid(WMID_GUID3) &&
	    (interface->capability & ACER_CAP_SET_FUNCTION_MODE)) {
		if (ACPI_FAILURE(acer_wmi_enable_rf_button()))
			pr_warn("Cannot enable RF Button Driver\n");

		if (ec_raw_mode) {
			if (ACPI_FAILURE(acer_wmi_enable_ec_raw())) {
				pr_err("Cannot enable EC raw mode\n");
				return -ENODEV;
			}
		} else if (ACPI_FAILURE(acer_wmi_enable_lm())) {
			pr_err("Cannot enable Launch Manager mode\n");
			return -ENODEV;
		}
	} else if (ec_raw_mode) {
		pr_info("No WMID EC raw mode enable method\n");
	}

	if (wmi_has_guid(ACERWMID_EVENT_GUID)) {
		err = acer_wmi_input_setup();
		if (err)
			return err;
		err = acer_wmi_accel_setup();
		if (err && err != -ENODEV)
			pr_warn("Cannot enable accelerometer\n");
	}

	err = platform_driver_register(&acer_platform_driver);
	if (err) {
		pr_err("Unable to register platform driver\n");
		goto error_platform_register;
	}

	acer_platform_device = platform_device_alloc("acer-wmi", PLATFORM_DEVID_NONE);
	if (!acer_platform_device) {
		err = -ENOMEM;
		goto error_device_alloc;
	}

	err = platform_device_add(acer_platform_device);
	if (err)
		goto error_device_add;

	if (wmi_has_guid(WMID_GUID2)) {
		interface->debug.wmid_devices = get_wmid_devices();
		create_debugfs();
	}

	/* Override any initial settings with values from the commandline */
	acer_commandline_init();

	return 0;

error_device_add:
	platform_device_put(acer_platform_device);
error_device_alloc:
	platform_driver_unregister(&acer_platform_driver);
error_platform_register:
	if (wmi_has_guid(ACERWMID_EVENT_GUID))
		acer_wmi_input_destroy();
	if (acer_wmi_accel_dev)
		input_unregister_device(acer_wmi_accel_dev);

	return err;
}

static void __exit acer_wmi_exit(void)
{
	if (wmi_has_guid(ACERWMID_EVENT_GUID))
		acer_wmi_input_destroy();

	if (acer_wmi_accel_dev)
		input_unregister_device(acer_wmi_accel_dev);

	if (wmi_has_guid(WMID_GUID4)) {
		gaming_kbbl_cdev_exit();
		gaming_kbbl_static_cdev_exit();
	}

	remove_debugfs();
	platform_device_unregister(acer_platform_device);
	platform_driver_unregister(&acer_platform_driver);

	pr_info("Acer Laptop WMI Extras unloaded\n");
}

module_init(acer_wmi_init);
module_exit(acer_wmi_exit);
