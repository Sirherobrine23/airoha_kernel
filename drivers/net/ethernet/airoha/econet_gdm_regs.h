// SPDX-License-Identifier: GPL-2.0-only
#ifndef GDM_REGS_H
#define GDM_REGS_H

#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/types.h>

#ifndef FIELD_SET
#define FIELD_SET(current, mask, val)	\
	(((current) & ~(mask)) | FIELD_PREP((mask), (val)))
#endif

/** gdm: GDM Registers */
struct gdm {
	/** gdm_vlan: VLAN tag control */
	struct gdm_vlan {
		/** gdm_vlan_tpid: If inserting a MTK Special Tag, this TPID will be used */
		u16 tpid;

		/**
		 * See accessors:
		 * is_gdm_vlan_urx()
		 * set_gdm_vlan_urx()
		 * is_gdm_vlan_ttx()
		 * set_gdm_vlan_ttx()
		 */
		u16 bitfield_0;

	} vlan;

	/** gdm_pppoe: PPPoE header control */
	struct gdm_pppoe {
		/**
		 * See accessors:
		 * is_gdm_pppoe_ttx()
		 * set_gdm_pppoe_ttx()
		 */
		u16 bitfield_0;

		/** gdm_pppoe_pppid: PPPoE Session ID to insert on transmitted packets */
		u16 pppoe_id;

	} pppoe;

	/**
	 * See accessors:
	 * get_gdm_red_cp()
	 * set_gdm_red_cp()
	 * get_gdm_red_lp()
	 * set_gdm_red_lp()
	 * get_gdm_red_hp()
	 * set_gdm_red_hp()
	 * get_gdm_red_dscp_rxm_sz()
	 * set_gdm_red_dscp_rxm_sz()
	 */
	struct red_drop_criteria { u32 word; } red_drop_criteria;

	/**
	 * gdm_chan_en: Each bit corrisponds to a channel, if one bit is cleared then
	 * this GDM will refuse to forward traffic from that channel number.
	 */
	u32 channels_enabled;

	/**
	 * gdm_crsn_prio_lo: Priority of a packet based on the CPU_REASON for packet
	 * being sent to CPU. Each CPU_REASON corrisponds to 2 bits, 00 = crit_prio, 01
	 * = low_prio, 10 = high_prio. This word covers CPU_REASONS 0..15
	 */
	u32 cpu_reason_priority_lo;

	/**
	 * gdm_crsn_prio_hi: Priority of a packet based on the CPU_REASON for packet
	 * being sent to CPU. Each CPU_REASON corrisponds to 2 bits, 00 = crit_prio, 01
	 * = low_prio, 10 = high_prio. This word covers CPU_REASONS 16..31
	 */
	u32 cpu_reason_priority_hi;

	u8 unused_0[232];

	/**
	 * See accessors:
	 * is_gdm_fwd_cfg_vip()
	 * set_gdm_fwd_cfg_vip()
	 * is_gdm_fwd_cfg_l2lu_dcsp_2cpu()
	 * set_gdm_fwd_cfg_l2lu_dcsp_2cpu()
	 * is_gdm_fwd_cfg_l2lu_ctag_2cpu()
	 * set_gdm_fwd_cfg_l2lu_ctag_2cpu()
	 * is_gdm_fwd_cfg_l2lu_stag_2cpu()
	 * set_gdm_fwd_cfg_l2lu_stag_2cpu()
	 * is_gdm_fwd_cfg_g2_underrun_retry()
	 * set_gdm_fwd_cfg_g2_underrun_retry()
	 * is_gdm_fwd_cfg_g2_drop_256b()
	 * set_gdm_fwd_cfg_g2_drop_256b()
	 * is_gdm_fwd_cfg_drop_oversize()
	 * set_gdm_fwd_cfg_drop_oversize()
	 * is_gdm_fwd_cfg_drop_runt()
	 * set_gdm_fwd_cfg_drop_runt()
	 * is_gdm_fwd_cfg_drop_crc()
	 * set_gdm_fwd_cfg_drop_crc()
	 * is_gdm_fwd_cfg_drop_ip4_csum()
	 * set_gdm_fwd_cfg_drop_ip4_csum()
	 * is_gdm_fwd_cfg_drop_tcp_csum()
	 * set_gdm_fwd_cfg_drop_tcp_csum()
	 * is_gdm_fwd_cfg_drop_ucp_csum()
	 * set_gdm_fwd_cfg_drop_ucp_csum()
	 * is_gdm_fwd_cfg_g2_favor_oam()
	 * set_gdm_fwd_cfg_g2_favor_oam()
	 * is_gdm_fwd_cfg_l2lu_brg_cpu()
	 * set_gdm_fwd_cfg_l2lu_brg_cpu()
	 * is_gdm_fwd_cfg_l2lu_unbrg_cpu()
	 * set_gdm_fwd_cfg_l2lu_unbrg_cpu()
	 * is_gdm_fwd_cfg_strip_crc()
	 * set_gdm_fwd_cfg_strip_crc()
	 * get_gdm_fwd_cfg_mymac_fport()
	 * set_gdm_fwd_cfg_mymac_fport()
	 * get_gdm_fwd_cfg_bcast_fport()
	 * set_gdm_fwd_cfg_bcast_fport()
	 * get_gdm_fwd_cfg_mcast_fport()
	 * set_gdm_fwd_cfg_mcast_fport()
	 * get_gdm_fwd_cfg_default_fport()
	 * set_gdm_fwd_cfg_default_fport()
	 */
	struct fwd_cfg { u32 word; } fwd_cfg;

	/** gdm_tx_shaper: Traffic shaper for TX */
	u32 tx_shaper;

	/**
	 * gdm_mymac_lsb: Mac address bytes [c,d,e,f] (bytes 2-5 of MAC)
	 * Register layout (32-bit word):
	 * - Bits 31-24: MAC byte c (index 2)
	 * - Bits 23-16: MAC byte d (index 3)
	 * - Bits 15-8: MAC byte e (index 4)
	 * - Bits 7-0: MAC byte f (index 5)
	 * See accessors:
	 * get_gdm_mymac_lsb_c/d/e/f()
	 * set_gdm_mymac_lsb_c/d/e/f()
	 */
	struct gdm_mymac_lsb { u32 word; } mymac_lsb;

	/**
	 * gdm_mymac_msb: Mac address bytes [a,b] (bytes 0-1 of MAC) and mask
	 * Register layout (32-bit word):
	 * - Bits 31-24: unused
	 * - Bits 23-16: lsb_mask (LSB matching mask)
	 * - Bits 15-8: MAC byte a (index 0)
	 * - Bits 7-0: MAC byte b (index 1)
	 * See accessors:
	 * get_gdm_mymac_msb_lsb_mask/a/b()
	 * set_gdm_mymac_msb_lsb_mask/a/b()
	 */
	struct gdm_mymac_msb { u32 word; } mymac_msb;

