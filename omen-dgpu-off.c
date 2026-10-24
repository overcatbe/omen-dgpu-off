// SPDX-License-Identifier: GPL-2.0
/*
 * omen-dgpu-off: cut NVIDIA dGPU power right before S5 on the
 * HP OMEN 16-ap0xxx (board 8E35).
 *
 * The firmware's GPU sleep hook \_SB.PCI0.GPTS is empty, and Linux
 * runtime-resumes every PCI device in device_shutdown(), which runs
 * PG00._ON. The PWEN GPIO then stays latched on in S5 and the dGPU
 * keeps drawing power with the laptop "off".
 *
 * syscore_shutdown() runs after device_shutdown(), just before the
 * firmware takes over, so PG00._OFF called from there sticks.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/syscore_ops.h>

#define PG00_PATH "\\_SB.PCI0.GPP0.PG00"

static acpi_handle pg00;

static unsigned long long pg00_sta(void)
{
	unsigned long long sta;

	if (ACPI_FAILURE(acpi_evaluate_integer(pg00, "_STA", NULL, &sta)))
		return ULLONG_MAX;
	return sta;
}

static void dgpu_off_shutdown(void *data)
{
	acpi_status status;
	unsigned long long before;

	if (system_state != SYSTEM_POWER_OFF)
		return;

	before = pg00_sta();
	status = acpi_evaluate_object(pg00, "_OFF", NULL, NULL);
	pr_notice("PG00._OFF: %s, _STA %llu -> %llu\n",
		  acpi_format_exception(status), before, pg00_sta());
}

static const struct syscore_ops dgpu_off_ops = {
	.shutdown = dgpu_off_shutdown,
};

static struct syscore dgpu_off_syscore = {
	.ops = &dgpu_off_ops,
};

static int __init dgpu_off_init(void)
{
	if (!dmi_match(DMI_BOARD_VENDOR, "HP") ||
	    !dmi_match(DMI_BOARD_NAME, "8E35"))
		return -ENODEV;

	if (ACPI_FAILURE(acpi_get_handle(NULL, PG00_PATH, &pg00))) {
		pr_err("%s not found\n", PG00_PATH);
		return -ENODEV;
	}

	register_syscore(&dgpu_off_syscore);
	pr_info("armed, PG00._STA=%llu\n", pg00_sta());
	return 0;
}

static void __exit dgpu_off_exit(void)
{
	unregister_syscore(&dgpu_off_syscore);
}

module_init(dgpu_off_init);
module_exit(dgpu_off_exit);

MODULE_DESCRIPTION("Power off dGPU before S5 on HP OMEN 16-ap0xxx (empty GPTS workaround)");
MODULE_LICENSE("GPL");
