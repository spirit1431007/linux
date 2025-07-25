// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux driver for WMI sensor information on Dell notebooks.
 *
 * Copyright (C) 2022 Armin Wolf <W_Armin@gmx.de>
 */

#define pr_format(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/device/driver.h>
#include <linux/dev_printk.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/hwmon.h>
#include <linux/kstrtox.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/limits.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/wmi.h>

#include <acpi/battery.h>

#include <linux/unaligned.h>

#define DRIVER_NAME	"dell-wmi-ddv"

#define DELL_DDV_SUPPORTED_VERSION_MIN	2
#define DELL_DDV_SUPPORTED_VERSION_MAX	3
#define DELL_DDV_GUID	"8A42EA14-4F2A-FD45-6422-0087F7A7E608"

/* Battery indices 1, 2 and 3 */
#define DELL_DDV_NUM_BATTERIES		3

#define SBS_MANUFACTURE_YEAR_MASK	GENMASK(15, 9)
#define SBS_MANUFACTURE_MONTH_MASK	GENMASK(8, 5)
#define SBS_MANUFACTURE_DAY_MASK	GENMASK(4, 0)

#define MA_FAILURE_MODE_MASK			GENMASK(11, 8)
#define MA_FAILURE_MODE_PERMANENT		0x9
#define MA_FAILURE_MODE_OVERHEAT		0xA
#define MA_FAILURE_MODE_OVERCURRENT		0xB

#define MA_PERMANENT_FAILURE_CODE_MASK		GENMASK(13, 12)
#define MA_PERMANENT_FAILURE_FUSE_BLOWN		0x0
#define MA_PERMANENT_FAILURE_CELL_IMBALANCE	0x1
#define MA_PERMANENT_FAILURE_OVERVOLTAGE	0x2
#define MA_PERMANENT_FAILURE_FET_FAILURE	0x3

#define MA_OVERHEAT_FAILURE_CODE_MASK		GENMASK(15, 12)
#define MA_OVERHEAT_FAILURE_START		0x5
#define MA_OVERHEAT_FAILURE_CHARGING		0x7
#define MA_OVERHEAT_FAILURE_DISCHARGING		0x8

#define MA_OVERCURRENT_FAILURE_CODE_MASK	GENMASK(15, 12)
#define MA_OVERCURRENT_FAILURE_CHARGING		0x6
#define MA_OVERCURRENT_FAILURE_DISCHARGING	0xB

#define DELL_EPPID_LENGTH	20
#define DELL_EPPID_EXT_LENGTH	23

static bool force;
module_param_unsafe(force, bool, 0);
MODULE_PARM_DESC(force, "Force loading without checking for supported WMI interface versions");

enum dell_ddv_method {
	DELL_DDV_BATTERY_DESIGN_CAPACITY	= 0x01,
	DELL_DDV_BATTERY_FULL_CHARGE_CAPACITY	= 0x02,
	DELL_DDV_BATTERY_MANUFACTURE_NAME	= 0x03,
	DELL_DDV_BATTERY_MANUFACTURE_DATE	= 0x04,
	DELL_DDV_BATTERY_SERIAL_NUMBER		= 0x05,
	DELL_DDV_BATTERY_CHEMISTRY_VALUE	= 0x06,
	DELL_DDV_BATTERY_TEMPERATURE		= 0x07,
	DELL_DDV_BATTERY_CURRENT		= 0x08,
	DELL_DDV_BATTERY_VOLTAGE		= 0x09,
	DELL_DDV_BATTERY_MANUFACTURER_ACCESS	= 0x0A,
	DELL_DDV_BATTERY_RELATIVE_CHARGE_STATE	= 0x0B,
	DELL_DDV_BATTERY_CYCLE_COUNT		= 0x0C,
	DELL_DDV_BATTERY_EPPID			= 0x0D,
	DELL_DDV_BATTERY_RAW_ANALYTICS_START	= 0x0E,
	DELL_DDV_BATTERY_RAW_ANALYTICS		= 0x0F,
	DELL_DDV_BATTERY_DESIGN_VOLTAGE		= 0x10,
	DELL_DDV_BATTERY_RAW_ANALYTICS_A_BLOCK	= 0x11, /* version 3 */

	DELL_DDV_INTERFACE_VERSION		= 0x12,

	DELL_DDV_FAN_SENSOR_INFORMATION		= 0x20,
	DELL_DDV_THERMAL_SENSOR_INFORMATION	= 0x22,
};

struct fan_sensor_entry {
	u8 type;
	__le16 rpm;
} __packed;