	/** gdm_stag_en: Add Special Tag on RX frames, 0 or 1 */
	u32 stag_en;

	/**
	 * gdm_len_th:
	 * Register layout (32-bit word):
	 * - Bits 31-16: OVERSIZE_LEN (packet larger than this is oversize, max 0x3f00)
	 * - Bits 15-0: RUNT_LEN (packet smaller than this is a runt)
	 * See accessors:
	 * get_gdm_len_th_oversize_len()
	 * set_gdm_len_th_oversize_len()
	 * get_gdm_len_th_runt_len()
	 * set_gdm_len_th_runt_len()
	 */
	struct gdm_len_th { u32 word; } rx_len_threshold;

	/**
	 * See accessors:
	 * get_gdm_pcp_gdm_rx()
	 * set_gdm_pcp_gdm_rx()
	 * get_gdm_pcp_gdm_tx()
	 * set_gdm_pcp_gdm_tx()
	 * get_gdm_pcp_cdm_rx()
	 * set_gdm_pcp_cdm_rx()
	 * get_gdm_pcp_cdm_tx()
	 * set_gdm_pcp_cdm_tx()
	 */
	struct pcp { u32 word; } pcp;

	/**
	 * See accessors:
	 * get_gdm_lpbk_gap()
	 * set_gdm_lpbk_gap()
	 * get_gdm_lpbk_len()
	 * set_gdm_lpbk_len()
	 * get_gdm_lpbk_chan()
	 * set_gdm_lpbk_chan()
	 * is_gdm_lpbk_gap_mode()
	 * set_gdm_lpbk_gap_mode()
	 * is_gdm_lpbk_len_mode()
	 * set_gdm_lpbk_len_mode()
	 * is_gdm_lpbk_chan_mode()
	 * set_gdm_lpbk_chan_mode()
	 * is_gdm_lpbk_enabled()
	 * set_gdm_lpbk_enabled()
	 */
	struct loopback { u32 word; } loopback;

	/**
	 * See accessors:
	 * get_gdm_channel_retire_channel()
	 * set_gdm_channel_retire_channel()
	 * is_gdm_channel_retire_done()
	 * set_gdm_channel_retire_done()
	 * is_gdm_channel_retire_release()
	 * set_gdm_channel_retire_release()
	 */
	struct channel_retire { u32 word; } channel_retire;

	/**
	 * gdm_tx_ch_en: Each bit corripsonds to one of the 32 channels, set bit to
	 * enable channel
	 */
	u32 enabled_tx_channels;

	/**
	 * gdm_rx_ch_en: Each bit 0..16 corrisonds to one of the 16 RX channels, upper
	 * 16 bits are unused
	 */
	u32 enabled_rx_channels;

	/**
	 * See accessors:
	 * is_gdm_rx7_en()
	 * set_gdm_rx7_en()
	 * get_gdm_rx7_fport()
	 * set_gdm_rx7_fport()
	 */
	u32 bitfield_0;

	/** gdm_g2_rx_shapers: Incoming traffic shapers, only present in GDM2 */
	struct gdm_g2_rx_shapers {
		/** gdm_g2_rx_shapers_mymac: Shaper for incoming traffic to my MAC */
		u32 mymac;

		/** gdm_g2_rx_shapers_bcast: Shaper for incoming broadcast traffic */
		u32 bcast;

		/** gdm_g2_rx_shapers_mcast: Shaper for incoming multicast traffic */
		u32 mcast;

		/**
		 * gdm_g2_rx_shapers_dflt: Default shaper for incoming traffic not otherwise
		 * categorized
		 */
		u32 dflt;

	} g2_rx_shapers;

	/**
	 * See accessors:
	 * is_gdm_g1_cport_cfg_add_crc()
	 * set_gdm_g1_cport_cfg_add_crc()
	 * is_gdm_g1_cport_cfg_pad()
	 * set_gdm_g1_cport_cfg_pad()
	 * is_gdm_g1_cport_cfg_port_xfc()
	 * set_gdm_g1_cport_cfg_port_xfc()
	 * is_gdm_g1_cport_cfg_queue_xfc()
	 * set_gdm_g1_cport_cfg_queue_xfc()
	 * get_gdm_g1_cport_cfg_unknown()
	 * set_gdm_g1_cport_cfg_unknown()
	 */
	struct g1_cport_cfg { u32 word; } g1_cport_cfg;

	/**
	 * gdm_g1_cport_channel_map: Called FE_CPORT_CHN_MAP, never used, observed
	 * value is 0x76543210, GDM1 only
	 */
	u32 g1_cport_channel_map;

	/** gdm_g1_cport_shaper: Traffic shaper for CPORT */
	u32 g1_cport_shaper;

	/**
	 * gdm_g1_unknown0: Unused, no symbol, observed value 0x000003c0, GDM1 only.
	 */
	u32 g1_unknown0;

	/**
	 * gdm_g1_unknown1: Unused, no symbol, observed value 0x00000000, GDM1 only.
	 */
	u32 g1_unknown1;

	u8 unused_1[28];

	/**
	 * gdm_tx_chan_active: Each of the 32 bits represents one of the 32 channels, 1
	 * means channel is active used with channel_retire to confirm channel is
	 * shutdown. Also GDMA1_TX_CHN_VLD
	 */
	u32 tx_chan_active;

	/**
	 * gdm_rx_chan_active: Unused in practice except on EN7580, each bit represents
	 * an RX channel, 1 means RX channel is active, used with channel_retire.
	 * Unsure if 16 or 32 RX channels. Also GDMA1_RX_CHN_VLD
	 */
	u32 rx_chan_active;

	u8 unused_2[8];

	/** gdm_cdm_counters: Various packet counters for CDM1 / CDM2 */
	struct gdm_cdm_counters {
		/**
		 * gdm_cdm_counters_tx: Unused in practice, seems to be frames successfully
		 * sent by CDM, also CDMA1_TX_OK_CNT
		 */
		u32 tx;

		u8 unused_0[12];

		/**
		 * gdm_cdm_counters_rxcpu: Unused in practice, seems to be frames successfully
		 * received to CPU by CDM, also CDMA1_RXCPU_OK_CNT
		 */
		u32 rxcpu;

		/**
		 * gdm_cdm_counters_rxhwf: Unused in practice, seems to be frames successfully
		 * received to hardware forwarding chain by CDM, also CDMA1_RXHWF_OK_CNT
		 */
		u32 rxhwf;

