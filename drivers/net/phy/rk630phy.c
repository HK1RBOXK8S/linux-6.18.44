// SPDX-License-Identifier: GPL-2.0+
/*
 * Rockchip RK630 10/100M Ethernet PHY 最小驱动
 *
 * 只保留上网所需：PHY 识别、S40/T22 板级初始化、自协商、链路读取。
 * 已裁剪：efuse TX 等级、长线/丢包自适应、10M 专项、WoL、温补、sysfs。
 */

#include <linux/kernel.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/phy.h>

#define RK630_PHY_ID		0x00441400
#define REG_PAGE_SEL		0x1f
#define REG_MMD_CTRL		0x0d
#define REG_MMD_DATA		0x0e

/*
 * 分页寄存器（页号 / 偏移）：
 * P0: 0x0d MMD 控制, 0x0e MMD 数据, 0x1f 页选择
 * P1: 0x10 偏移时钟, 0x12 APS, 0x13 UAPS, 0x17 EEE
 * P2: 0x18 AFE TRX
 * P6: 0x10 ADC, 0x12 增益, 0x13 AFE RX, 0x14 AFE TX,
 *     0x15 TX 驱动, 0x17 CP 电流, 0x18 ADC 偏置,
 *     0x19 RX 检测, 0x1b TX MOS, 0x1c PDCW
 * P8: 0x13 偏移校准, 0x18 AFE TRX, 0x1d 自动校准
 */

/* MDIO 地址同时区分板型 */
#define PHY_ADDR_S40		1
#define PHY_ADDR_T22		2

struct rk630_reg_val {
	u8 page;
	u8 reg;
	u16 val;
};

static int rk630_phy_select_page(struct phy_device *phydev, unsigned int page)
{
	return phy_write(phydev, REG_PAGE_SEL, page << 8);
}

/* 写指定页寄存器，完成后回到 PAGE0 */
static int rk630_phy_write_reg(struct phy_device *phydev, unsigned int page,
			       u32 reg, u16 val)
{
	int ret = rk630_phy_select_page(phydev, page);

	if (ret)
		return ret;

	ret = phy_write(phydev, reg, val);
	rk630_phy_select_page(phydev, 0);
	return ret;
}

static int rk630_phy_write_seq(struct phy_device *phydev,
			       const struct rk630_reg_val *seq, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = rk630_phy_write_reg(phydev, seq[i].page, seq[i].reg,
					  seq[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

static int rk630_phy_update_bits(struct phy_device *phydev, unsigned int page,
				 u32 reg, u32 mask, u32 val)
{
	int old, ret = rk630_phy_select_page(phydev, page);

	if (ret)
		return ret;

	old = phy_read(phydev, reg);
	ret = old < 0 ? old : phy_write(phydev, reg, (old & ~mask) | (val & mask));
	rk630_phy_select_page(phydev, 0);
	return ret;
}

static const struct rk630_reg_val rk630_s40_seq[] = {
	{ 1, 0x12, 0x4824 },	/* APS 关闭 */
	{ 2, 0x18, 0x0000 },	/* AFE TRX */
	{ 6, 0x14, 0x708f },	/* AFE TX */
	{ 6, 0x13, 0xf000 },	/* AFE RX */
	{ 6, 0x15, 0x1530 },	/* TX 驱动 */
	{ 8, 0x18, 0x00bc },	/* AFE TRX */
};

static const struct rk630_reg_val rk630_t22_seq[] = {
	{ 1, 0x10, 0xfbfe },	/* 偏移时钟 */
	{ 1, 0x12, 0x4822 },	/* APS 关闭，检测时间 2s */
	{ 2, 0x18, 0x0000 },	/* AFE TRX */
	{ 6, 0x10, 0x5540 },	/* ADC */
	{ 6, 0x12, 0x0400 },	/* 增益 */
	{ 6, 0x14, 0x10c8 },	/* RX 阈值 260mV */
	{ 6, 0x15, 0x2d32 },	/* TX 等级 100M/10M */
	{ 6, 0x17, 0x0575 },	/* CP 电流 */
	{ 6, 0x18, 0x0000 },	/* ADC 偏置 */
	{ 6, 0x19, 0x0408 },	/* RX 检测电平 */
	{ 6, 0x1c, 0x8880 },	/* PDCW */
	{ 6, 0x1b, 0x888e },	/* TX MOS 驱动 */
	{ 8, 0x1d, 0x0844 },	/* 关闭自动校准 */
	{ 8, 0x13, 0xc096 },	/* 重启偏移校准 */
	/* MMD 7.0x3c：关闭 EEE 通告 */
	{ 0, REG_MMD_CTRL, 0x0007 },
	{ 0, REG_MMD_DATA, 0x003c },
	{ 0, REG_MMD_CTRL, 0x4007 },
	{ 0, REG_MMD_DATA, 0x0000 },
};

static int rk630_phy_config_init(struct phy_device *phydev)
{
	int ret;

	switch (phydev->mdio.addr) {
	case PHY_ADDR_S40:
		ret = rk630_phy_update_bits(phydev, 0, MII_BMCR, BIT(13), 0);
		if (!ret)
			ret = rk630_phy_write_seq(phydev, rk630_s40_seq,
						  ARRAY_SIZE(rk630_s40_seq));
		if (!ret)	/* 开启 UAPS，空口省电 */
			ret = rk630_phy_update_bits(phydev, 1, 0x13,
						    BIT(15), BIT(15));
		break;
	case PHY_ADDR_T22:
		ret = rk630_phy_write_seq(phydev, rk630_t22_seq,
					  ARRAY_SIZE(rk630_t22_seq));
		if (!ret)	/* 关闭 UAPS */
			ret = rk630_phy_update_bits(phydev, 1, 0x13,
						    BIT(15), 0);
		break;
	default:
		return -EINVAL;
	}

	if (ret)
		return ret;

	/* 使能 EEE 模拟配置 */
	return rk630_phy_update_bits(phydev, 1, 0x17, BIT(3), BIT(3));
}

static struct phy_driver rk630_phy_driver[] = {
{
	.phy_id			= RK630_PHY_ID,
	.phy_id_mask		= 0xffffffff,
	.name			= "RK630 PHY",
	/* PHY_BASIC_FEATURES：6.18 已无 .features 字段，能力由 BMSR 自动读取 */
	.soft_reset		= genphy_soft_reset,
	.config_init		= rk630_phy_config_init,
	.config_aneg		= genphy_config_aneg,
	.read_status		= genphy_read_status,
	.suspend		= genphy_suspend,
	.resume			= genphy_resume,
},
};

static const struct mdio_device_id __maybe_unused rk630_phy_tbl[] = {
	{ RK630_PHY_ID, 0xffffffff },
	{ }
};

MODULE_DEVICE_TABLE(mdio, rk630_phy_tbl);

module_phy_driver(rk630_phy_driver);

MODULE_DESCRIPTION("Rockchip RK630 Ethernet PHY driver");
MODULE_LICENSE("GPL v2");