struct thermal_sensor_entry {
	u8 type;
	s8 now;
	s8 min;
	s8 max;
	u8 unknown;
} __packed;

struct combined_channel_info {
	struct hwmon_channel_info info;
	u32 config[];
};

struct combined_chip_info {
	struct hwmon_chip_info chip;
	const struct hwmon_channel_info *info[];
};

struct dell_wmi_ddv_sensors {
	bool active;
	struct mutex lock;	/* protect caching */
	unsigned long timestamp;
	union acpi_object *obj;
	u64 entries;
};

struct dell_wmi_ddv_data {
	struct acpi_battery_hook hook;
	struct device_attribute eppid_attr;
	struct mutex translation_cache_lock;	/* Protects the translation cache */
	struct power_supply *translation_cache[DELL_DDV_NUM_BATTERIES];
	struct dell_wmi_ddv_sensors fans;
	struct dell_wmi_ddv_sensors temps;
	struct wmi_device *wdev;
};

static const char * const fan_labels[] = {
	"CPU Fan",
	"Chassis Motherboard Fan",
	"Video Fan",
	"Power Supply Fan",
	"Chipset Fan",
	"Memory Fan",
	"PCI Fan",
	"HDD Fan",
};

static const char * const fan_dock_labels[] = {
	"Docking Chassis/Motherboard Fan",
	"Docking Video Fan",
	"Docking Power Supply Fan",
	"Docking Chipset Fan",
};

static int dell_wmi_ddv_query_type(struct wmi_device *wdev, enum dell_ddv_method method, u32 arg,
				   union acpi_object **result, acpi_object_type type)
{
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	const struct acpi_buffer in = {
		.length = sizeof(arg),
		.pointer = &arg,
	};
	union acpi_object *obj;
	acpi_status ret;

	ret = wmidev_evaluate_method(wdev, 0x0, method, &in, &out);
	if (ACPI_FAILURE(ret))
		return -EIO;

	obj = out.pointer;
	if (!obj)
		return -ENODATA;

	if (obj->type != type) {
		kfree(obj);
		return -ENOMSG;
	}

	*result = obj;

	return 0;
}

static int dell_wmi_ddv_query_integer(struct wmi_device *wdev, enum dell_ddv_method method,
				      u32 arg, u32 *res)
{
	union acpi_object *obj;
	int ret;

	ret = dell_wmi_ddv_query_type(wdev, method, arg, &obj, ACPI_TYPE_INTEGER);
	if (ret < 0)
		return ret;

	if (obj->integer.value <= U32_MAX)
		*res = (u32)obj->integer.value;
	else
		ret = -ERANGE;

	kfree(obj);

	return ret;
}

static int dell_wmi_ddv_query_buffer(struct wmi_device *wdev, enum dell_ddv_method method,
				     u32 arg, union acpi_object **result)
{
	union acpi_object *obj;
	u64 buffer_size;
	int ret;

	ret = dell_wmi_ddv_query_type(wdev, method, arg, &obj, ACPI_TYPE_PACKAGE);
	if (ret < 0)
		return ret;

	if (obj->package.count != 2 ||
	    obj->package.elements[0].type != ACPI_TYPE_INTEGER ||
	    obj->package.elements[1].type != ACPI_TYPE_BUFFER) {
		ret = -ENOMSG;

		goto err_free;
	}

	buffer_size = obj->package.elements[0].integer.value;

	if (!buffer_size) {
		ret = -ENODATA;

		goto err_free;
	}

	if (buffer_size > obj->package.elements[1].buffer.length) {
		dev_warn(&wdev->dev,
			 FW_WARN "WMI buffer size (%llu) exceeds ACPI buffer size (%d)\n",
			 buffer_size, obj->package.elements[1].buffer.length);
		ret = -EMSGSIZE;

		goto err_free;
	}

	*result = obj;

	return 0;

err_free:
	kfree(obj);

	return ret;
}

static int dell_wmi_ddv_query_string(struct wmi_device *wdev, enum dell_ddv_method method,
				     u32 arg, union acpi_object **result)
{
	return dell_wmi_ddv_query_type(wdev, method, arg, result, ACPI_TYPE_STRING);
}

/*
 * Needs to be called with lock held, except during initialization.
 */
static int dell_wmi_ddv_update_sensors(struct wmi_device *wdev, enum dell_ddv_method method,
				       struct dell_wmi_ddv_sensors *sensors, size_t entry_size)
{
	u64 buffer_size, rem, entries;
	union acpi_object *obj;
	u8 *buffer;
	int ret;