		/**
		 * gdm_cdm_counters_ka: Unused in practice, seems to be frames sent to CPU for
		 * NAT keepalive purposes, also CDMA1_RXCPU_KA_CNT
		 */
		u32 ka;

		u32 unused_1;

		/**
		 * gdm_cdm_counters_rxcpu_drop: Unused in practice, seems to be frames dropped
		 * for congestion on the RX->CPU chain, also CDMA1_RXCPU_DROP_CNT
		 */
		u32 rxcpu_drop;

		/**
		 * gdm_cdm_counters_rxhwf_drop: Unused in practice, seems to be frames dropped
		 * for congestion on the RX->Hardware Forwarding chain, also
		 * CDMA1_RXHWF_DROP_CNT
		 */
		u32 rxhwf_drop;

		/**
		 * gdm_cdm_counters_unknown0: Unused, no symbol, observed value matches rxcpu
		 */
		u32 unknown0;

		/**
		 * gdm_cdm_counters_unknown1: Unused, no symbol, observed value 0x00000000
		 */
		u32 unknown1;

		/**
		 * gdm_cdm_counters_unknown2: Unused, no symbol, observed value 0x00000000
		 */
		u32 unknown2;

		/**
		 * gdm_cdm_counters_unknown3: Unused, no symbol, observed value 0x00000000
		 */
		u32 unknown3;

		/**
		 * gdm_cdm_counters_unknown4: Unused, no symbol, observed value 0x00000000
		 */
		u32 unknown4;

		/**
		 * gdm_cdm_counters_unknown5: Unused, no symbol, observed value 0x00000000
		 */
		u32 unknown5;

	} cdm_counters;

	u8 unused_3[48];

	/**
	 * See accessors:
	 * set_gdm_cl_cnt_rx()
	 * set_gdm_cl_cnt_tx()
	 */
	struct clear_counters { u32 word; } clear_counters;

	u8 unused_4[12];

	/** gdm_counters: Packet and byte counters for GDM port */
	struct gdm_counters {
		/** gdm_counters_tx:  */
		struct gdm_counters_tx {
			/**
			 * gdm_counters_tx_get: Unused in practice, believed to be number of packets
			 * enregistered for TX, also GDMA1_TX_GET_CNT / GDMA2_TX_GETCNT
			 */
			u32 get;

			/**
			 * gdm_counters_tx_pkts: Packets successfully transmitted, also
			 * GDMA1_TX_OK_CNT / GDMA2_TX_OKCNT
			 */
			u32 tx_pkts;

			/**
			 * gdm_counters_tx_drops: Packets dropped on transmission chain, also
			 * GDMA1_TX_DROP_CNT / GDMA2_TX_DROPCNT
			 */
			u32 drops;

			/**
			 * gdm_counters_tx_bytes: Bytes successfully transmitted, also
			 * GDMA1_TX_OK_BYTE_CNT / GDMA2_TX_OKBYTE_CNT
			 */
			u32 bytes;

			/** gdm_counters_tx_g2: GDM2 only extended TX stats */
			struct gdm_counters_tx_g2 {
				/**
				 * gdm_counters_tx_g2_epkts: Number of eth frames transmitted, should match
				 * .pkts, also GDMA2_TX_ETHCNT
				 */
				u32 tx_epkts;

				/**
				 * gdm_counters_tx_g2_ebytes: Bytes of eth bytes transmitted, also
				 * GDMA2_TX_ETHLENCNT
				 */
				u32 tx_ebytes;

				/**
				 * gdm_counters_tx_g2_edrops: Number of eth frames dropped, see
				 * GDMA2_TX_ETHDROPCNT
				 */
				u32 edrops;

				/**
				 * gdm_counters_tx_g2_bcast: Number of broadcast frames transmitted, see
				 * GDMA2_TX_ETHBCDCNT
				 */
				u32 tx_bcast;

				/**
				 * gdm_counters_tx_g2_mcast: Number of multicast frames transmitted, see
				 * GDMA2_TX_ETHMULTICASTCNT
				 */
				u32 tx_mcast;

				/**
				 * gdm_counters_tx_g2_f_less_64: Counter of TX frames of length less than
				 * 64, see GDMA2_TX_ETH_LESS64_CNT
				 */
				u32 f_less_64;

				/**
				 * gdm_counters_tx_g2_f_more_1518: Counter of TX frames of length more than
				 * 1518, see GDMA2_TX_ETH_MORE1518_CNT
				 */
				u32 f_more_1518;

				/**
				 * gdm_counters_tx_g2_f_64: Counter of TX frames of length 64, see
				 * GDMA2_TX_ETH_64_CNT
				 */
				u32 f_64;

				/**
				 * gdm_counters_tx_g2_f_65_127: Counter of TX frames of length 65-127, see
				 * GDMA2_TX_ETH_65_TO_127_CNT
				 */
				u32 f_65_127;

				/**
				 * gdm_counters_tx_g2_f_128_255: Counter of TX frames of length 128-255, see
				 * GDMA2_TX_ETH_128_TO_255_CNT
				 */
				u32 f_128_255;

				/**
				 * gdm_counters_tx_g2_f_256_511: Counter of TX frames of length 256-511, see
				 * GDMA2_TX_ETH_256_TO_511_CNT
				 */
				u32 f_256_511;

				/**
				 * gdm_counters_tx_g2_f_512_1023: Counter of TX frames of length 512-1023,
				 * see GDMA2_TX_ETH_512_TO_1023_CNT
				 */
				u32 f_512_1023;

				/**
				 * gdm_counters_tx_g2_f_1024_1518: Counter of TX frames of length 1024-1518,
				 * see GDMA2_TX_ETH_1024_TO_1518_CNT
				 */
				u32 f_1024_1518;

			} g2;

		} tx;

		u32 unused_0;

		/** gdm_counters_rx:  */
		struct gdm_counters_rx {
			/**
			 * gdm_counters_rx_pkts: Number of packets received, see GDMA1_RX_OK_CNT /
			 * GDMA2_RX_OKCNT
			 */
			u32 pkts;

			/**
			 * gdm_counters_rx_drops_fc: Packets dropped due to flow control, thought to
			 * be when port sends PAUSE signal. see GDMA1_RX_FC_DROP_CNT
			 */
			u32 drops_fc;

			/**
			 * gdm_counters_rx_drops_rc: Packets dropped due to rate control, thought to
			 * be shapers. See GDMA1_RX_RC_DROP_CNT / GDMA2_RX_RCDROPCNT
			 */
			u32 drops_rc;

