// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2020 Felix Fietkau <nbd@nbd.name> */

#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/soc/mediatek/mtk_wed.h>

const struct mtk_wed_ops __rcu *mtk_soc_wed_ops;
EXPORT_SYMBOL_GPL(mtk_soc_wed_ops);

static DEFINE_MUTEX(mtk_wed_ops_mutex);

int mtk_wed_ops_register(const struct mtk_wed_ops *ops)
{
	const struct mtk_wed_ops *cur;
	int ret = 0;

	mutex_lock(&mtk_wed_ops_mutex);
	cur = rcu_dereference_protected(mtk_soc_wed_ops,
					lockdep_is_held(&mtk_wed_ops_mutex));
	if (cur && cur != ops)
		ret = -EBUSY;
	else if (!cur)
		rcu_assign_pointer(mtk_soc_wed_ops, ops);
	mutex_unlock(&mtk_wed_ops_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mtk_wed_ops_register);

void mtk_wed_ops_unregister(const struct mtk_wed_ops *ops)
{
	const struct mtk_wed_ops *cur;
	bool removed = false;

	mutex_lock(&mtk_wed_ops_mutex);
	cur = rcu_dereference_protected(mtk_soc_wed_ops,
					lockdep_is_held(&mtk_wed_ops_mutex));
	if (cur == ops) {
		RCU_INIT_POINTER(mtk_soc_wed_ops, NULL);
		removed = true;
	}
	mutex_unlock(&mtk_wed_ops_mutex);

	if (removed)
		synchronize_rcu();
}
EXPORT_SYMBOL_GPL(mtk_wed_ops_unregister);
