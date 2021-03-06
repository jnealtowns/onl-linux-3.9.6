/*
* Driver for EHCI HCD on SPEAR SOC
*
* Copyright (C) 2010 ST Micro Electronics,
* Deepak Sikri <deepak.sikri@st.com>
*
* Based on various ehci-*.c drivers
*
* This file is subject to the terms and conditions of the GNU General Public
* License. See the file COPYING in the main directory of this archive for
* more details.
*/

#include <linux/clk.h>
#include <linux/jiffies.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>

struct spear_ehci {
	struct ehci_hcd ehci;
	struct clk *clk;
};

#define to_spear_ehci(hcd)	(struct spear_ehci *)hcd_to_ehci(hcd)

static void spear_start_ehci(struct spear_ehci *ehci)
{
	clk_prepare_enable(ehci->clk);
}

static void spear_stop_ehci(struct spear_ehci *ehci)
{
	clk_disable_unprepare(ehci->clk);
}

static int ehci_spear_setup(struct usb_hcd *hcd)
{
	struct ehci_hcd *ehci = hcd_to_ehci(hcd);

	/* registers start at offset 0x0 */
	ehci->caps = hcd->regs;

	return ehci_setup(hcd);
}

static const struct hc_driver ehci_spear_hc_driver = {
	.description			= hcd_name,
	.product_desc			= "SPEAr EHCI",
	.hcd_priv_size			= sizeof(struct spear_ehci),

	/* generic hardware linkage */
	.irq				= ehci_irq,
	.flags				= HCD_MEMORY | HCD_USB2,

	/* basic lifecycle operations */
	.reset				= ehci_spear_setup,
	.start				= ehci_run,
	.stop				= ehci_stop,
	.shutdown			= ehci_shutdown,

	/* managing i/o requests and associated device resources */
	.urb_enqueue			= ehci_urb_enqueue,
	.urb_dequeue			= ehci_urb_dequeue,
	.endpoint_disable		= ehci_endpoint_disable,
	.endpoint_reset			= ehci_endpoint_reset,

	/* scheduling support */
	.get_frame_number		= ehci_get_frame,

	/* root hub support */
	.hub_status_data		= ehci_hub_status_data,
	.hub_control			= ehci_hub_control,
	.bus_suspend			= ehci_bus_suspend,
	.bus_resume			= ehci_bus_resume,
	.relinquish_port		= ehci_relinquish_port,
	.port_handed_over		= ehci_port_handed_over,
	.clear_tt_buffer_complete	= ehci_clear_tt_buffer_complete,
};

#ifdef CONFIG_PM
static int ehci_spear_drv_suspend(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);
	bool do_wakeup = device_may_wakeup(dev);

	return ehci_suspend(hcd, do_wakeup);
}

static int ehci_spear_drv_resume(struct device *dev)
{
	struct usb_hcd *hcd = dev_get_drvdata(dev);

	ehci_resume(hcd, false);
	return 0;
}
#endif /* CONFIG_PM */

static SIMPLE_DEV_PM_OPS(ehci_spear_pm_ops, ehci_spear_drv_suspend,
		ehci_spear_drv_resume);

static u64 spear_ehci_dma_mask = DMA_BIT_MASK(32);

static int spear_ehci_hcd_drv_probe(struct platform_device *pdev)
{
	struct usb_hcd *hcd ;
	struct spear_ehci *ehci;
	struct resource *res;
	struct clk *usbh_clk;
	const struct hc_driver *driver = &ehci_spear_hc_driver;
	int irq, retval;

	if (usb_disabled())
		return -ENODEV;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		retval = irq;
		goto fail;
	}

	/*
	 * Right now device-tree probed devices don't get dma_mask set.
	 * Since shared usb code relies on it, set it here for now.
	 * Once we have dma capability bindings this can go away.
	 */
	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &spear_ehci_dma_mask;

	usbh_clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(usbh_clk)) {
		dev_err(&pdev->dev, "Error getting interface clock\n");
		retval = PTR_ERR(usbh_clk);
		goto fail;
	}

	hcd = usb_create_hcd(driver, &pdev->dev, dev_name(&pdev->dev));
	if (!hcd) {
		retval = -ENOMEM;
		goto fail;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		retval = -ENODEV;
		goto err_put_hcd;
	}

	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);
	if (!devm_request_mem_region(&pdev->dev, hcd->rsrc_start, hcd->rsrc_len,
				driver->description)) {
		retval = -EBUSY;
		goto err_put_hcd;
	}

	hcd->regs = devm_ioremap(&pdev->dev, hcd->rsrc_start, hcd->rsrc_len);
	if (hcd->regs == NULL) {
		dev_dbg(&pdev->dev, "error mapping memory\n");
		retval = -ENOMEM;
		goto err_put_hcd;
	}

	ehci = (struct spear_ehci *)hcd_to_ehci(hcd);
	ehci->clk = usbh_clk;

	spear_start_ehci(ehci);
	retval = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (retval)
		goto err_stop_ehci;

	return retval;

err_stop_ehci:
	spear_stop_ehci(ehci);
err_put_hcd:
	usb_put_hcd(hcd);
fail:
	dev_err(&pdev->dev, "init fail, %d\n", retval);

	return retval ;
}

static int spear_ehci_hcd_drv_remove(struct platform_device *pdev)
{
	struct usb_hcd *hcd = platform_get_drvdata(pdev);
	struct spear_ehci *ehci_p = to_spear_ehci(hcd);

	if (!hcd)
		return 0;
	if (in_interrupt())
		BUG();
	usb_remove_hcd(hcd);

	if (ehci_p->clk)
		spear_stop_ehci(ehci_p);
	usb_put_hcd(hcd);

	return 0;
}

static struct of_device_id spear_ehci_id_table[] = {
	{ .compatible = "st,spear600-ehci", },
	{ },
};

static struct platform_driver spear_ehci_hcd_driver = {
	.probe		= spear_ehci_hcd_drv_probe,
	.remove		= spear_ehci_hcd_drv_remove,
	.shutdown	= usb_hcd_platform_shutdown,
	.driver		= {
		.name = "spear-ehci",
		.bus = &platform_bus_type,
		.pm = &ehci_spear_pm_ops,
		.of_match_table = of_match_ptr(spear_ehci_id_table),
	}
};

MODULE_ALIAS("platform:spear-ehci");