			/**
			 * gdm_counters_rx_drops_overflow: Packets dropped because we ran out of
			 * queue resources, vendor code logs when this counter increases so it should
			 * not happen. GDMA2_RX_OVDROPCNT / GDMA1_RX_OVER_DROP_CNT
			 */
			u32 drops_overflow;

			/**
			 * gdm_counters_rx_drops_err: Packets dropped because of any error (crc,
			 * IP/TCP/UDP checksum, oversize, runt, etc)
			 */
			u32 drops_err;

			/**
			 * gdm_counters_rx_bytes: Received bytes, see GDMA1_RX_BYTECNT /
			 * GDMA2_RX_OKBYTECNT
			 */
			u32 bytes;

			/** gdm_counters_rx_g2: GDM2 only extended RX stats */
			struct gdm_counters_rx_g2 {
				/**
				 * gdm_counters_rx_g2_epkts: Number of eth frames received, should match
				 * .pkts, also GDMA2_RX_ETHERPCNT
				 */
				u32 epkts;

				/**
				 * gdm_counters_rx_g2_ebytes: Bytes of eth bytes received, also
				 * GDMA2_RX_ETHERPLEN
				 */
				u32 ebytes;

				/**
				 * gdm_counters_rx_g2_edrops: Number of eth frames dropped, see
				 * GDMA2_RX_ETHDROPCNT
				 */
				u32 edrops;

				/**
				 * gdm_counters_rx_g2_bcast: Number of broadcast frames received, see
				 * GDMA2_RX_ETHBCCNT
				 */
				u32 bcast;

				/**
				 * gdm_counters_rx_g2_mcast: Number of multicast frames received, see
				 * GDMA2_RX_ETHMCCNT
				 */
				u32 mcast;

				/**
				 * gdm_counters_rx_g2_ecrc: Number of frames with CRC errors, see
				 * GDMA2_RX_ETHCRCCNT
				 */
				u32 ecrc;

				/**
				 * gdm_counters_rx_g2_efrag: Number of frames with CRC error and which are
				 * less than 64 bytes (probably fragments from synchronization issue) see
				 * GDMA2_RX_ETHFRACCNT
				 */
				u32 efrag;

				/**
				 * gdm_counters_rx_g2_ejabber: Number of frames longer than 1518 with bad
				 * CRC, sign of potentially jabbering hardware, see GDMA2_RX_ETHJABCNT
				 */
				u32 ejabber;

				/**
				 * gdm_counters_rx_g2_f_less_64: Counter of TX frames of length less than
				 * 64, see GDMA2_RX_ETHRUNTCNT
				 */
				u32 f_less_64;

				/**
				 * gdm_counters_rx_g2_f_more_1518: Counter of TX frames of length more than
				 * 1518, see GDMA2_RX_ETHLONGCNT
				 */
				u32 f_more_1518;

				/**
				 * gdm_counters_rx_g2_f_64: Counter of TX frames of length 64, see
				 * GDMA2_RX_ETH_64_CNT
				 */
				u32 f_64;

				/**
				 * gdm_counters_rx_g2_f_65_127: Counter of TX frames of length 65-127, see
				 * GDMA2_RX_ETH_65_TO_127_CNT
				 */
				u32 f_65_127;

				/**
				 * gdm_counters_rx_g2_f_128_255: Counter of TX frames of length 128-255, see
				 * GDMA2_RX_ETH_128_TO_255_CNT
				 */
				u32 f_128_255;

				/**
				 * gdm_counters_rx_g2_f_256_511: Counter of TX frames of length 256-511, see
				 * GDMA2_RX_ETH_256_TO_511_CNT
				 */
				u32 f_256_511;

				/**
				 * gdm_counters_rx_g2_f_512_1023: Counter of TX frames of length 512-1023,
				 * see GDMA2_RX_ETH_512_TO_1023_CNT
				 */
				u32 f_512_1023;

				/**
				 * gdm_counters_rx_g2_f_1024_1518: Counter of TX frames of length 1024-1518,
				 * see GDMA2_RX_ETH_1024_TO_1518_CNT
				 */
				u32 f_1024_1518;

				/** gdm_counters_rx_g2_unknown0: Observed 0x00000000, possible counter */
				u32 tx_unknown0;

				/** gdm_counters_rx_g2_unknown1: Observed 0x00000000, possible counter */
				u32 unknown1;

			} g2;

		} rx;

	} counters;

};

/**
 * Bitfield accessors for: gdm_vlan bitfield_0
 */

#define GDM_VLAN_URX					BIT(1)
#define GDM_VLAN_TTX					BIT(0)


/** Untag incoming tagged packets (and put the tag in the DESC) */
static inline bool is_gdm_vlan_urx(struct gdm_vlan *x)
{
	return FIELD_GET(GDM_VLAN_URX, x->bitfield_0);
}
static inline void set_gdm_vlan_urx(struct gdm_vlan *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, GDM_VLAN_URX, v);
}

/** Insert MTK Special Tag when sending packets */
static inline bool is_gdm_vlan_ttx(struct gdm_vlan *x)
{
	return FIELD_GET(GDM_VLAN_TTX, x->bitfield_0);
}
static inline void set_gdm_vlan_ttx(struct gdm_vlan *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, GDM_VLAN_TTX, v);
}

/**
 * Bitfield accessors for: gdm_pppoe bitfield_0
 */

#define GDM_PPPOE_TTX					BIT(0)


/** Insert PPPoE header on transmitted packets */
static inline bool is_gdm_pppoe_ttx(struct gdm_pppoe *x)
{
	return FIELD_GET(GDM_PPPOE_TTX, x->bitfield_0);
}
static inline void set_gdm_pppoe_ttx(struct gdm_pppoe *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, GDM_PPPOE_TTX, v);
}

/**
 * Bitfield accessors for: struct red_drop_criteria
 */

enum gdm_red_cp {
	GDM_RED_CP_RL_OR_FULL				= 0,
	GDM_RED_CP_RL_OR_TH				= 1,
	GDM_RED_CP_RL_AND_FULL				= 2,
	GDM_RED_CP_RL_AND_TH				= 3,
};
enum gdm_red_dscp_rxm_sz {
	GDM_RED_DSCP_RXM_SZ_4B				= 0,
	GDM_RED_DSCP_RXM_SZ_8B				= 1,
	GDM_RED_DSCP_RXM_SZ_16B				= 2,
	GDM_RED_DSCP_RXM_SZ_32B				= 3,
};

#define GDM_RED_CP_MASK					GENMASK(31, 30)
#define GDM_RED_LP_MASK					GENMASK(29, 28)
#define GDM_RED_HP_MASK					GENMASK(27, 26)
#define GDM_RED_DSCP_RXM_SZ_MASK			GENMASK(25, 24)


