/* Copyright Altera Corporation (C) 2014. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Adopted from dwmac-sti.c
 */

#if defined CONFIG_HAVE_ARM_SMCCC && defined CONFIG_ARCH_STRATIX10
#include <linux/arm-smccc.h>
#endif
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/stmmac.h>

#include "stmmac.h"
#include "stmmac_platform.h"

#include "altr_tse_pcs.h"

#define SGMII_ADAPTER_CTRL_REG                          0x00
#define SGMII_ADAPTER_DISABLE                           0x0001

#define SYSMGR_EMACGRP_CTRL_PHYSEL_ENUM_GMII_MII 0x0
#define SYSMGR_EMACGRP_CTRL_PHYSEL_ENUM_RGMII 0x1
#define SYSMGR_EMACGRP_CTRL_PHYSEL_ENUM_RMII 0x2
#define SYSMGR_EMACGRP_CTRL_PHYSEL_WIDTH 2
#define SYSMGR_EMACGRP_CTRL_PHYSEL_MASK 0x00000003
#define SYSMGR_EMACGRP_CTRL_PTP_REF_CLK_MASK 0x00000010

#define SYSMGR_FPGAGRP_MODULE_REG  0x00000028
#define SYSMGR_FPGAGRP_MODULE_EMAC 0x00000004

#define EMAC_SPLITTER_CTRL_REG			0x0
#define EMAC_SPLITTER_CTRL_SPEED_MASK		0x3
#define EMAC_SPLITTER_CTRL_SPEED_10		0x2
#define EMAC_SPLITTER_CTRL_SPEED_100		0x3
#define EMAC_SPLITTER_CTRL_SPEED_1000		0x0

struct socfpga_dwmac {
	int	interface;
	u32	reg_offset;
	u32	reg_shift;
#if defined CONFIG_HAVE_ARM_SMCCC && defined CONFIG_ARCH_STRATIX10
	u32	sysmgr_reg;
#endif
	struct	device *dev;
	struct regmap *sys_mgr_base_addr;
	struct reset_control *stmmac_rst;
	struct reset_control *stmmac_ocp_rst;
	void __iomem *splitter_base;
	bool f2h_ptp_ref_clk;
	struct tse_pcs pcs;
};

#if defined CONFIG_HAVE_ARM_SMCCC && defined CONFIG_ARCH_STRATIX10
/* Functions specified by ARM SMC Calling convention:
 *
 * FAST call executes atomic operations, returns when the requested operation
 * has completed.
 * STD call starts a operation which can be preempted by a non-secure
 * interrupt. The call can return before the requested operation has completed.
 * a0..a7 is used as register names in the descriptions below, on arm32 that
 * translates to r0..r7 and on arm64 to w0..w7.
 */

#define INTEL_SIP_SMC_STD_CALL_VAL(func_num) \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_STD_CALL, ARM_SMCCC_SMC_64, \
	ARM_SMCCC_OWNER_SIP, (func_num))

#define INTEL_SIP_SMC_FAST_CALL_VAL(func_num) \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64, \
	ARM_SMCCC_OWNER_SIP, (func_num))

#define INTEL_SIP_SMC_RETURN_UNKNOWN_FUNCTION		0xFFFFFFFF
#define INTEL_SIP_SMC_STATUS_OK				0x0
#define INTEL_SIP_SMC_REG_ERROR				0x5

/* Request INTEL_SIP_SMC_REG_READ
 *
 * Read a protected register using SMCCC
 *
 * Call register usage:
 * a0: INTEL_SIP_SMC_REG_READ.
 * a1: register address.
 * a2-7: not used.
 *
 * Return status:
 * a0: INTEL_SIP_SMC_STATUS_OK, INTEL_SIP_SMC_REG_ERROR, or
 *     INTEL_SIP_SMC_RETURN_UNKNOWN_FUNCTION
 * a1: Value in the register
 * a2-3: not used.
 */
#define INTEL_SIP_SMC_FUNCID_REG_READ 7
#define INTEL_SIP_SMC_REG_READ \
	INTEL_SIP_SMC_FAST_CALL_VAL(INTEL_SIP_SMC_FUNCID_REG_READ)

