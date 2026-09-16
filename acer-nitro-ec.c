// SPDX-License-Identifier: GPL-2.0
/*
 * Acer Nitro EC fan control driver
 *
 * Exposes CPU/GPU fan speed control and temperature readings via the
 * standard Linux hwmon interface for Acer Nitro AN515/AN517 laptops.
 *
 * Supported models:
 *   AN515-44, AN515-46, AN515-54, AN515-55, AN515-56, AN515-57, AN515-58, AN517-55
 *
 * Once loaded, the following sysfs entries become available under
 * /sys/class/hwmon/hwmonX/:
 *
 *   fan1_input      - CPU fan speed (RPM)
 *   fan2_input      - GPU fan speed (RPM)
 *   pwm1            - CPU fan duty cycle (0-255)
 *   pwm1_enable     - CPU fan mode: 0=turbo, 1=manual, 2=auto
 *   pwm2            - GPU fan duty cycle (0-255)
 *   pwm2_enable     - GPU fan mode: 0=turbo, 1=manual, 2=auto
 *   temp1_input     - CPU temperature (millidegrees Celsius)
 *   temp2_input     - GPU temperature (millidegrees Celsius)
 *   temp3_input     - System temperature (millidegrees Celsius)
 *
 * MANUAL mode has a five-second lease per fan. Successful PWM writes
 * refresh only that fan; expiry of either lease restores both fans to AUTO.
 * TURBO is not leased. Re-enter MANUAL explicitly after fallback.
 *
 * Logging:
 *   - Load with debug=1 for verbose output:
 *       sudo insmod acer-nitro-ec.ko debug=1
 *   - Or enable dynamic_debug at runtime (no reload needed):
 *       echo "module acer_nitro_ec +p" > /sys/kernel/debug/dynamic_debug/control
 *   - Watch logs:
 *       sudo dmesg -w | grep acer-nitro-ec
 */

#define DRIVER_NAME "acer-nitro-ec"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt
#define NITRO_FEATURE_CTRL_REG 0x03
#define NITRO_FAN_CONTROL_ENABLE 0x10

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm.h>

/* When debug=1, emit dev_dbg messages as dev_info so they appear in dmesg
 * without needing to change the kernel log level or dynamic_debug config.
 */
static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable verbose logging (default: false). "
		 "Can also use: echo 'module acer_nitro_ec +p' > "
		 "/sys/kernel/debug/dynamic_debug/control");