/**
 * Trigger RED packet dropping on 1. Rate-limit-violation OR buffer is full 2.
 * Rate-limit-violation OR buffer has surpassed threshold 3.
 * Rate-limit-violation WHEN buffer is also full 4. Rate-limit-violation WHEN
 * buffer has surpassed threshold For critical priority packets (VIP)
 */
static inline enum gdm_red_cp get_gdm_red_cp(struct red_drop_criteria *x)
{
	return FIELD_GET(GDM_RED_CP_MASK, x->word);
}
static inline void set_gdm_red_cp(struct red_drop_criteria *x, enum gdm_red_cp v)
{
	x->word = FIELD_SET(x->word, GDM_RED_CP_MASK, v);
}

/** Same as crit_prio, for packets classed as low priority by CPU Reason */
static inline u8 get_gdm_red_lp(struct red_drop_criteria *x)
{
	return FIELD_GET(GDM_RED_LP_MASK, x->word);
}
static inline void set_gdm_red_lp(struct red_drop_criteria *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_RED_LP_MASK, v);
}

/** Same as crit_prio, for packets classed as low priority by CPU Reason */
static inline u8 get_gdm_red_hp(struct red_drop_criteria *x)
{
	return FIELD_GET(GDM_RED_HP_MASK, x->word);
}
static inline void set_gdm_red_hp(struct red_drop_criteria *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_RED_HP_MASK, v);
}

/** Size of the msg field within the packet descriptor */
static inline enum gdm_red_dscp_rxm_sz get_gdm_red_dscp_rxm_sz(struct red_drop_criteria *x)
{
	return FIELD_GET(GDM_RED_DSCP_RXM_SZ_MASK, x->word);
}
static inline void set_gdm_red_dscp_rxm_sz(struct red_drop_criteria *x, enum gdm_red_dscp_rxm_sz v)
{
	x->word = FIELD_SET(x->word, GDM_RED_DSCP_RXM_SZ_MASK, v);
}

/**
 * Bitfield accessors for: struct fwd_cfg
 * Configuration for GDM
 */

#define GDM_FWD_CFG_VIP					BIT(31)
#define GDM_FWD_CFG_L2LU_DCSP_2CPU			BIT(30)
#define GDM_FWD_CFG_L2LU_CTAG_2CPU			BIT(29)
#define GDM_FWD_CFG_L2LU_STAG_2CPU			BIT(28)
#define GDM_FWD_CFG_G2_UNDERRUN_RETRY			BIT(27)
#define GDM_FWD_CFG_G2_DROP_256B			BIT(26)
#define GDM_FWD_CFG_DROP_OVERSIZE			BIT(25)
#define GDM_FWD_CFG_DROP_RUNT				BIT(24)
#define GDM_FWD_CFG_DROP_CRC				BIT(23)
#define GDM_FWD_CFG_DROP_IP4_CSUM			BIT(22)
#define GDM_FWD_CFG_DROP_TCP_CSUM			BIT(21)
#define GDM_FWD_CFG_DROP_UCP_CSUM			BIT(20)
#define GDM_FWD_CFG_G2_FAVOR_OAM			BIT(19)
#define GDM_FWD_CFG_L2LU_BRG_CPU			BIT(18)
#define GDM_FWD_CFG_L2LU_UNBRG_CPU			BIT(17)
#define GDM_FWD_CFG_STRIP_CRC				BIT(16)
#define GDM_FWD_CFG_MYMAC_FPORT_MASK			GENMASK(15, 12)
#define GDM_FWD_CFG_BCAST_FPORT_MASK			GENMASK(11, 8)
#define GDM_FWD_CFG_MCAST_FPORT_MASK			GENMASK(7, 4)
#define GDM_FWD_CFG_DEFAULT_FPORT_MASK			GENMASK(3, 0)


/**
 * GDM2 only, configure whether VIP packets are included in rate control
 * calculation. They are not actually dropped in any case.
 */
static inline bool is_gdm_fwd_cfg_vip(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_VIP, x->word);
}
static inline void set_gdm_fwd_cfg_vip(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_VIP, v);
}

/** On L2LU DSCP lookup miss, send packet to CPU */
static inline bool is_gdm_fwd_cfg_l2lu_dcsp_2cpu(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_L2LU_DCSP_2CPU, x->word);
}
static inline void set_gdm_fwd_cfg_l2lu_dcsp_2cpu(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_L2LU_DCSP_2CPU, v);
}

/** On L2LU CTAG lookup miss, send package to CPU */
static inline bool is_gdm_fwd_cfg_l2lu_ctag_2cpu(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_L2LU_CTAG_2CPU, x->word);
}
static inline void set_gdm_fwd_cfg_l2lu_ctag_2cpu(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_L2LU_CTAG_2CPU, v);
}

/** On L2LU STAG lookup miss, send packet to CPU */
static inline bool is_gdm_fwd_cfg_l2lu_stag_2cpu(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_L2LU_STAG_2CPU, x->word);
}
static inline void set_gdm_fwd_cfg_l2lu_stag_2cpu(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_L2LU_STAG_2CPU, v);
}

/** Unknown/Unused, see GDM2_UNDERRUN_RETRY / GDMA1_FWD_CFG_RETRY_OFFSET */
static inline bool is_gdm_fwd_cfg_g2_underrun_retry(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_G2_UNDERRUN_RETRY, x->word);
}
static inline void set_gdm_fwd_cfg_g2_underrun_retry(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_G2_UNDERRUN_RETRY, v);
}

/** Unknown/unused, referred to as GDM2_DROP_256B */
static inline bool is_gdm_fwd_cfg_g2_drop_256b(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_G2_DROP_256B, x->word);
}
static inline void set_gdm_fwd_cfg_g2_drop_256b(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_G2_DROP_256B, v);
}

/**
 * If frame length (with CRC) is greater than rx_len_threshold.oversize_len,
 * drop. Referred to as GDM2_DROP_LONG.
 */
static inline bool is_gdm_fwd_cfg_drop_oversize(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_OVERSIZE, x->word);
}
static inline void set_gdm_fwd_cfg_drop_oversize(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_OVERSIZE, v);
}

/**
 * If frame length (with CRC) is less than rx_len_threshold.runt_len, drop.
 * Referred to as GDM2_DROP_RUNT.
 */
static inline bool is_gdm_fwd_cfg_drop_runt(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_RUNT, x->word);
}
static inline void set_gdm_fwd_cfg_drop_runt(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_RUNT, v);
}