/* Request INTEL_SIP_SMC_REG_WRITE
 *
 * Write a protected register using SMCCC
 *
 * Call register usage:
 * a0: INTEL_SIP_SMC_REG_WRITE.
 * a1: register address
 * a2: value to program into register.
 * a3-7: not used.
 *
 * Return status:
 * a0: INTEL_SIP_SMC_STATUS_OK, INTEL_SIP_SMC_REG_ERROR, or
 *     INTEL_SIP_SMC_RETURN_UNKNOWN_FUNCTION
 * a1-3: not used.
 */
#define INTEL_SIP_SMC_FUNCID_REG_WRITE 8
#define INTEL_SIP_SMC_REG_WRITE \
	INTEL_SIP_SMC_FAST_CALL_VAL(INTEL_SIP_SMC_FUNCID_REG_WRITE)

/**************** Stratix 10 EMAC Memory Controller Functions ************/

/* s10_protected_reg_write
 * Write to a protected SMC register.
 * @context: Not used
 * @reg: Address of register
 * @value: Value to write
 * Return: INTEL_SIP_SMC_STATUS_OK (0) on success
 *         INTEL_SIP_SMC_REG_ERROR on error
 *         INTEL_SIP_SMC_RETURN_UNKNOWN_FUNCTION if not supported
 */
static int s10_protected_reg_write(void *context, unsigned int reg,
				   unsigned int val)
{
	struct arm_smccc_res result;

	arm_smccc_smc(INTEL_SIP_SMC_REG_WRITE, reg, val, 0, 0,
		      0, 0, 0, &result);

	return (int)result.a0;
}

/* s10_protected_reg_read
 * Read the status of a protected SMC register
 * @context: Not used
 * @reg: Address of register
 * @value: Value read.
 * Return: INTEL_SIP_SMC_STATUS_OK (0) on success
 *         INTEL_SIP_SMC_REG_ERROR on error
 *         INTEL_SIP_SMC_RETURN_UNKNOWN_FUNCTION if not supported
 */
static int s10_protected_reg_read(void *context, unsigned int reg,
				  unsigned int *val)
{
	struct arm_smccc_res result;

	arm_smccc_smc(INTEL_SIP_SMC_REG_READ, reg, 0, 0, 0,
		      0, 0, 0, &result);

	*val = (unsigned int)result.a1;

	return (int)result.a0;
}

static const struct regmap_config s10_emac_regmap_cfg = {
	.name = "s10_emac",
	.reg_bits = 32,
	.val_bits = 32,
	.max_register = 0xffffffff,
	.reg_read = s10_protected_reg_read,
	.reg_write = s10_protected_reg_write,
	.use_single_read = true,
	.use_single_write = true,
};
#endif

static void socfpga_dwmac_fix_mac_speed(void *priv, unsigned int speed)
{
	struct socfpga_dwmac *dwmac = (struct socfpga_dwmac *)priv;
	void __iomem *splitter_base = dwmac->splitter_base;
	void __iomem *tse_pcs_base = dwmac->pcs.tse_pcs_base;
	void __iomem *sgmii_adapter_base = dwmac->pcs.sgmii_adapter_base;
	struct device *dev = dwmac->dev;
	struct net_device *ndev = dev_get_drvdata(dev);
	struct phy_device *phy_dev = ndev->phydev;
	u32 val;

	if ((tse_pcs_base) && (sgmii_adapter_base))
		writew(SGMII_ADAPTER_DISABLE,
		       sgmii_adapter_base + SGMII_ADAPTER_CTRL_REG);

	if (splitter_base) {
		val = readl(splitter_base + EMAC_SPLITTER_CTRL_REG);
		val &= ~EMAC_SPLITTER_CTRL_SPEED_MASK;

		switch (speed) {
		case 1000:
			val |= EMAC_SPLITTER_CTRL_SPEED_1000;
			break;
		case 100:
			val |= EMAC_SPLITTER_CTRL_SPEED_100;
			break;
		case 10:
			val |= EMAC_SPLITTER_CTRL_SPEED_10;
			break;
		default:
			return;
		}
		writel(val, splitter_base + EMAC_SPLITTER_CTRL_REG);
	}

	if (tse_pcs_base && sgmii_adapter_base)
		tse_pcs_fix_mac_speed(&dwmac->pcs, phy_dev, speed);
}

