/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ETA6937 register definitions, retained as hl7005.h for existing
 * MediaTek build integration.
 */

#ifndef _HL7005_SW_H_
#define _HL7005_SW_H_

#define HL7005_CON0			0x00
#define HL7005_CON1			0x01
#define HL7005_CON2			0x02
#define HL7005_CON3			0x03
#define HL7005_CON4			0x04
#define HL7005_CON5			0x05
#define HL7005_CON6			0x06
#define HL7005_CON7			0x07

#define HL7005_REG_NUM			8

/* REG00: status/control */
#define CON0_TMR_RST_MASK		0x01
#define CON0_TMR_RST_SHIFT		7
#define CON0_OTG_MASK			0x01
#define CON0_OTG_SHIFT			7
#define CON0_EN_STAT_MASK		0x01
#define CON0_EN_STAT_SHIFT		6
#define CON0_STAT_MASK			0x03
#define CON0_STAT_SHIFT			4
#define CON0_BOOST_MASK			0x01
#define CON0_BOOST_SHIFT		3
#define CON0_FAULT_MASK			0x07
#define CON0_FAULT_SHIFT		0

/* REG01: charger control and legacy input-current limit */
#define CON1_LIN_LIMIT_MASK		0x03
#define CON1_LIN_LIMIT_SHIFT		6
#define CON1_LOW_V_MASK			0x03
#define CON1_LOW_V_SHIFT		4
#define CON1_TE_MASK			0x01
#define CON1_TE_SHIFT			3
#define CON1_CE_MASK			0x01
#define CON1_CE_SHIFT			2
#define CON1_HZ_MODE_MASK		0x01
#define CON1_HZ_MODE_SHIFT		1
#define CON1_OPA_MODE_MASK		0x01
#define CON1_OPA_MODE_SHIFT		0

/* REG02: battery regulation voltage and OTG control */
#define CON2_OREG_MASK			0x3f
#define CON2_OREG_SHIFT			2
#define CON2_OTG_PL_MASK		0x01
#define CON2_OTG_PL_SHIFT		1
#define CON2_OTG_EN_MASK		0x01
#define CON2_OTG_EN_SHIFT		0

/* REG03: vendor, part number, and revision */
#define CON3_VENDOR_CODE_MASK		0x07
#define CON3_VENDOR_CODE_SHIFT		5
#define CON3_PN_MASK			0x03
#define CON3_PN_SHIFT			3
#define CON3_REVISION_MASK		0x07
#define CON3_REVISION_SHIFT		0

/* Compatibility spelling retained for old code. */
#define CON3_VENDER_CODE_MASK		CON3_VENDOR_CODE_MASK
#define CON3_VENDER_CODE_SHIFT		CON3_VENDOR_CODE_SHIFT
#define CON3_PIN_MASK			CON3_PN_MASK
#define CON3_PIN_SHIFT			CON3_PN_SHIFT

/* REG04: charge-current low bits, offset, and termination current */
#define CON4_RESET_MASK			0x01
#define CON4_RESET_SHIFT		7
#define CON4_I_CHR_MASK			0x07
#define CON4_I_CHR_SHIFT		4
#define CON4_I_CHR_OFFSET_MASK		0x01
#define CON4_I_CHR_OFFSET_SHIFT		3
#define CON4_I_TERM_MASK		0x07
#define CON4_I_TERM_SHIFT		0

/* REG05: charge-current high bits, status, and VINDPM low bits */
#define CON5_I_CHR_HI_MASK		0x03
#define CON5_I_CHR_HI_SHIFT		6
#define CON5_LOW_CHG_MASK		0x01
#define CON5_LOW_CHG_SHIFT		5
#define CON5_DPM_STATUS_MASK		0x01
#define CON5_DPM_STATUS_SHIFT		4
#define CON5_CD_STATUS_MASK		0x01
#define CON5_CD_STATUS_SHIFT		3
#define CON5_VINDPM_LO_MASK		0x07
#define CON5_VINDPM_LO_SHIFT		0

/* REG06: write-once maximum current and voltage limits */
#define CON6_ISAFE_MASK			0x0f
#define CON6_ISAFE_SHIFT		4
#define CON6_VSAFE_MASK			0x0f
#define CON6_VSAFE_SHIFT		0

/* REG07: VINDPM high bits and extended input-current limit */
#define CON7_VINDPM_HI_MASK		0x0f
#define CON7_VINDPM_HI_SHIFT		4
#define CON7_EN_ILIM2_MASK		0x01
#define CON7_EN_ILIM2_SHIFT		3
#define CON7_IIN_LIMIT2_MASK		0x07
#define CON7_IIN_LIMIT2_SHIFT		0

#endif /* _HL7005_SW_H_ */