	if (sensors->obj) {
		if (time_before(jiffies, sensors->timestamp + HZ))
			return 0;

		kfree(sensors->obj);
		sensors->obj = NULL;
	}

	ret = dell_wmi_ddv_query_buffer(wdev, method, 0, &obj);
	if (ret < 0)
		return ret;

	/* buffer format sanity check */
	buffer_size = obj->package.elements[0].integer.value;
	buffer = obj->package.elements[1].buffer.pointer;
	entries = div64_u64_rem(buffer_size, entry_size, &rem);
	if (rem != 1 || buffer[buffer_size - 1] != 0xff) {
		ret = -ENOMSG;
		goto err_free;
	}

	if (!entries) {
		ret = -ENODATA;
		goto err_free;
	}

	sensors->obj = obj;
	sensors->entries = entries;
	sensors->timestamp = jiffies;

	return 0;

err_free:
	kfree(obj);

	return ret;
}

static umode_t dell_wmi_ddv_is_visible(const void *drvdata, enum hwmon_sensor_types type, u32 attr,
				       int channel)
{
	return 0444;
}

static int dell_wmi_ddv_fan_read_channel(struct dell_wmi_ddv_data *data, u32 attr, int channel,
					 long *val)
{
	struct fan_sensor_entry *entry;
	int ret;

	ret = dell_wmi_ddv_update_sensors(data->wdev, DELL_DDV_FAN_SENSOR_INFORMATION,
					  &data->fans, sizeof(*entry));
	if (ret < 0)
		return ret;

	if (channel >= data->fans.entries)
		return -ENXIO;

	entry = (struct fan_sensor_entry *)data->fans.obj->package.elements[1].buffer.pointer;
	switch (attr) {
	case hwmon_fan_input:
		*val = get_unaligned_le16(&entry[channel].rpm);
		return 0;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int dell_wmi_ddv_temp_read_channel(struct dell_wmi_ddv_data *data, u32 attr, int channel,
					  long *val)
{
	struct thermal_sensor_entry *entry;
	int ret;

	ret = dell_wmi_ddv_update_sensors(data->wdev, DELL_DDV_THERMAL_SENSOR_INFORMATION,
					  &data->temps, sizeof(*entry));
	if (ret < 0)
		return ret;

	if (channel >= data->temps.entries)
		return -ENXIO;

	entry = (struct thermal_sensor_entry *)data->temps.obj->package.elements[1].buffer.pointer;
	switch (attr) {
	case hwmon_temp_input:
		*val = entry[channel].now * 1000;
		return 0;
	case hwmon_temp_min:
		*val = entry[channel].min * 1000;
		return 0;
	case hwmon_temp_max:
		*val = entry[channel].max * 1000;
		return 0;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int dell_wmi_ddv_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			     int channel, long *val)
{
	struct dell_wmi_ddv_data *data = dev_get_drvdata(dev);
	int ret;

	switch (type) {
	case hwmon_fan:
		mutex_lock(&data->fans.lock);
		ret = dell_wmi_ddv_fan_read_channel(data, attr, channel, val);
		mutex_unlock(&data->fans.lock);
		return ret;
	case hwmon_temp:
		mutex_lock(&data->temps.lock);
		ret = dell_wmi_ddv_temp_read_channel(data, attr, channel, val);
		mutex_unlock(&data->temps.lock);
		return ret;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int dell_wmi_ddv_fan_read_string(struct dell_wmi_ddv_data *data, int channel,
					const char **str)
{
	struct fan_sensor_entry *entry;
	int ret;
	u8 type;

	ret = dell_wmi_ddv_update_sensors(data->wdev, DELL_DDV_FAN_SENSOR_INFORMATION,
					  &data->fans, sizeof(*entry));
	if (ret < 0)
		return ret;

	if (channel >= data->fans.entries)
		return -ENXIO;

	entry = (struct fan_sensor_entry *)data->fans.obj->package.elements[1].buffer.pointer;
	type = entry[channel].type;
	switch (type) {
	case 0x00 ... 0x07:
		*str = fan_labels[type];
		break;
	case 0x11 ... 0x14:
		*str = fan_dock_labels[type - 0x11];
		break;
	default:
		*str = "Unknown Fan";
		break;
	}

	return 0;
}

static int dell_wmi_ddv_temp_read_string(struct dell_wmi_ddv_data *data, int channel,
					 const char **str)
{
	struct thermal_sensor_entry *entry;
	int ret;

	ret = dell_wmi_ddv_update_sensors(data->wdev, DELL_DDV_THERMAL_SENSOR_INFORMATION,
					  &data->temps, sizeof(*entry));
	if (ret < 0)
		return ret;

	if (channel >= data->temps.entries)
		return -ENXIO;

	entry = (struct thermal_sensor_entry *)data->temps.obj->package.elements[1].buffer.pointer;
	switch (entry[channel].type) {
	case 0x00:
		*str = "CPU";
		break;
	case 0x11:
		*str = "Video";
		break;
	case 0x22:
		*str = "Memory"; /* sometimes called DIMM */
		break;
	case 0x33:
		*str = "Other";
		break;
	case 0x44:
		*str = "Ambient"; /* sometimes called SKIN */
		break;
	case 0x52:
		*str = "SODIMM";
		break;
	case 0x55:
		*str = "HDD";
		break;
	case 0x62:
		*str = "SODIMM 2";
		break;
	case 0x73:
		*str = "NB";
		break;
	case 0x83:
		*str = "Charger";
		break;
	case 0xbb:
		*str = "Memory 3";
		break;
	default:
		*str = "Unknown";
		break;
	}

	return 0;
}

static int dell_wmi_ddv_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				    int channel, const char **str)
{
	struct dell_wmi_ddv_data *data = dev_get_drvdata(dev);
	int ret;

	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_label:
			mutex_lock(&data->fans.lock);
			ret = dell_wmi_ddv_fan_read_string(data, channel, str);
			mutex_unlock(&data->fans.lock);
			return ret;
		default:
			break;
		}
		break;
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_label:
			mutex_lock(&data->temps.lock);
			ret = dell_wmi_ddv_temp_read_string(data, channel, str);
			mutex_unlock(&data->temps.lock);
			return ret;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static const struct hwmon_ops dell_wmi_ddv_ops = {
	.is_visible = dell_wmi_ddv_is_visible,
	.read = dell_wmi_ddv_read,
	.read_string = dell_wmi_ddv_read_string,
};

static struct hwmon_channel_info *dell_wmi_ddv_channel_create(struct device *dev, u64 count,
							      enum hwmon_sensor_types type,
							      u32 config)
{
	struct combined_channel_info *cinfo;
	int i;

	cinfo = devm_kzalloc(dev, struct_size(cinfo, config, count + 1), GFP_KERNEL);
	if (!cinfo)
		return ERR_PTR(-ENOMEM);

	cinfo->info.type = type;
	cinfo->info.config = cinfo->config;

	for (i = 0; i < count; i++)
		cinfo->config[i] = config;

	return &cinfo->info;
}

static void dell_wmi_ddv_hwmon_cache_invalidate(struct dell_wmi_ddv_sensors *sensors)
{
	if (!sensors->active)
		return;

	mutex_lock(&sensors->lock);
	kfree(sensors->obj);
	sensors->obj = NULL;
	mutex_unlock(&sensors->lock);
}

static void dell_wmi_ddv_hwmon_cache_destroy(void *data)
{
	struct dell_wmi_ddv_sensors *sensors = data;

	sensors->active = false;
	mutex_destroy(&sensors->lock);
	kfree(sensors->obj);
}

static struct hwmon_channel_info *dell_wmi_ddv_channel_init(struct wmi_device *wdev,
							    enum dell_ddv_method method,
							    struct dell_wmi_ddv_sensors *sensors,
							    size_t entry_size,
							    enum hwmon_sensor_types type,
							    u32 config)
{
	struct hwmon_channel_info *info;
	int ret;

	ret = dell_wmi_ddv_update_sensors(wdev, method, sensors, entry_size);
	if (ret < 0)
		return ERR_PTR(ret);

	mutex_init(&sensors->lock);
	sensors->active = true;

	ret = devm_add_action_or_reset(&wdev->dev, dell_wmi_ddv_hwmon_cache_destroy, sensors);
	if (ret < 0)
		return ERR_PTR(ret);

	info = dell_wmi_ddv_channel_create(&wdev->dev, sensors->entries, type, config);
	if (IS_ERR(info))
		devm_release_action(&wdev->dev, dell_wmi_ddv_hwmon_cache_destroy, sensors);

	return info;
}

static int dell_wmi_ddv_hwmon_add(struct dell_wmi_ddv_data *data)
{
	struct wmi_device *wdev = data->wdev;
	struct combined_chip_info *cinfo;
	struct hwmon_channel_info *info;
	struct device *hdev;
	int index = 0;
	int ret;

	if (!devres_open_group(&wdev->dev, dell_wmi_ddv_hwmon_add, GFP_KERNEL))
		return -ENOMEM;

	cinfo = devm_kzalloc(&wdev->dev, struct_size(cinfo, info, 4), GFP_KERNEL);
	if (!cinfo) {
		ret = -ENOMEM;

		goto err_release;
	}

	cinfo->chip.ops = &dell_wmi_ddv_ops;
	cinfo->chip.info = cinfo->info;

	info = dell_wmi_ddv_channel_create(&wdev->dev, 1, hwmon_chip, HWMON_C_REGISTER_TZ);
	if (IS_ERR(info)) {
		ret = PTR_ERR(info);

		goto err_release;
	}

	cinfo->info[index] = info;
	index++;

	info = dell_wmi_ddv_channel_init(wdev, DELL_DDV_FAN_SENSOR_INFORMATION, &data->fans,
					 sizeof(struct fan_sensor_entry), hwmon_fan,
					 (HWMON_F_INPUT | HWMON_F_LABEL));
	if (!IS_ERR(info)) {
		cinfo->info[index] = info;
		index++;
	}

	info = dell_wmi_ddv_channel_init(wdev, DELL_DDV_THERMAL_SENSOR_INFORMATION, &data->temps,
					 sizeof(struct thermal_sensor_entry), hwmon_temp,
					 (HWMON_T_INPUT | HWMON_T_MIN | HWMON_T_MAX |
					 HWMON_T_LABEL));
	if (!IS_ERR(info)) {
		cinfo->info[index] = info;
		index++;
	}

	if (index < 2) {
		/* Finding no available sensors is not an error */
		ret = 0;

		goto err_release;
	}

	hdev = devm_hwmon_device_register_with_info(&wdev->dev, "dell_ddv", data, &cinfo->chip,
						    NULL);
	if (IS_ERR(hdev)) {
		ret = PTR_ERR(hdev);

		goto err_release;
	}

	devres_close_group(&wdev->dev, dell_wmi_ddv_hwmon_add);

	return 0;

err_release:
	devres_release_group(&wdev->dev, dell_wmi_ddv_hwmon_add);

	return ret;
}

static int dell_wmi_ddv_battery_translate(struct dell_wmi_ddv_data *data,
					  struct power_supply *battery, u32 *index)
{
	u32 serial_dec, serial_hex, serial;
	union power_supply_propval val;
	int ret;

	guard(mutex)(&data->translation_cache_lock);

	for (int i = 0; i < ARRAY_SIZE(data->translation_cache); i++) {
		if (data->translation_cache[i] == battery) {
			dev_dbg(&data->wdev->dev, "Translation cache hit for battery index %u\n",
				i + 1);
			*index = i + 1;
			return 0;
		}
	}

	dev_dbg(&data->wdev->dev, "Translation cache miss\n");

	/*
	 * Perform a translation between a ACPI battery and a battery index.
	 * We have to use power_supply_get_property_direct() here because this
	 * function will also get called from the callbacks of the power supply
	 * extension.
	 */
	ret = power_supply_get_property_direct(battery, POWER_SUPPLY_PROP_SERIAL_NUMBER, &val);
	if (ret < 0)
		return ret;

	/*
	 * Some devices display the serial number of the ACPI battery (string!) as a decimal
	 * number while other devices display it as a hexadecimal number. Because of this we
	 * have to check both cases.
	 */
	ret = kstrtou32(val.strval, 16, &serial_hex);
	if (ret < 0)
		return ret;	/* Should never fail */

	ret = kstrtou32(val.strval, 10, &serial_dec);
	if (ret < 0)
		serial_dec = 0; /* Can fail, thus we only mark serial_dec as invalid */

	for (int i = 0; i < ARRAY_SIZE(data->translation_cache); i++) {
		ret = dell_wmi_ddv_query_integer(data->wdev, DELL_DDV_BATTERY_SERIAL_NUMBER, i + 1,
						 &serial);
		if (ret < 0)
			return ret;

		/* A serial number of 0 signals that this index is not associated with a battery */
		if (!serial)
			continue;

		if (serial == serial_dec || serial == serial_hex) {
			dev_dbg(&data->wdev->dev, "Translation cache update for battery index %u\n",
				i + 1);
			data->translation_cache[i] = battery;
			*index = i + 1;
			return 0;
		}
	}

	return -ENODEV;
}

static void dell_wmi_battery_invalidate(struct dell_wmi_ddv_data *data,
					struct power_supply *battery)
{
	guard(mutex)(&data->translation_cache_lock);

	for (int i = 0; i < ARRAY_SIZE(data->translation_cache); i++) {
		if (data->translation_cache[i] == battery) {
			data->translation_cache[i] = NULL;
			return;
		}
	}
}

static ssize_t eppid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct dell_wmi_ddv_data *data = container_of(attr, struct dell_wmi_ddv_data, eppid_attr);
	union acpi_object *obj;
	u32 index;
	int ret;

	ret = dell_wmi_ddv_battery_translate(data, to_power_supply(dev), &index);
	if (ret < 0)
		return ret;

	ret = dell_wmi_ddv_query_string(data->wdev, DELL_DDV_BATTERY_EPPID, index, &obj);
	if (ret < 0)
		return ret;

	if (obj->string.length != DELL_EPPID_LENGTH && obj->string.length != DELL_EPPID_EXT_LENGTH)
		dev_info_once(&data->wdev->dev, FW_INFO "Suspicious ePPID length (%d)\n",
			      obj->string.length);

	ret = sysfs_emit(buf, "%s\n", obj->string.pointer);

	kfree(obj);

	return ret;
}

static int dell_wmi_ddv_get_health(struct dell_wmi_ddv_data *data, u32 index,
				   union power_supply_propval *val)
{
	u32 value, code;
	int ret;

	ret = dell_wmi_ddv_query_integer(data->wdev, DELL_DDV_BATTERY_MANUFACTURER_ACCESS, index,
					 &value);
	if (ret < 0)
		return ret;

	switch (FIELD_GET(MA_FAILURE_MODE_MASK, value)) {
	case MA_FAILURE_MODE_PERMANENT:
		code = FIELD_GET(MA_PERMANENT_FAILURE_CODE_MASK, value);
		switch (code) {
		case MA_PERMANENT_FAILURE_FUSE_BLOWN:
			val->intval = POWER_SUPPLY_HEALTH_BLOWN_FUSE;
			return 0;
		case MA_PERMANENT_FAILURE_CELL_IMBALANCE:
			val->intval = POWER_SUPPLY_HEALTH_CELL_IMBALANCE;
			return 0;
		case MA_PERMANENT_FAILURE_OVERVOLTAGE:
			val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
			return 0;
		case MA_PERMANENT_FAILURE_FET_FAILURE:
			val->intval = POWER_SUPPLY_HEALTH_DEAD;
			return 0;
		default:
			dev_notice_once(&data->wdev->dev, "Unknown permanent failure code %u\n",
					code);
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
			return 0;
		}
	case MA_FAILURE_MODE_OVERHEAT:
		code = FIELD_GET(MA_OVERHEAT_FAILURE_CODE_MASK, value);
		switch (code) {
		case MA_OVERHEAT_FAILURE_START:
		case MA_OVERHEAT_FAILURE_CHARGING:
		case MA_OVERHEAT_FAILURE_DISCHARGING:
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
			return 0;
		default:
			dev_notice_once(&data->wdev->dev, "Unknown overheat failure code %u\n",
					code);
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
			return 0;
		}
	case MA_FAILURE_MODE_OVERCURRENT:
		code = FIELD_GET(MA_OVERCURRENT_FAILURE_CODE_MASK, value);
		switch (code) {
		case MA_OVERCURRENT_FAILURE_CHARGING:
		case MA_OVERCURRENT_FAILURE_DISCHARGING:
			val->intval = POWER_SUPPLY_HEALTH_OVERCURRENT;
			return 0;
		default:
			dev_notice_once(&data->wdev->dev, "Unknown overcurrent failure code %u\n",
					code);
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
			return 0;
		}
	default:
		val->intval = POWER_SUPPLY_HEALTH_GOOD;
		return 0;
	}
}

static int dell_wmi_ddv_get_manufacture_date(struct dell_wmi_ddv_data *data, u32 index,
					     enum power_supply_property psp,
					     union power_supply_propval *val)
{
	u16 year, month, day;
	u32 value;
	int ret;

	ret = dell_wmi_ddv_query_integer(data->wdev, DELL_DDV_BATTERY_MANUFACTURE_DATE,
					 index, &value);
	if (ret < 0)
		return ret;
	if (value > U16_MAX)
		return -ENXIO;

	/*
	 * Some devices report a invalid manufacture date value
	 * like 0.0.1980. Because of this we have to check the
	 * whole value before exposing parts of it to user space.
	 */
	year = FIELD_GET(SBS_MANUFACTURE_YEAR_MASK, value) + 1980;
	month = FIELD_GET(SBS_MANUFACTURE_MONTH_MASK, value);
	if (month < 1 || month > 12)
		return -ENODATA;

	day = FIELD_GET(SBS_MANUFACTURE_DAY_MASK, value);
	if (day < 1 || day > 31)
		return -ENODATA;

	switch (psp) {
	case POWER_SUPPLY_PROP_MANUFACTURE_YEAR:
		val->intval = year;
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURE_MONTH:
		val->intval = month;
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURE_DAY:
		val->intval = day;
		return 0;
	default:
		return -EINVAL;
	}
}

static int dell_wmi_ddv_get_property(struct power_supply *psy, const struct power_supply_ext *ext,
				     void *drvdata, enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct dell_wmi_ddv_data *data = drvdata;
	u32 index, value;
	int ret;

	ret = dell_wmi_ddv_battery_translate(data, psy, &index);
	if (ret < 0)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_HEALTH:
		return dell_wmi_ddv_get_health(data, index, val);
	case POWER_SUPPLY_PROP_TEMP:
		ret = dell_wmi_ddv_query_integer(data->wdev, DELL_DDV_BATTERY_TEMPERATURE, index,
						 &value);
		if (ret < 0)
			return ret;

		/* Use 2732 instead of 2731.5 to avoid unnecessary rounding and to emulate
		 * the behaviour of the OEM application which seems to round down the result.
		 */
		val->intval = value - 2732;
		return 0;
	case POWER_SUPPLY_PROP_MANUFACTURE_YEAR:
	case POWER_SUPPLY_PROP_MANUFACTURE_MONTH:
	case POWER_SUPPLY_PROP_MANUFACTURE_DAY:
		return dell_wmi_ddv_get_manufacture_date(data, index, psp, val);
	default:
		return -EINVAL;
	}
}

static const enum power_supply_property dell_wmi_ddv_properties[] = {
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MANUFACTURE_YEAR,
	POWER_SUPPLY_PROP_MANUFACTURE_MONTH,
	POWER_SUPPLY_PROP_MANUFACTURE_DAY,
};

static const struct power_supply_ext dell_wmi_ddv_extension = {
	.name = DRIVER_NAME,
	.properties = dell_wmi_ddv_properties,
	.num_properties = ARRAY_SIZE(dell_wmi_ddv_properties),
	.get_property = dell_wmi_ddv_get_property,
};

static int dell_wmi_ddv_add_battery(struct power_supply *battery, struct acpi_battery_hook *hook)
{
	struct dell_wmi_ddv_data *data = container_of(hook, struct dell_wmi_ddv_data, hook);
	int ret;

	/*
	 * We cannot do the battery matching here since the battery might be absent, preventing
	 * us from reading the serial number.
	 */

	ret = device_create_file(&battery->dev, &data->eppid_attr);
	if (ret < 0)
		return ret;

	ret = power_supply_register_extension(battery, &dell_wmi_ddv_extension, &data->wdev->dev,
					      data);
	if (ret < 0) {
		device_remove_file(&battery->dev, &data->eppid_attr);

		return ret;
	}

	return 0;
}

static int dell_wmi_ddv_remove_battery(struct power_supply *battery, struct acpi_battery_hook *hook)
{
	struct dell_wmi_ddv_data *data = container_of(hook, struct dell_wmi_ddv_data, hook);

	device_remove_file(&battery->dev, &data->eppid_attr);
	power_supply_unregister_extension(battery, &dell_wmi_ddv_extension);

	dell_wmi_battery_invalidate(data, battery);

	return 0;
}

static int dell_wmi_ddv_battery_add(struct dell_wmi_ddv_data *data)
{
	int ret;

	ret = devm_mutex_init(&data->wdev->dev, &data->translation_cache_lock);
	if (ret < 0)
		return ret;

	data->hook.name = "Dell DDV Battery Extension";
	data->hook.add_battery = dell_wmi_ddv_add_battery;
	data->hook.remove_battery = dell_wmi_ddv_remove_battery;

	sysfs_attr_init(&data->eppid_attr.attr);
	data->eppid_attr.attr.name = "eppid";
	data->eppid_attr.attr.mode = 0444;
	data->eppid_attr.show = eppid_show;

	return devm_battery_hook_register(&data->wdev->dev, &data->hook);
}

static int dell_wmi_ddv_buffer_read(struct seq_file *seq, enum dell_ddv_method method)
{
	struct device *dev = seq->private;
	struct dell_wmi_ddv_data *data = dev_get_drvdata(dev);
	union acpi_object *obj;
	u64 size;
	u8 *buf;
	int ret;

	ret = dell_wmi_ddv_query_buffer(data->wdev, method, 0, &obj);
	if (ret < 0)
		return ret;

	size = obj->package.elements[0].integer.value;
	buf = obj->package.elements[1].buffer.pointer;
	ret = seq_write(seq, buf, size);
	kfree(obj);

	return ret;
}

static int dell_wmi_ddv_fan_read(struct seq_file *seq, void *offset)
{
	return dell_wmi_ddv_buffer_read(seq, DELL_DDV_FAN_SENSOR_INFORMATION);
}

static int dell_wmi_ddv_temp_read(struct seq_file *seq, void *offset)
{
	return dell_wmi_ddv_buffer_read(seq, DELL_DDV_THERMAL_SENSOR_INFORMATION);
}

static void dell_wmi_ddv_debugfs_remove(void *data)
{
	struct dentry *entry = data;

	debugfs_remove(entry);
}

static void dell_wmi_ddv_debugfs_init(struct wmi_device *wdev)
{
	struct dentry *entry;
	char name[64];

	scnprintf(name, ARRAY_SIZE(name), "%s-%s", DRIVER_NAME, dev_name(&wdev->dev));
	entry = debugfs_create_dir(name, NULL);

	debugfs_create_devm_seqfile(&wdev->dev, "fan_sensor_information", entry,
				    dell_wmi_ddv_fan_read);
	debugfs_create_devm_seqfile(&wdev->dev, "thermal_sensor_information", entry,
				    dell_wmi_ddv_temp_read);

	devm_add_action_or_reset(&wdev->dev, dell_wmi_ddv_debugfs_remove, entry);
}

static int dell_wmi_ddv_probe(struct wmi_device *wdev, const void *context)
{
	struct dell_wmi_ddv_data *data;
	u32 version;
	int ret;

	ret = dell_wmi_ddv_query_integer(wdev, DELL_DDV_INTERFACE_VERSION, 0, &version);
	if (ret < 0)
		return ret;

	dev_dbg(&wdev->dev, "WMI interface version: %d\n", version);
	if (version < DELL_DDV_SUPPORTED_VERSION_MIN || version > DELL_DDV_SUPPORTED_VERSION_MAX) {
		if (!force)
			return -ENODEV;

		dev_warn(&wdev->dev, "Loading despite unsupported WMI interface version (%u)\n",
			 version);
	}

	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	dev_set_drvdata(&wdev->dev, data);
	data->wdev = wdev;

	dell_wmi_ddv_debugfs_init(wdev);

	if (IS_REACHABLE(CONFIG_ACPI_BATTERY)) {
		ret = dell_wmi_ddv_battery_add(data);
		if (ret < 0)
			dev_warn(&wdev->dev, "Unable to register ACPI battery hook: %d\n", ret);
	}

	if (IS_REACHABLE(CONFIG_HWMON)) {
		ret = dell_wmi_ddv_hwmon_add(data);
		if (ret < 0)
			dev_warn(&wdev->dev, "Unable to register hwmon interface: %d\n", ret);
	}

	return 0;
}

static int dell_wmi_ddv_resume(struct device *dev)
{
	struct dell_wmi_ddv_data *data = dev_get_drvdata(dev);

	/* Force re-reading of all active sensors */
	dell_wmi_ddv_hwmon_cache_invalidate(&data->fans);
	dell_wmi_ddv_hwmon_cache_invalidate(&data->temps);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(dell_wmi_ddv_dev_pm_ops, NULL, dell_wmi_ddv_resume);

static const struct wmi_device_id dell_wmi_ddv_id_table[] = {
	{ DELL_DDV_GUID, NULL },
	{ }
};
MODULE_DEVICE_TABLE(wmi, dell_wmi_ddv_id_table);

static struct wmi_driver dell_wmi_ddv_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.pm = pm_sleep_ptr(&dell_wmi_ddv_dev_pm_ops),
	},
	.id_table = dell_wmi_ddv_id_table,
	.probe = dell_wmi_ddv_probe,
	.no_singleton = true,
};
module_wmi_driver(dell_wmi_ddv_driver);

MODULE_AUTHOR("Armin Wolf <W_Armin@gmx.de>");
MODULE_DESCRIPTION("Dell WMI sensor driver");
MODULE_LICENSE("GPL");