#define nitro_dbg(dev, fmt, ...) do {				\
	if (debug)						\
		dev_info(dev, fmt, ##__VA_ARGS__);		\
	else							\
		dev_dbg(dev, fmt, ##__VA_ARGS__);		\
} while (0)

/* ------------------------------------------------------------------ */
/* EC register map                                                      */
/* ------------------------------------------------------------------ */

struct nitro_ec_regs {
	u8 cpu_fan_mode_ctrl;
	u8 cpu_fan_speed_ctrl;
	u8 cpu_fan_rpm_hi;
	u8 cpu_fan_rpm_lo;
	u8 gpu_fan_mode_ctrl;
	u8 gpu_fan_speed_ctrl;
	u8 gpu_fan_rpm_hi;
	u8 gpu_fan_rpm_lo;
	u8 cpu_temp;
	u8 gpu_temp;
	u8 sys_temp;
};

/* MANUAL must be refreshed by successful PWM writes within five seconds. */
#define NITRO_MANUAL_LEASE_MS 5000

/* Fan mode EC values */
#define CPU_AUTO_MODE		0x04
#define CPU_MANUAL_MODE		0x0C
#define CPU_TURBO_MODE		0x08
#define GPU_AUTO_MODE		0x10
#define GPU_MANUAL_MODE		0x30
#define GPU_TURBO_MODE		0x20

/*
 * AN515-46 register layout — shared by most AN515/AN517 models.
 * Source: reverse-engineered from Linux-NitroSense project.
 */
static const struct nitro_ec_regs regs_an515_46 = {
	.cpu_fan_mode_ctrl  = 0x22,
	.cpu_fan_speed_ctrl = 0x37,
	.cpu_fan_rpm_hi     = 0x13,
	.cpu_fan_rpm_lo     = 0x14,
	.gpu_fan_mode_ctrl  = 0x21,
	.gpu_fan_speed_ctrl = 0x3A,
	.gpu_fan_rpm_hi     = 0x15,
	.gpu_fan_rpm_lo     = 0x16,
	.cpu_temp           = 0xB0,
	.gpu_temp           = 0xB6,
	.sys_temp           = 0xB3,
};

/*
 * AN515-44 differs only in GPU/system temperature register addresses.
 */
static const struct nitro_ec_regs regs_an515_44 = {
	.cpu_fan_mode_ctrl  = 0x22,
	.cpu_fan_speed_ctrl = 0x37,
	.cpu_fan_rpm_hi     = 0x13,
	.cpu_fan_rpm_lo     = 0x14,
	.gpu_fan_mode_ctrl  = 0x21,
	.gpu_fan_speed_ctrl = 0x3A,
	.gpu_fan_rpm_hi     = 0x15,
	.gpu_fan_rpm_lo     = 0x16,
	.cpu_temp           = 0xB0,
	.gpu_temp           = 0xB4,
	.sys_temp           = 0xB0,
};

/* ------------------------------------------------------------------ */
/* Per-device data                                                      */
/* ------------------------------------------------------------------ */

struct nitro_manual_lease {
	bool active;
	unsigned long deadline;
};

struct nitro_ec_data {
	struct device		  *hwmon_dev;
	struct device		  *dev;
	struct mutex lock;
	struct delayed_work lease_work;
	struct nitro_manual_lease leases[2];
	bool writes_blocked;
	bool auto_pending;
	const struct nitro_ec_regs *regs;
};

/* ------------------------------------------------------------------ */
/* EC helpers                                                           */
/* ------------------------------------------------------------------ */

static int nitro_ec_read(u8 addr, u8 *val)
{
	int ret = ec_read(addr, val);

	if (ret)
		pr_warn("EC read failed at 0x%02X (err %d)\n", addr, ret);

	return ret;
}

static int nitro_ec_write(u8 addr, u8 val)
{
	int ret = ec_write(addr, val);

	if (ret)
		pr_warn("EC write failed at 0x%02X = 0x%02X (err %d)\n",
			addr, val, ret);

	return ret;
}

static int nitro_enable_fan_control(struct device *dev)
{
	const char *model = dmi_get_system_info(DMI_PRODUCT_NAME);
	u8 feature_flags, new_flags;
	int ret;

	if (!model ||
	    (!strstr(model, "AN515-57") && !strstr(model, "AN515-58")))
		return 0;

	/* Fresh read-modify-write preserves unrelated feature flags. */
	ret = nitro_ec_read(NITRO_FEATURE_CTRL_REG, &feature_flags);
	if (ret) {
		dev_err(dev, "failed to read EC feature control register: %d\n",
			ret);
		return ret;
	}

	if (feature_flags & NITRO_FAN_CONTROL_ENABLE) {
		dev_info(dev, "EC fan control already enabled: 0x%02x\n",
			 feature_flags);
		return 0;
	}

	new_flags = feature_flags | NITRO_FAN_CONTROL_ENABLE;
	ret = nitro_ec_write(NITRO_FEATURE_CTRL_REG, new_flags);
	if (ret) {
		dev_err(dev, "failed to enable EC fan control: %d\n", ret);
		return ret;
	}

	dev_info(dev, "EC fan control enabled: 0x%02x -> 0x%02x\n",
		 feature_flags, new_flags);
	return 0;
}

static int nitro_set_fans_auto(struct device *dev)
{
	struct nitro_ec_data *data = dev_get_drvdata(dev);
	int cpu_ret, gpu_ret;

	cpu_ret = nitro_ec_write(data->regs->cpu_fan_mode_ctrl,
				CPU_AUTO_MODE);
	if (cpu_ret)
		dev_err(dev, "failed to restore CPU fan AUTO mode: %d\n",
			cpu_ret);

	/* Always attempt both fans, even if the CPU write failed. */
	gpu_ret = nitro_ec_write(data->regs->gpu_fan_mode_ctrl,
				GPU_AUTO_MODE);
	if (gpu_ret)
		dev_err(dev, "failed to restore GPU fan AUTO mode: %d\n",
			gpu_ret);

	return cpu_ret ? cpu_ret : gpu_ret;
}

/* All lease helpers and AUTO writes are called with data->lock held. */
static void nitro_invalidate_leases(struct nitro_ec_data *data)
{
	data->leases[0].active = false;
	data->leases[1].active = false;
}

static void nitro_schedule_lease_work(struct nitro_ec_data *data)
{
	unsigned long now = jiffies, deadline = 0;
	bool active = false;
	int i;

	if (data->writes_blocked)
		return;
	if (data->auto_pending) {
		mod_delayed_work(system_wq, &data->lease_work,
				 msecs_to_jiffies(NITRO_MANUAL_LEASE_MS));
		return;
	}
	for (i = 0; i < ARRAY_SIZE(data->leases); i++) {
		if (!data->leases[i].active)
			continue;
		if (!active || time_before(data->leases[i].deadline, deadline))
			deadline = data->leases[i].deadline;
		active = true;
	}
	if (active)
		mod_delayed_work(system_wq, &data->lease_work,
				 time_after(deadline, now) ? deadline - now : 0);
	else
		cancel_delayed_work(&data->lease_work);
}

static int nitro_check_lease_expiry(struct nitro_ec_data *data)
{
	unsigned long now = jiffies;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(data->leases); i++) {
		if (data->leases[i].active &&
		    time_after_eq(now, data->leases[i].deadline)) {
			dev_warn(data->dev,
				 "%s MANUAL lease expired; restoring both fans to AUTO\n",
				 i == 0 ? "CPU" : "GPU");
			nitro_invalidate_leases(data);
			data->auto_pending = true;
			break;
		}
	}
	if (!data->auto_pending)
		return 0;

	ret = nitro_set_fans_auto(data->dev);
	data->auto_pending = !!ret;
	/* Failed restoration blocks writes and is retried by the same worker. */
	return ret;
}