/** If eth CRC invalid, drop. See GDM2_DROP_CRC_ERR */
static inline bool is_gdm_fwd_cfg_drop_crc(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_CRC, x->word);
}
static inline void set_gdm_fwd_cfg_drop_crc(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_CRC, v);
}

/** Drop packet on IPv4 checksum error */
static inline bool is_gdm_fwd_cfg_drop_ip4_csum(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_IP4_CSUM, x->word);
}
static inline void set_gdm_fwd_cfg_drop_ip4_csum(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_IP4_CSUM, v);
}

/** Drop packet on TCP checksum error */
static inline bool is_gdm_fwd_cfg_drop_tcp_csum(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_TCP_CSUM, x->word);
}
static inline void set_gdm_fwd_cfg_drop_tcp_csum(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_TCP_CSUM, v);
}

/** Drop packet on UDP checksum error */
static inline bool is_gdm_fwd_cfg_drop_ucp_csum(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DROP_UCP_CSUM, x->word);
}
static inline void set_gdm_fwd_cfg_drop_ucp_csum(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DROP_UCP_CSUM, v);
}

/**
 * Favor OAM frames during transmission, this probably means raise their
 * priority. GDM2 only, EN761221 (EN7521/EN7526) only, see
 * GDMA2_TX_FAVOR_OAM_OFFSET
 */
static inline bool is_gdm_fwd_cfg_g2_favor_oam(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_G2_FAVOR_OAM, x->word);
}
static inline void set_gdm_fwd_cfg_g2_favor_oam(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_G2_FAVOR_OAM, v);
}

/**
 * Packets which are L2LU misses and which match an L2-Bridge to be forwarded to
 * the CPU. Referred to as GDMA1_FWD_CFG_BRG_OFFSET
 */
static inline bool is_gdm_fwd_cfg_l2lu_brg_cpu(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_L2LU_BRG_CPU, x->word);
}
static inline void set_gdm_fwd_cfg_l2lu_brg_cpu(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_L2LU_BRG_CPU, v);
}

/**
 * Packets which are L2LU misses and which are NOT part of a L2-Bridge to be
 * forwarded to the CPU. Referred to as GDMA1_FWD_CFG_RUT_OFFSET
 */
static inline bool is_gdm_fwd_cfg_l2lu_unbrg_cpu(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_L2LU_UNBRG_CPU, x->word);
}
static inline void set_gdm_fwd_cfg_l2lu_unbrg_cpu(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_L2LU_UNBRG_CPU, v);
}

/**
 * Strip eth crc trailer on RX, this is used with g1_cport_cfg.add_crc when xPON
 * WAN and LAN are bridged.
 */
static inline bool is_gdm_fwd_cfg_strip_crc(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_STRIP_CRC, x->word);
}
static inline void set_gdm_fwd_cfg_strip_crc(struct fwd_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_STRIP_CRC, v);
}

/** GDM_P_* port for packets with our MAC address */
static inline u8 get_gdm_fwd_cfg_mymac_fport(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_MYMAC_FPORT_MASK, x->word);
}
static inline void set_gdm_fwd_cfg_mymac_fport(struct fwd_cfg *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_MYMAC_FPORT_MASK, v);
}

/** GDM_P_* port for broadcast packets */
static inline u8 get_gdm_fwd_cfg_bcast_fport(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_BCAST_FPORT_MASK, x->word);
}
static inline void set_gdm_fwd_cfg_bcast_fport(struct fwd_cfg *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_BCAST_FPORT_MASK, v);
}

/** GDM_P_* port for multicast packets */
static inline u8 get_gdm_fwd_cfg_mcast_fport(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_MCAST_FPORT_MASK, x->word);
}
static inline void set_gdm_fwd_cfg_mcast_fport(struct fwd_cfg *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_MCAST_FPORT_MASK, v);
}

/** GDM_P_* port for other packets */
static inline u8 get_gdm_fwd_cfg_default_fport(struct fwd_cfg *x)
{
	return FIELD_GET(GDM_FWD_CFG_DEFAULT_FPORT_MASK, x->word);
}
static inline void set_gdm_fwd_cfg_default_fport(struct fwd_cfg *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_FWD_CFG_DEFAULT_FPORT_MASK, v);
}

/**
 * Bitfield accessors for: struct pcp
 * These codings are defined in IEEE 802.1ad and define how priority and
 * drop-eligiblity. should be represented in the 3 bit VLAN PCP header. 8p0d
 * specifies 8 priorities of which none are drop-eligible. 7p1d specifies 7
 * priorities + p0d which is the same as p0, but marked drop-eligible. 6p2d
 * specieies 6 priorities + p0d and p1d, same as p0 and p1, but marked
 * drop-eligible. 5p3d specifies 5 priorities + p0d, p1d, and p2d, same as p0,
 * p1, p2, but marked drop-eligible.
 */

enum gdm_pcp_gdm_rx {
	GDM_PCP_GDM_RX_5P3D				= 8,
	GDM_PCP_GDM_RX_6P2D				= 4,
	GDM_PCP_GDM_RX_7P1D				= 2,
	GDM_PCP_GDM_RX_8P0D				= 1,
};

#define GDM_PCP_GDM_RX_MASK				GENMASK(15, 12)
#define GDM_PCP_GDM_TX_MASK				GENMASK(11, 8)
#define GDM_PCP_CDM_RX_MASK				GENMASK(7, 4)
#define GDM_PCP_CDM_TX_MASK				GENMASK(3, 0)

static inline enum gdm_pcp_gdm_rx get_gdm_pcp_gdm_rx(struct pcp *x)
{
	return FIELD_GET(GDM_PCP_GDM_RX_MASK, x->word);
}
static inline void set_gdm_pcp_gdm_rx(struct pcp *x, enum gdm_pcp_gdm_rx v)
{
	x->word = FIELD_SET(x->word, GDM_PCP_GDM_RX_MASK, v);
}
static inline u8 get_gdm_pcp_gdm_tx(struct pcp *x)
{
	return FIELD_GET(GDM_PCP_GDM_TX_MASK, x->word);
}
static inline void set_gdm_pcp_gdm_tx(struct pcp *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_PCP_GDM_TX_MASK, v);
}
static inline u8 get_gdm_pcp_cdm_rx(struct pcp *x)
{
	return FIELD_GET(GDM_PCP_CDM_RX_MASK, x->word);
}
static inline void set_gdm_pcp_cdm_rx(struct pcp *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_PCP_CDM_RX_MASK, v);
}
static inline u8 get_gdm_pcp_cdm_tx(struct pcp *x)
{
	return FIELD_GET(GDM_PCP_CDM_TX_MASK, x->word);
}
static inline void set_gdm_pcp_cdm_tx(struct pcp *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_PCP_CDM_TX_MASK, v);
}