static int socfpga_dwmac_parse_data(struct socfpga_dwmac *dwmac, struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct regmap *sys_mgr_base_addr;
	u32 reg_offset, reg_shift;
#if defined CONFIG_HAVE_ARM_SMCCC && defined CONFIG_ARCH_STRATIX10
	u32 sysmgr_reg = 0;
#endif
	int ret, index;
	struct device_node *np_splitter = NULL;
	struct device_node *np_sgmii_adapter = NULL;
#if defined CONFIG_HAVE_ARM_SMCCC && defined CONFIG_ARCH_STRATIX10
	struct device_node *np_sysmgr = NULL;
#endif
	struct resource res_splitter;
	struct resource res_tse_pcs;
	struct resource res_sgmii_adapter;

	dwmac->interface = of_get_phy_mode(np);

#if defined CONFIG_HAVE_ARM_SMCCC && defined CONFIG_ARCH_STRATIX10
	sys_mgr_base_addr = devm_regmap_init(dev, NULL, (void *)dwmac,
					     &s10_emac_regmap_cfg);
	if (IS_ERR(sys_mgr_base_addr))
		return PTR_ERR(sys_mgr_base_addr);

	np_sysmgr = of_parse_phandle(np, "altr,sysmgr-syscon", 0);
	if (np_sysmgr) {
		ret = of_property_read_u32_index(np_sysmgr, "reg", 0,
						 &sysmgr_reg);
		if (ret) {
			dev_info(dev, "Could not read sysmgr register address\n");
			return -EINVAL;
		}
	}
#else
	sys_mgr_base_addr = syscon_regmap_lookup_by_phandle(np, "altr,sysmgr-syscon");
	if (IS_ERR(sys_mgr_base_addr)) {
		dev_info(dev, "No sysmgr-syscon node found\n");
		return PTR_ERR(sys_mgr_base_addr);
	}
#endif

	ret = of_property_read_u32_index(np, "altr,sysmgr-syscon", 1, &reg_offset);
	if (ret) {
		dev_info(dev, "Could not read reg_offset from sysmgr-syscon!\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_index(np, "altr,sysmgr-syscon", 2, &reg_shift);
	if (ret) {
		dev_info(dev, "Could not read reg_shift from sysmgr-syscon!\n");
		return -EINVAL;
	}

	dwmac->f2h_ptp_ref_clk = of_property_read_bool(np, "altr,f2h_ptp_ref_clk");

	np_splitter = of_parse_phandle(np, "altr,emac-splitter", 0);
	if (np_splitter) {
		ret = of_address_to_resource(np_splitter, 0, &res_splitter);
		of_node_put(np_splitter);
		if (ret) {
			dev_info(dev, "Missing emac splitter address\n");
			return -EINVAL;
		}

		dwmac->splitter_base = devm_ioremap_resource(dev, &res_splitter);
		if (IS_ERR(dwmac->splitter_base)) {
			dev_info(dev, "Failed to mapping emac splitter\n");
			return PTR_ERR(dwmac->splitter_base);
		}
	}

	np_sgmii_adapter = of_parse_phandle(np,
					    "altr,gmii-to-sgmii-converter", 0);
	if (np_sgmii_adapter) {
		index = of_property_match_string(np_sgmii_adapter, "reg-names",
						 "hps_emac_interface_splitter_avalon_slave");

		if (index >= 0) {
			if (of_address_to_resource(np_sgmii_adapter, index,
						   &res_splitter)) {
				dev_err(dev,
					"%s: ERROR: missing emac splitter address\n",
					__func__);
				ret = -EINVAL;
				goto err_node_put;
			}

			dwmac->splitter_base =
			    devm_ioremap_resource(dev, &res_splitter);

			if (IS_ERR(dwmac->splitter_base)) {
				ret = PTR_ERR(dwmac->splitter_base);
				goto err_node_put;
			}
		}

		index = of_property_match_string(np_sgmii_adapter, "reg-names",
						 "gmii_to_sgmii_adapter_avalon_slave");

		if (index >= 0) {
			if (of_address_to_resource(np_sgmii_adapter, index,
						   &res_sgmii_adapter)) {
				dev_err(dev,
					"%s: ERROR: failed mapping adapter\n",
					__func__);
				ret = -EINVAL;
				goto err_node_put;
			}

			dwmac->pcs.sgmii_adapter_base =
			    devm_ioremap_resource(dev, &res_sgmii_adapter);

			if (IS_ERR(dwmac->pcs.sgmii_adapter_base)) {
				ret = PTR_ERR(dwmac->pcs.sgmii_adapter_base);
				goto err_node_put;
			}
		}

		index = of_property_match_string(np_sgmii_adapter, "reg-names",
						 "eth_tse_control_port");

		if (index >= 0) {
			if (of_address_to_resource(np_sgmii_adapter, index,
						   &res_tse_pcs)) {
				dev_err(dev,
					"%s: ERROR: failed mapping tse control port\n",
					__func__);
				ret = -EINVAL;
				goto err_node_put;
			}

			dwmac->pcs.tse_pcs_base =
			    devm_ioremap_resource(dev, &res_tse_pcs);

			if (IS_ERR(dwmac->pcs.tse_pcs_base)) {
				ret = PTR_ERR(dwmac->pcs.tse_pcs_base);
				goto err_node_put;
			}
		}
	}
	dwmac->reg_offset = reg_offset;
	dwmac->reg_shift = reg_shift;
	dwmac->sys_mgr_base_addr = sys_mgr_base_addr;
#if defined CONFIG_HAVE_ARM_SMCCC && defined CONFIG_ARCH_STRATIX10
	dwmac->sysmgr_reg = sysmgr_reg;
#endif
	dwmac->dev = dev;
	of_node_put(np_sgmii_adapter);

	return 0;

err_node_put:
	of_node_put(np_sgmii_adapter);
	return ret;
}

static int socfpga_dwmac_set_phy_mode(struct socfpga_dwmac *dwmac)
{
	struct regmap *sys_mgr_base_addr = dwmac->sys_mgr_base_addr;
	int phymode = dwmac->interface;
	u32 reg_offset = dwmac->reg_offset;
	u32 reg_shift = dwmac->reg_shift;
#if defined CONFIG_HAVE_ARM_SMCCC && defined CONFIG_ARCH_STRATIX10
	u32 sysmgr_reg = dwmac->sysmgr_reg;
#endif
	u32 ctrl, val, module;

	switch (phymode) {
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
		val = SYSMGR_EMACGRP_CTRL_PHYSEL_ENUM_RGMII;
		break;
	case PHY_INTERFACE_MODE_MII:
	case PHY_INTERFACE_MODE_GMII:
	case PHY_INTERFACE_MODE_SGMII:
		val = SYSMGR_EMACGRP_CTRL_PHYSEL_ENUM_GMII_MII;
		break;
	default:
		dev_err(dwmac->dev, "bad phy mode %d\n", phymode);
		return -EINVAL;
	}

	/* Overwrite val to GMII if splitter core is enabled. The phymode here
	 * is the actual phy mode on phy hardware, but phy interface from
	 * EMAC core is GMII.
	 */
	if (dwmac->splitter_base)
		val = SYSMGR_EMACGRP_CTRL_PHYSEL_ENUM_GMII_MII;

	/* Assert reset to the enet controller before changing the phy mode */
	reset_control_assert(dwmac->stmmac_ocp_rst);
	reset_control_assert(dwmac->stmmac_rst);

#if defined CONFIG_HAVE_ARM_SMCCC && defined CONFIG_ARCH_STRATIX10
	regmap_read(sys_mgr_base_addr, sysmgr_reg + reg_offset, &ctrl);
#else
	regmap_read(sys_mgr_base_addr, reg_offset, &ctrl);
#endif
	ctrl &= ~(SYSMGR_EMACGRP_CTRL_PHYSEL_MASK << reg_shift);
	ctrl |= val << reg_shift;

	if (dwmac->f2h_ptp_ref_clk ||
	    phymode == PHY_INTERFACE_MODE_MII ||
	    phymode == PHY_INTERFACE_MODE_GMII ||
	    phymode == PHY_INTERFACE_MODE_SGMII) {
		ctrl |= SYSMGR_EMACGRP_CTRL_PTP_REF_CLK_MASK << (reg_shift / 2);
		regmap_read(sys_mgr_base_addr, SYSMGR_FPGAGRP_MODULE_REG,
			    &module);
		module |= (SYSMGR_FPGAGRP_MODULE_EMAC << (reg_shift / 2));
		regmap_write(sys_mgr_base_addr, SYSMGR_FPGAGRP_MODULE_REG,
			     module);
	} else {
		ctrl &= ~(SYSMGR_EMACGRP_CTRL_PTP_REF_CLK_MASK << (reg_shift / 2));
	}

#if defined CONFIG_HAVE_ARM_SMCCC && defined CONFIG_ARCH_STRATIX10
	regmap_write(sys_mgr_base_addr, sysmgr_reg + reg_offset, ctrl);
#else
	regmap_write(sys_mgr_base_addr, reg_offset, ctrl);
#endif

	/* Deassert reset for the phy configuration to be sampled by
	 * the enet controller, and operation to start in requested mode
	 */
	reset_control_deassert(dwmac->stmmac_ocp_rst);
	reset_control_deassert(dwmac->stmmac_rst);
	if (phymode == PHY_INTERFACE_MODE_SGMII) {
		if (tse_pcs_init(dwmac->pcs.tse_pcs_base, &dwmac->pcs) != 0) {
			dev_err(dwmac->dev, "Unable to initialize TSE PCS");
			return -EINVAL;
		}
	}

	return 0;
}

static int socfpga_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct device		*dev = &pdev->dev;
	int			ret;
	struct socfpga_dwmac	*dwmac;
	struct net_device	*ndev;
	struct stmmac_priv	*stpriv;

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = stmmac_probe_config_dt(pdev, &stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	dwmac = devm_kzalloc(dev, sizeof(*dwmac), GFP_KERNEL);
	if (!dwmac) {
		ret = -ENOMEM;
		goto err_remove_config_dt;
	}

	dwmac->stmmac_ocp_rst = devm_reset_control_get_optional(dev, "stmmaceth-ocp");
	if (IS_ERR(dwmac->stmmac_ocp_rst)) {
		ret = PTR_ERR(dwmac->stmmac_ocp_rst);
		dev_err(dev, "error getting reset control of ocp %d\n", ret);
		goto err_remove_config_dt;
	}

	reset_control_deassert(dwmac->stmmac_ocp_rst);

	ret = socfpga_dwmac_parse_data(dwmac, dev);
	if (ret) {
		dev_err(dev, "Unable to parse OF data\n");
		goto err_remove_config_dt;
	}

	plat_dat->bsp_priv = dwmac;
	plat_dat->fix_mac_speed = socfpga_dwmac_fix_mac_speed;

	ret = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (ret)
		goto err_remove_config_dt;

	ndev = platform_get_drvdata(pdev);
	stpriv = netdev_priv(ndev);

	/* The socfpga driver needs to control the stmmac reset to set the phy
	 * mode. Create a copy of the core reset handle so it can be used by
	 * the driver later.
	 */
	dwmac->stmmac_rst = stpriv->plat->stmmac_rst;

	ret = socfpga_dwmac_set_phy_mode(dwmac);
	if (ret)
		goto err_dvr_remove;

	return 0;

err_dvr_remove:
	stmmac_dvr_remove(&pdev->dev);
err_remove_config_dt:
	stmmac_remove_config_dt(pdev, plat_dat);

	return ret;
}

#ifdef CONFIG_PM_SLEEP
static int socfpga_dwmac_resume(struct device *dev)
{
	struct net_device *ndev = dev_get_drvdata(dev);
	struct stmmac_priv *priv = netdev_priv(ndev);

	socfpga_dwmac_set_phy_mode(priv->plat->bsp_priv);

	/* Before the enet controller is suspended, the phy is suspended.
	 * This causes the phy clock to be gated. The enet controller is
	 * resumed before the phy, so the clock is still gated "off" when
	 * the enet controller is resumed. This code makes sure the phy
	 * is "resumed" before reinitializing the enet controller since
	 * the enet controller depends on an active phy clock to complete
	 * a DMA reset. A DMA reset will "time out" if executed
	 * with no phy clock input on the Synopsys enet controller.
	 * Verified through Synopsys Case #8000711656.
	 *
	 * Note that the phy clock is also gated when the phy is isolated.
	 * Phy "suspend" and "isolate" controls are located in phy basic
	 * control register 0, and can be modified by the phy driver
	 * framework.
	 */
	if (ndev->phydev)
		phy_resume(ndev->phydev);

	return stmmac_resume(dev);
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(socfpga_dwmac_pm_ops, stmmac_suspend,
					       socfpga_dwmac_resume);

static const struct of_device_id socfpga_dwmac_match[] = {
	{ .compatible = "altr,socfpga-stmmac" },
	{ }
};
MODULE_DEVICE_TABLE(of, socfpga_dwmac_match);

static struct platform_driver socfpga_dwmac_driver = {
	.probe  = socfpga_dwmac_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name           = "socfpga-dwmac",
		.pm		= &socfpga_dwmac_pm_ops,
		.of_match_table = socfpga_dwmac_match,
	},
};
module_platform_driver(socfpga_dwmac_driver);

MODULE_LICENSE("GPL v2");