static void nitro_lease_work(struct work_struct *work)
{
	struct nitro_ec_data *data = container_of(to_delayed_work(work),
						 struct nitro_ec_data, lease_work);

	mutex_lock(&data->lock);
	if (!data->writes_blocked) {
		nitro_check_lease_expiry(data);
		nitro_schedule_lease_work(data);
	}
	mutex_unlock(&data->lock);
}

/*
 * Read a 16-bit RPM value from two consecutive EC registers.
 * The EC stores the raw fan RPM as a big-endian 16-bit integer.
 */
static int nitro_read_fan_rpm(struct device *dev,
			      const struct nitro_ec_regs *regs, int channel,
			      long *rpm)
{
	u8 hi, lo;
	int ret;

	if (channel == 0) {
		ret = nitro_ec_read(regs->cpu_fan_rpm_hi, &hi);
		if (ret)
			return ret;
		ret = nitro_ec_read(regs->cpu_fan_rpm_lo, &lo);
	} else {
		ret = nitro_ec_read(regs->gpu_fan_rpm_hi, &hi);
		if (ret)
			return ret;
		ret = nitro_ec_read(regs->gpu_fan_rpm_lo, &lo);
	}

	if (ret)
		return ret;

	/*
	 * The EC naming is misleading: the register called "HIGH BITS" (0x13/0x15)
	 * holds the low byte of RPM, and "LOW BITS" (0x14/0x16) holds the high byte.
	 * Confirmed by cross-referencing with the Linux-NitroSense Python implementation:
	 *   cpufanspeed = cpufanspeedLowBits << 8 | cpufanspeedHighBits
	 */
	*rpm = (long)(((u16)lo << 8) | hi);

	nitro_dbg(dev, "%s fan RPM: reg_hi(0x%02X)=0x%02X reg_lo(0x%02X)=0x%02X -> %ld RPM\n",
		  channel == 0 ? "CPU" : "GPU",
		  channel == 0 ? regs->cpu_fan_rpm_hi : regs->gpu_fan_rpm_hi, hi,
		  channel == 0 ? regs->cpu_fan_rpm_lo : regs->gpu_fan_rpm_lo, lo,
		  *rpm);

	return 0;
}

