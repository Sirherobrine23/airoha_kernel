/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __LINUX_PCS_AIROHA_H
#define __LINUX_PCS_AIROHA_H

struct phylink_pcs *airoha_pcs_create(struct device *dev);
void airoha_pcs_destroy(struct phylink_pcs *pcs);

#endif /* __LINUX_PCS_AIROHA_H */
