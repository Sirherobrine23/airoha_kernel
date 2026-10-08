/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XDSL_INTERNAL_H
#define _AIROHA_XDSL_INTERNAL_H

#include <linux/bitops.h>
#include <linux/ethtool.h>
#include <linux/math64.h>

#include "airoha_xdsl.h"

static inline int
airoha_xdsl_validate_line(const struct airoha_xdsl_line_state *state)
{
	if (state->status > AIROHA_XDSL_LINE_FAILED ||
	    state->status < AIROHA_XDSL_LINE_DOWN || state->error > 0)
		return -EINVAL;
	if (state->status != AIROHA_XDSL_LINE_SHOWTIME)
		return 0;
	if (state->transport != AIROHA_XDSL_TRANSPORT_PTM)
		return -EOPNOTSUPP;
	if (state->error || !state->bearer_mask ||
	    (state->bearer_mask & ~GENMASK(1, 0)) || state->tx_bearer > 1 ||
	    !(state->bearer_mask & BIT(state->tx_bearer)) ||
	    !state->downstream_bps || !state->upstream_bps ||
	    div_u64(state->downstream_bps, 1000000) >= (u32)SPEED_UNKNOWN)
		return -EINVAL;
	return 0;
}

static inline u8 airoha_xdsl_path_mask(u8 bearer_mask)
{
	u8 paths = 0;

	if (bearer_mask & BIT(0))
		paths |= GENMASK(1, 0);
	if (bearer_mask & BIT(1))
		paths |= GENMASK(3, 2);
	return paths;
}

static inline bool
airoha_xdsl_same_line(const struct airoha_xdsl_line_state *a,
		      const struct airoha_xdsl_line_state *b)
{
	return a->status == b->status && a->transport == b->transport &&
	       a->bearer_mask == b->bearer_mask && a->tx_bearer == b->tx_bearer &&
	       a->downstream_bps == b->downstream_bps &&
	       a->upstream_bps == b->upstream_bps && a->error == b->error;
}

#endif /* _AIROHA_XDSL_INTERNAL_H */