/* ------------------------------------------------------------------ */
/* hwmon callbacks                                                      */
/* ------------------------------------------------------------------ */

static umode_t nitro_hwmon_is_visible(const void *drvdata,
				      enum hwmon_sensor_types type,
				      u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		return 0444;
	case hwmon_pwm:
		return 0644;
	case hwmon_temp:
		return 0444;
	default:
		return 0;
	}
}

static int nitro_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val)
{
	struct nitro_ec_data *data = dev_get_drvdata(dev);
	const struct nitro_ec_regs *regs = data->regs;
	u8 raw;
	int ret;

	switch (type) {

	case hwmon_fan:
		return nitro_read_fan_rpm(dev, regs, channel, val);

	case hwmon_pwm:
		switch (attr) {

		case hwmon_pwm_input:
			ret = nitro_ec_read(channel == 0
					    ? regs->cpu_fan_speed_ctrl
					    : regs->gpu_fan_speed_ctrl, &raw);
			if (ret)
				return ret;
			/* EC uses raw values 0-100; hwmon expects 0-255 */
			*val = (long)raw * 255 / 100;
			nitro_dbg(dev, "%s pwm read: EC raw=%u (0-100) hwmon=%ld\n",
				  channel == 0 ? "CPU" : "GPU", raw, *val);
			return 0;

		case hwmon_pwm_enable:
			ret = nitro_ec_read(channel == 0
					    ? regs->cpu_fan_mode_ctrl
					    : regs->gpu_fan_mode_ctrl, &raw);
			if (ret)
				return ret;
			if (channel == 0) {
				if (raw == CPU_TURBO_MODE)
					*val = 0;
				else if (raw == CPU_MANUAL_MODE)
					*val = 1;
				else if (raw == CPU_AUTO_MODE)
					*val = 2; /* auto */
				else
					return -EIO;
			} else {
				if (raw == GPU_TURBO_MODE)
					*val = 0;
				else if (raw == GPU_MANUAL_MODE)
					*val = 1;
				else if (raw == GPU_AUTO_MODE)
					*val = 2; /* auto */
				else
					return -EIO;
			}
			nitro_dbg(dev,
				  "%s mode read: EC=0x%02X -> pwm_enable=%ld "
				  "(0=turbo 1=manual 2=auto)\n",
				  channel == 0 ? "CPU" : "GPU", raw, *val);
			return 0;
		}
		break;

	case hwmon_temp: {
		static const char * const temp_names[] = {
			"CPU", "GPU", "system"
		};
		switch (channel) {
		case 0: ret = nitro_ec_read(regs->cpu_temp, &raw); break;
		case 1: ret = nitro_ec_read(regs->gpu_temp, &raw); break;
		case 2: ret = nitro_ec_read(regs->sys_temp, &raw); break;
		default: return -EOPNOTSUPP;
		}
		if (ret)
			return ret;
		*val = (long)raw * 1000; /* millidegrees */
		nitro_dbg(dev, "%s temp: %u°C\n", temp_names[channel], raw);
		return 0;
	}

	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int nitro_hwmon_write_locked(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long val)
{
	struct nitro_ec_data *data = dev_get_drvdata(dev);
	const struct nitro_ec_regs *regs = data->regs;
	u8 ec_val;

	switch (type) {

	case hwmon_pwm:
		switch (attr) {

		case hwmon_pwm_input:
			if (val < 0 || val > 255)
				return -EINVAL;
			/* Scale hwmon 0-255 to raw EC 0-100 */
			ec_val = (u8)(val * 100 / 255);
			dev_info(dev, "%s fan speed set: hwmon=%ld -> EC raw=%u (0-100)\n",
				 channel == 0 ? "CPU" : "GPU", val, ec_val);
			return nitro_ec_write(channel == 0
					      ? regs->cpu_fan_speed_ctrl
					      : regs->gpu_fan_speed_ctrl,
					      ec_val);

		case hwmon_pwm_enable: {
			/*
			 * 0 = turbo (full speed, firmware-forced)
			 * 1 = manual (controlled via pwm attribute)
			 * 2 = automatic (firmware manages speed)
			 */
			static const char * const mode_names[] = {
				"turbo", "manual", "auto"
			};
			if (val < 0 || val > 2)
				return -EINVAL;
			if (channel == 0) {
				if (val == 0)
					ec_val = CPU_TURBO_MODE;
				else if (val == 1)
					ec_val = CPU_MANUAL_MODE;
				else
					ec_val = CPU_AUTO_MODE;
				dev_info(dev,
					 "CPU fan mode set: %s (EC=0x%02X)\n",
					 mode_names[val], ec_val);
				return nitro_ec_write(regs->cpu_fan_mode_ctrl,
						      ec_val);
			} else {
				if (val == 0)
					ec_val = GPU_TURBO_MODE;
				else if (val == 1)
					ec_val = GPU_MANUAL_MODE;
				else
					ec_val = GPU_AUTO_MODE;
				dev_info(dev,
					 "GPU fan mode set: %s (EC=0x%02X)\n",
					 mode_names[val], ec_val);
				return nitro_ec_write(regs->gpu_fan_mode_ctrl,
						      ec_val);
			}
		}
		}
		break;

	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int nitro_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long val)
{
	struct nitro_ec_data *data = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_pwm || channel < 0 ||
	    channel >= ARRAY_SIZE(data->leases) ||
	    (attr != hwmon_pwm_input && attr != hwmon_pwm_enable))
		return -EOPNOTSUPP;

	mutex_lock(&data->lock);
	if (data->writes_blocked) {
		ret = -EBUSY;
		goto out;
	}
	/* Check before even invalid writes, and before any possible renewal. */
	ret = nitro_check_lease_expiry(data);
	if (ret)
		goto schedule;

	ret = nitro_hwmon_write_locked(dev, type, attr, channel, val);
	if (!ret) {
		if (attr == hwmon_pwm_enable) {
			data->leases[channel].active = val == 1;
			if (val == 1)
				nitro_dbg(dev, "%s MANUAL lease started (%u ms)\n",
					  channel == 0 ? "CPU" : "GPU",
					  NITRO_MANUAL_LEASE_MS);
		}
		if (data->leases[channel].active)
			data->leases[channel].deadline = jiffies +
				msecs_to_jiffies(NITRO_MANUAL_LEASE_MS);
	}
schedule:
	nitro_schedule_lease_work(data);
out:
	mutex_unlock(&data->lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* hwmon chip descriptor                                                */
/* ------------------------------------------------------------------ */

static const struct hwmon_channel_info * const nitro_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
		HWMON_F_INPUT,   /* fan1 = CPU */
		HWMON_F_INPUT),  /* fan2 = GPU */
	HWMON_CHANNEL_INFO(pwm,
		HWMON_PWM_INPUT | HWMON_PWM_ENABLE,  /* pwm1 = CPU */
		HWMON_PWM_INPUT | HWMON_PWM_ENABLE), /* pwm2 = GPU */
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT,   /* temp1 = CPU */
		HWMON_T_INPUT,   /* temp2 = GPU */
		HWMON_T_INPUT),  /* temp3 = system */
	NULL
};

static const struct hwmon_ops nitro_hwmon_ops = {
	.is_visible = nitro_hwmon_is_visible,
	.read       = nitro_hwmon_read,
	.write      = nitro_hwmon_write,
};

static const struct hwmon_chip_info nitro_chip_info = {
	.ops  = &nitro_hwmon_ops,
	.info = nitro_hwmon_info,
};

/* ------------------------------------------------------------------ */
/* Platform driver                                                      */
/* ------------------------------------------------------------------ */

static int nitro_ec_probe(struct platform_device *pdev)
{
	const struct nitro_ec_regs *regs;
	struct nitro_ec_data *data;
	int ret;

	regs = dev_get_platdata(&pdev->dev);
	if (!regs) {
		dev_err(&pdev->dev, "no platform data — aborting probe\n");
		return -ENODEV;
	}

	ret = nitro_enable_fan_control(&pdev->dev);
	if (ret)
		return ret;

	dev_dbg(&pdev->dev, "EC register map:\n"
		"  CPU fan mode=0x%02X speed=0x%02X rpm=0x%02X/0x%02X\n"
		"  GPU fan mode=0x%02X speed=0x%02X rpm=0x%02X/0x%02X\n"
		"  Temps: cpu=0x%02X gpu=0x%02X sys=0x%02X\n",
		regs->cpu_fan_mode_ctrl, regs->cpu_fan_speed_ctrl,
		regs->cpu_fan_rpm_hi,    regs->cpu_fan_rpm_lo,
		regs->gpu_fan_mode_ctrl, regs->gpu_fan_speed_ctrl,
		regs->gpu_fan_rpm_hi,    regs->gpu_fan_rpm_lo,
		regs->cpu_temp, regs->gpu_temp, regs->sys_temp);

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regs = regs;
	data->dev = &pdev->dev;
	data->writes_blocked = true;
	mutex_init(&data->lock);
	INIT_DELAYED_WORK(&data->lease_work, nitro_lease_work);
	platform_set_drvdata(pdev, data);

	/* Establish the safe state before exposing writable hwmon attributes. */
	mutex_lock(&data->lock);
	ret = nitro_set_fans_auto(&pdev->dev);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	data->hwmon_dev = devm_hwmon_device_register_with_info(
		&pdev->dev, "acer_nitro_ec", data,
		&nitro_chip_info, NULL);

	if (IS_ERR(data->hwmon_dev)) {
		dev_err(&pdev->dev, "hwmon registration failed: %ld\n",
			PTR_ERR(data->hwmon_dev));
		return PTR_ERR(data->hwmon_dev);
	}

	mutex_lock(&data->lock);
	data->writes_blocked = false;
	mutex_unlock(&data->lock);

	dev_info(&pdev->dev, "hwmon interface registered at %s\n",
		 dev_name(data->hwmon_dev));
	if (debug)
		dev_info(&pdev->dev, "verbose logging enabled\n");

	return 0;
}

static int nitro_stop_fan_control(struct device *dev, bool teardown)
{
	struct nitro_ec_data *data = dev_get_drvdata(dev);
	int attempt, ret;
	int attempts = teardown ? 3 : 1;

	mutex_lock(&data->lock);
	data->writes_blocked = true;
	nitro_invalidate_leases(data);
	mutex_unlock(&data->lock);

	/* The worker needs lock: drain it only after releasing lock. */
	cancel_delayed_work_sync(&data->lease_work);

	mutex_lock(&data->lock);
	for (attempt = 0; attempt < attempts; attempt++) {
		ret = nitro_set_fans_auto(dev);
		if (!ret)
			break;
		if (attempt + 1 < attempts)
			msleep(100);
	}
	if (ret && teardown)
		dev_err(dev, "failed to restore both fans to AUTO after %d teardown attempts: %d\n",
			attempts, ret);
	data->auto_pending = !!ret;
	mutex_unlock(&data->lock);
	return ret;
}

static void nitro_ec_remove(struct platform_device *pdev)
{
	nitro_stop_fan_control(&pdev->dev, true);
}

static void nitro_ec_shutdown(struct platform_device *pdev)
{
	nitro_stop_fan_control(&pdev->dev, true);
}

static int nitro_ec_suspend(struct device *dev)
{
	struct nitro_ec_data *data = dev_get_drvdata(dev);
	int ret = nitro_stop_fan_control(dev, false);

	if (ret) {
		/* Suspend was rejected; keep recovery running on the awake device. */
		mutex_lock(&data->lock);
		data->writes_blocked = false;
		nitro_schedule_lease_work(data);
		mutex_unlock(&data->lock);
	}
	return ret;
}

static int nitro_ec_resume(struct device *dev)
{
	struct nitro_ec_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->lock);
	/* Recheck the feature quirk without restoring old leases or PWM modes. */
	ret = nitro_enable_fan_control(dev);
	if (!ret) {
		data->writes_blocked = false;
		nitro_schedule_lease_work(data);
	}
	mutex_unlock(&data->lock);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(nitro_ec_pm_ops,
			       nitro_ec_suspend, nitro_ec_resume);

static struct platform_driver nitro_ec_driver = {
	.probe  = nitro_ec_probe,
	.remove = nitro_ec_remove,
	.shutdown = nitro_ec_shutdown,
	.driver = {
		.name = DRIVER_NAME,
		.pm = pm_sleep_ptr(&nitro_ec_pm_ops),
	},
};

/* ------------------------------------------------------------------ */
/* Module init/exit — DMI-based device detection                       */
/* ------------------------------------------------------------------ */

static struct platform_device *nitro_pdev;

static int __init nitro_ec_init(void)
{
	const struct nitro_ec_regs *regs = NULL;
	const char *model;
	int ret;

	model = dmi_get_system_info(DMI_PRODUCT_NAME);
	if (!model)
		return -ENODEV;

	if (strstr(model, "AN515-44"))
		regs = &regs_an515_44;
	else if (strstr(model, "AN515-46") ||
		 strstr(model, "AN515-54") ||
		 strstr(model, "AN515-55") ||
		 strstr(model, "AN515-56") ||
		 strstr(model, "AN515-57") ||
		 strstr(model, "AN515-58") ||
		 strstr(model, "AN517-55"))
		regs = &regs_an515_46;

	if (!regs) {
		pr_info("unsupported model '%s' — not loading\n", model);
		return -ENODEV;
	}

	pr_info("detected '%s', loading driver\n", model);

	ret = platform_driver_register(&nitro_ec_driver);
	if (ret) {
		pr_err("platform_driver_register failed: %d\n", ret);
		return ret;
	}

	nitro_pdev = platform_device_register_data(
		NULL, DRIVER_NAME, PLATFORM_DEVID_NONE,
		regs, sizeof(*regs));

	if (IS_ERR(nitro_pdev)) {
		pr_err("platform_device_register failed: %ld\n",
		       PTR_ERR(nitro_pdev));
		platform_driver_unregister(&nitro_ec_driver);
		return PTR_ERR(nitro_pdev);
	}

	return 0;
}

static void __exit nitro_ec_exit(void)
{
	pr_info("unloading driver\n");
	platform_device_unregister(nitro_pdev);
	platform_driver_unregister(&nitro_ec_driver);
}

module_init(nitro_ec_init);
module_exit(nitro_ec_exit);

MODULE_AUTHOR("Linux-NitroSense contributors");
MODULE_DESCRIPTION("Acer Nitro AN515/AN517 EC fan control driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("dmi:*:svnAcer:pnNitroAN515*:");
MODULE_ALIAS("dmi:*:svnAcer:pnNitroAN517*:");