/**
 * Bitfield accessors for: struct loopback
 * This is a poorly understood / rarely used register which seems to control GDM
 * level loopback. It has gap, len, and channel. Channel seems to corrispond to
 * the QDMA channel but the meaning of gap and len are unknown. Names include
 * GDMA1_LPBK_CFG, GDMA2_LPBP_CFG, and REG_GDM_LPBK_CFG (Airoha).
 */

#define GDM_LPBK_GAP_MASK				GENMASK(31, 24)
#define GDM_LPBK_LEN_MASK				GENMASK(23, 10)
#define GDM_LPBK_CHAN_MASK				GENMASK(8, 4)
#define GDM_LPBK_GAP_MODE				BIT(3)
#define GDM_LPBK_LEN_MODE				BIT(2)
#define GDM_LPBK_CHAN_MODE				BIT(1)
#define GDM_LPBK_ENABLED				BIT(0)

static inline u8 get_gdm_lpbk_gap(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_GAP_MASK, x->word);
}
static inline void set_gdm_lpbk_gap(struct loopback *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_GAP_MASK, v);
}
static inline u16 get_gdm_lpbk_len(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_LEN_MASK, x->word);
}
static inline void set_gdm_lpbk_len(struct loopback *x, u16 v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_LEN_MASK, v);
}
static inline u8 get_gdm_lpbk_chan(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_CHAN_MASK, x->word);
}
static inline void set_gdm_lpbk_chan(struct loopback *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_CHAN_MASK, v);
}
static inline bool is_gdm_lpbk_gap_mode(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_GAP_MODE, x->word);
}
static inline void set_gdm_lpbk_gap_mode(struct loopback *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_GAP_MODE, v);
}
static inline bool is_gdm_lpbk_len_mode(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_LEN_MODE, x->word);
}
static inline void set_gdm_lpbk_len_mode(struct loopback *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_LEN_MODE, v);
}
static inline bool is_gdm_lpbk_chan_mode(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_CHAN_MODE, x->word);
}
static inline void set_gdm_lpbk_chan_mode(struct loopback *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_CHAN_MODE, v);
}
static inline bool is_gdm_lpbk_enabled(struct loopback *x)
{
	return FIELD_GET(GDM_LPBK_ENABLED, x->word);
}
static inline void set_gdm_lpbk_enabled(struct loopback *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_LPBK_ENABLED, v);
}

/**
 * Bitfield accessors for: struct channel_retire
 * Shut down a channel and release its resources
 */

#define GDM_CHANNEL_RETIRE_CHANNEL_MASK			GENMASK(8, 4)
#define GDM_CHANNEL_RETIRE_DONE				BIT(1)
#define GDM_CHANNEL_RETIRE_RELEASE			BIT(0)


/** Number of the channel to release */
static inline u8 get_gdm_channel_retire_channel(struct channel_retire *x)
{
	return FIELD_GET(GDM_CHANNEL_RETIRE_CHANNEL_MASK, x->word);
}
static inline void set_gdm_channel_retire_channel(struct channel_retire *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_CHANNEL_RETIRE_CHANNEL_MASK, v);
}

/** Read 1 when channel release has completed */
static inline bool is_gdm_channel_retire_done(struct channel_retire *x)
{
	return FIELD_GET(GDM_CHANNEL_RETIRE_DONE, x->word);
}
static inline void set_gdm_channel_retire_done(struct channel_retire *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_CHANNEL_RETIRE_DONE, v);
}

/** Write 1 to release the channel */
static inline bool is_gdm_channel_retire_release(struct channel_retire *x)
{
	return FIELD_GET(GDM_CHANNEL_RETIRE_RELEASE, x->word);
}
static inline void set_gdm_channel_retire_release(struct channel_retire *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_CHANNEL_RETIRE_RELEASE, v);
}

/**
 * Bitfield accessors for: gdm bitfield_0
 * Apparently the lower 8 channels RX channels can be wired to forward to any
 * fport on receipt. This is not really ever used and the default value is
 * disabled + all route to PPE.
 */

#define GDM_RX7_EN					BIT(31)
#define GDM_RX7_FPORT_MASK				GENMASK(30, 28)

static inline bool is_gdm_rx7_en(struct gdm *x)
{
	return FIELD_GET(GDM_RX7_EN, x->bitfield_0);
}
static inline void set_gdm_rx7_en(struct gdm *x, bool v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, GDM_RX7_EN, v);
}
static inline u8 get_gdm_rx7_fport(struct gdm *x)
{
	return FIELD_GET(GDM_RX7_FPORT_MASK, x->bitfield_0);
}
static inline void set_gdm_rx7_fport(struct gdm *x, u8 v)
{
	x->bitfield_0 = FIELD_SET(x->bitfield_0, GDM_RX7_FPORT_MASK, v);
}

/**
 * Bitfield accessors for: struct g1_cport_cfg
 * Not well known what the CPORT config does, a few bits are understood. Only
 * used in GDM1, but might still be present in GDM2.
 */

#define GDM_G1_CPORT_CFG_ADD_CRC			BIT(30)
#define GDM_G1_CPORT_CFG_PAD				BIT(26)
#define GDM_G1_CPORT_CFG_PORT_XFC			BIT(25)
#define GDM_G1_CPORT_CFG_QUEUE_XFC			BIT(24)
#define GDM_G1_CPORT_CFG_UNKNOWN_MASK			GENMASK(23, 0)


/**
 * Add the ethernet trailing CRC to incoming frames (from the switch) this is
 * used with fwd_cfg.strip_crc when xPON WAN and LAN are bridged because xPON
 * PHY always sends us a CRC.
 */
static inline bool is_gdm_g1_cport_cfg_add_crc(struct g1_cport_cfg *x)
{
	return FIELD_GET(GDM_G1_CPORT_CFG_ADD_CRC, x->word);
}
static inline void set_gdm_g1_cport_cfg_add_crc(struct g1_cport_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_G1_CPORT_CFG_ADD_CRC, v);
}

/**
 * Enable padding (fe_api_set_padding, FE_CPORT_PAD), meaning is unknown but
 * this is always set at startup.
 */
static inline bool is_gdm_g1_cport_cfg_pad(struct g1_cport_cfg *x)
{
	return FIELD_GET(GDM_G1_CPORT_CFG_PAD, x->word);
}
static inline void set_gdm_g1_cport_cfg_pad(struct g1_cport_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_G1_CPORT_CFG_PAD, v);
}

/**
 * FE_CPORT_PORT_XFC_MASK, always set on startup. Meaning is thought to be frame
 * engine will pause in case of congestion on port (i.e. switch)
 */
static inline bool is_gdm_g1_cport_cfg_port_xfc(struct g1_cport_cfg *x)
{
	return FIELD_GET(GDM_G1_CPORT_CFG_PORT_XFC, x->word);
}
static inline void set_gdm_g1_cport_cfg_port_xfc(struct g1_cport_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_G1_CPORT_CFG_PORT_XFC, v);
}

/**
 * FE_CPORT_QUEUE_XFC_MASK, always cleared on startup. Meaning is throught to be
 * individual queue will pause in case of congestion on port (i.e. switch)
 */
static inline bool is_gdm_g1_cport_cfg_queue_xfc(struct g1_cport_cfg *x)
{
	return FIELD_GET(GDM_G1_CPORT_CFG_QUEUE_XFC, x->word);
}
static inline void set_gdm_g1_cport_cfg_queue_xfc(struct g1_cport_cfg *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_G1_CPORT_CFG_QUEUE_XFC, v);
}

/** Observed value 0x000a02, never updated, unknown meaning */
static inline u32 get_gdm_g1_cport_cfg_unknown(struct g1_cport_cfg *x)
{
	return FIELD_GET(GDM_G1_CPORT_CFG_UNKNOWN_MASK, x->word);
}
static inline void set_gdm_g1_cport_cfg_unknown(struct g1_cport_cfg *x, u32 v)
{
	x->word = FIELD_SET(x->word, GDM_G1_CPORT_CFG_UNKNOWN_MASK, v);
}

/**
 * Bitfield accessors for: struct clear_counters
 */

#define GDM_CL_CNT_RX					BIT(1)
#define GDM_CL_CNT_TX					BIT(0)

static inline void set_gdm_cl_cnt_rx(struct clear_counters *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_CL_CNT_RX, v);
}
static inline void set_gdm_cl_cnt_tx(struct clear_counters *x, bool v)
{
	x->word = FIELD_SET(x->word, GDM_CL_CNT_TX, v);
}

/**
 * Bitfield accessors for: gdm_mymac_lsb
 * MAC address bytes c,d,e,f (indices 2-5)
 */
#define GDM_MYMAC_LSB_C_MASK				GENMASK(31, 24)
#define GDM_MYMAC_LSB_D_MASK				GENMASK(23, 16)
#define GDM_MYMAC_LSB_E_MASK				GENMASK(15, 8)
#define GDM_MYMAC_LSB_F_MASK				GENMASK(7, 0)

static inline u8 get_gdm_mymac_lsb_c(struct gdm_mymac_lsb *x)
{
	return FIELD_GET(GDM_MYMAC_LSB_C_MASK, x->word);
}
static inline void set_gdm_mymac_lsb_c(struct gdm_mymac_lsb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_LSB_C_MASK, v);
}
static inline u8 get_gdm_mymac_lsb_d(struct gdm_mymac_lsb *x)
{
	return FIELD_GET(GDM_MYMAC_LSB_D_MASK, x->word);
}
static inline void set_gdm_mymac_lsb_d(struct gdm_mymac_lsb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_LSB_D_MASK, v);
}
static inline u8 get_gdm_mymac_lsb_e(struct gdm_mymac_lsb *x)
{
	return FIELD_GET(GDM_MYMAC_LSB_E_MASK, x->word);
}
static inline void set_gdm_mymac_lsb_e(struct gdm_mymac_lsb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_LSB_E_MASK, v);
}
static inline u8 get_gdm_mymac_lsb_f(struct gdm_mymac_lsb *x)
{
	return FIELD_GET(GDM_MYMAC_LSB_F_MASK, x->word);
}
static inline void set_gdm_mymac_lsb_f(struct gdm_mymac_lsb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_LSB_F_MASK, v);
}

/**
 * Bitfield accessors for: gdm_mymac_msb
 * MAC address bytes a,b (indices 0-1) and LSB matching mask
 */
#define GDM_MYMAC_MSB_LSB_MASK_MASK			GENMASK(23, 16)
#define GDM_MYMAC_MSB_A_MASK				GENMASK(15, 8)
#define GDM_MYMAC_MSB_B_MASK				GENMASK(7, 0)

static inline u8 get_gdm_mymac_msb_lsb_mask(struct gdm_mymac_msb *x)
{
	return FIELD_GET(GDM_MYMAC_MSB_LSB_MASK_MASK, x->word);
}
static inline void set_gdm_mymac_msb_lsb_mask(struct gdm_mymac_msb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_MSB_LSB_MASK_MASK, v);
}
static inline u8 get_gdm_mymac_msb_a(struct gdm_mymac_msb *x)
{
	return FIELD_GET(GDM_MYMAC_MSB_A_MASK, x->word);
}
static inline void set_gdm_mymac_msb_a(struct gdm_mymac_msb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_MSB_A_MASK, v);
}
static inline u8 get_gdm_mymac_msb_b(struct gdm_mymac_msb *x)
{
	return FIELD_GET(GDM_MYMAC_MSB_B_MASK, x->word);
}
static inline void set_gdm_mymac_msb_b(struct gdm_mymac_msb *x, u8 v)
{
	x->word = FIELD_SET(x->word, GDM_MYMAC_MSB_B_MASK, v);
}

/**
 * Bitfield accessors for: gdm_len_th
 * Register layout (32-bit word):
 * - Bits 31-16: OVERSIZE_LEN (packet larger than this is treated as oversize)
 * - Bits 15-0: RUNT_LEN (packet smaller than this is treated as a runt)
 */
#define GDM_LEN_TH_OVERSIZE_LEN_MASK			GENMASK(31, 16)
#define GDM_LEN_TH_RUNT_LEN_MASK			GENMASK(15, 0)

static inline u16 get_gdm_len_th_oversize_len(struct gdm_len_th *x)
{
	return FIELD_GET(GDM_LEN_TH_OVERSIZE_LEN_MASK, x->word);
}
static inline void set_gdm_len_th_oversize_len(struct gdm_len_th *x, u16 v)
{
	x->word = FIELD_SET(x->word, GDM_LEN_TH_OVERSIZE_LEN_MASK, v);
}
static inline u16 get_gdm_len_th_runt_len(struct gdm_len_th *x)
{
	return FIELD_GET(GDM_LEN_TH_RUNT_LEN_MASK, x->word);
}
static inline void set_gdm_len_th_runt_len(struct gdm_len_th *x, u16 v)
{
	x->word = FIELD_SET(x->word, GDM_LEN_TH_RUNT_LEN_MASK, v);
}

#endif
