// SPDX-License-Identifier: GPL-2.0
/* Marvell RVU Admin Function driver
 *
 * Copyright (C) 2020 Marvell.
 */

#include <linux/bitfield.h>

#include "rvu_struct.h"
#include "rvu_reg.h"
#include "rvu.h"
#include "npc.h"
#include "rvu_npc_fs.h"
#include "rvu_npc_hash.h"

static const char * const npc_flow_names[] = {
	[NPC_DMAC]	= "dmac",
	[NPC_SMAC]	= "smac",
	[NPC_ETYPE]	= "ether type",
	[NPC_VLAN_ETYPE_CTAG] = "vlan ether type ctag",
	[NPC_VLAN_ETYPE_STAG] = "vlan ether type stag",
	[NPC_OUTER_VID]	= "outer vlan id",
	[NPC_INNER_VID]	= "inner vlan id",
	[NPC_TOS]	= "tos",
	[NPC_IPFRAG_IPV4] = "fragmented IPv4 header ",
	[NPC_SIP_IPV4]	= "ipv4 source ip",
	[NPC_DIP_IPV4]	= "ipv4 destination ip",
	[NPC_IPFRAG_IPV6] = "fragmented IPv6 header ",
	[NPC_SIP_IPV6]	= "ipv6 source ip",
	[NPC_DIP_IPV6]	= "ipv6 destination ip",
	[NPC_IPPROTO_TCP] = "ip proto tcp",
	[NPC_IPPROTO_UDP] = "ip proto udp",
	[NPC_IPPROTO_SCTP] = "ip proto sctp",
	[NPC_IPPROTO_ICMP] = "ip proto icmp",
	[NPC_IPPROTO_ICMP6] = "ip proto icmp6",
	[NPC_IPPROTO_AH] = "ip proto AH",
	[NPC_IPPROTO_ESP] = "ip proto ESP",
	[NPC_SPORT_TCP]	= "tcp source port",
	[NPC_DPORT_TCP]	= "tcp destination port",
	[NPC_SPORT_UDP]	= "udp source port",
	[NPC_DPORT_UDP]	= "udp destination port",
	[NPC_SPORT_SCTP] = "sctp source port",
	[NPC_DPORT_SCTP] = "sctp destination port",
	[NPC_LXMB]	= "Mcast/Bcast header ",
	[NPC_IPSEC_SPI] = "SPI ",
	[NPC_MPLS1_LBTCBOS] = "lse depth 1 label tc bos",
	[NPC_MPLS1_TTL]     = "lse depth 1 ttl",
	[NPC_MPLS2_LBTCBOS] = "lse depth 2 label tc bos",
	[NPC_MPLS2_TTL]     = "lse depth 2 ttl",
	[NPC_MPLS3_LBTCBOS] = "lse depth 3 label tc bos",
	[NPC_MPLS3_TTL]     = "lse depth 3 ttl",
	[NPC_MPLS4_LBTCBOS] = "lse depth 4 label tc bos",
	[NPC_MPLS4_TTL]     = "lse depth 4",
	[NPC_TYPE_ICMP] = "icmp type",
	[NPC_CODE_ICMP] = "icmp code",
	[NPC_TCP_FLAGS] = "tcp flags",
	[NPC_UNKNOWN]	= "unknown",
};

bool npc_is_feature_supported(struct rvu *rvu, u64 features, u8 intf)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;
	u64 mcam_features;
	u64 unsupported;

	mcam_features = is_npc_intf_tx(intf) ? mcam->tx_features : mcam->rx_features;
	unsupported = (mcam_features ^ features) & ~mcam_features;

	/* Return false if at least one of the input flows is not extracted */
	return !unsupported;
}

const char *npc_get_field_name(u8 hdr)
{
	if (hdr >= ARRAY_SIZE(npc_flow_names))
		return npc_flow_names[NPC_UNKNOWN];

	return npc_flow_names[hdr];
}

/* Compute keyword masks and figure out the number of keywords a field
 * spans in the key.
 */
static void npc_set_kw_masks(struct npc_mcam *mcam, u8 type,
			     u8 nr_bits, int start_kwi, int offset, u8 intf)
{
	struct npc_key_field *field = &mcam->rx_key_fields[type];
	u8 bits_in_kw;
	int max_kwi;

	if (mcam->banks_per_entry == 1)
		max_kwi = 1; /* NPC_MCAM_KEY_X1 */
	else if (mcam->banks_per_entry == 2)
		max_kwi = 3; /* NPC_MCAM_KEY_X2 */
	else
		max_kwi = 6; /* NPC_MCAM_KEY_X4 */

	if (is_npc_intf_tx(intf))
		field = &mcam->tx_key_fields[type];

	if (offset + nr_bits <= 64) {
		/* one KW only */
		if (start_kwi > max_kwi)
			return;
		field->kw_mask[start_kwi] |= GENMASK_ULL(nr_bits - 1, 0)
					     << offset;
		field->nr_kws = 1;
	} else if (offset + nr_bits > 64 &&
		   offset + nr_bits <= 128) {
		/* two KWs */
		if (start_kwi + 1 > max_kwi)
			return;
		/* first KW mask */
		bits_in_kw = 64 - offset;
		field->kw_mask[start_kwi] |= GENMASK_ULL(bits_in_kw - 1, 0)
					     << offset;
		/* second KW mask i.e. mask for rest of bits */
		bits_in_kw = nr_bits + offset - 64;
		field->kw_mask[start_kwi + 1] |= GENMASK_ULL(bits_in_kw - 1, 0);
		field->nr_kws = 2;
	} else {
		/* three KWs */
		if (start_kwi + 2 > max_kwi)
			return;
		/* first KW mask */
		bits_in_kw = 64 - offset;
		field->kw_mask[start_kwi] |= GENMASK_ULL(bits_in_kw - 1, 0)
					     << offset;
		/* second KW mask */
		field->kw_mask[start_kwi + 1] = ~0ULL;
		/* third KW mask i.e. mask for rest of bits */
		bits_in_kw = nr_bits + offset - 128;
		field->kw_mask[start_kwi + 2] |= GENMASK_ULL(bits_in_kw - 1, 0);
		field->nr_kws = 3;
	}
}

/* Helper function to figure out whether field exists in the key */
static bool npc_is_field_present(struct rvu *rvu, enum key_fields type, u8 intf)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;
	struct npc_key_field *input;

	input  = &mcam->rx_key_fields[type];
	if (is_npc_intf_tx(intf))
		input  = &mcam->tx_key_fields[type];

	return input->nr_kws > 0;
}

static bool npc_is_same(struct npc_key_field *input,
			struct npc_key_field *field)
{
	return memcmp(&input->layer_mdata, &field->layer_mdata,
		     sizeof(struct npc_layer_mdata)) == 0;
}

static void npc_set_layer_mdata(struct npc_mcam *mcam, enum key_fields type,
				u64 cfg, u8 lid, u8 lt, u8 intf)
{
	struct npc_key_field *input = &mcam->rx_key_fields[type];

	if (is_npc_intf_tx(intf))
		input = &mcam->tx_key_fields[type];

	input->layer_mdata.hdr = FIELD_GET(NPC_HDR_OFFSET, cfg);
	input->layer_mdata.key = FIELD_GET(NPC_KEY_OFFSET, cfg);
	input->layer_mdata.len = FIELD_GET(NPC_BYTESM, cfg) + 1;
	input->layer_mdata.ltype = lt;
	input->layer_mdata.lid = lid;
}

static bool npc_check_overlap_fields(struct npc_key_field *input1,
				     struct npc_key_field *input2)
{
	int kwi;

	/* Fields with same layer id and different ltypes are mutually
	 * exclusive hence they can be overlapped
	 */
	if (input1->layer_mdata.lid == input2->layer_mdata.lid &&
	    input1->layer_mdata.ltype != input2->layer_mdata.ltype)
		return false;

	for (kwi = 0; kwi < NPC_MAX_KWS_IN_KEY; kwi++) {
		if (input1->kw_mask[kwi] & input2->kw_mask[kwi])
			return true;
	}

	return false;
}

/* Helper function to check whether given field overlaps with any other fields
 * in the key. Due to limitations on key size and the key extraction profile in
 * use higher layers can overwrite lower layer's header fields. Hence overlap
 * needs to be checked.
 */
static bool npc_check_overlap(struct rvu *rvu, int blkaddr,
			      enum key_fields type, u8 start_lid, u8 intf)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;
	struct npc_key_field *dummy, *input;
	int start_kwi, offset;
	u8 nr_bits, lid, lt, ld;
	u64 cfg;

	dummy = &mcam->rx_key_fields[NPC_UNKNOWN];
	input = &mcam->rx_key_fields[type];

	if (is_npc_intf_tx(intf)) {
		dummy = &mcam->tx_key_fields[NPC_UNKNOWN];
		input = &mcam->tx_key_fields[type];
	}

	for (lid = start_lid; lid < NPC_MAX_LID; lid++) {
		for (lt = 0; lt < NPC_MAX_LT; lt++) {
			for (ld = 0; ld < NPC_MAX_LD; ld++) {
				cfg = rvu_read64(rvu, blkaddr,
						 NPC_AF_INTFX_LIDX_LTX_LDX_CFG
						 (intf, lid, lt, ld));
				if (!FIELD_GET(NPC_LDATA_EN, cfg))
					continue;
				memset(dummy, 0, sizeof(struct npc_key_field));
				npc_set_layer_mdata(mcam, NPC_UNKNOWN, cfg,
						    lid, lt, intf);
				/* exclude input */
				if (npc_is_same(input, dummy))
					continue;
				start_kwi = dummy->layer_mdata.key / 8;
				offset = (dummy->layer_mdata.key * 8) % 64;
				nr_bits = dummy->layer_mdata.len * 8;
				/* form KW masks */
				npc_set_kw_masks(mcam, NPC_UNKNOWN, nr_bits,
						 start_kwi, offset, intf);
				/* check any input field bits falls in any
				 * other field bits.
				 */
				if (npc_check_overlap_fields(dummy, input))
					return true;
			}
		}
	}

	return false;
}

static bool npc_check_field(struct rvu *rvu, int blkaddr, enum key_fields type,
			    u8 intf)
{
	if (!npc_is_field_present(rvu, type, intf) ||
	    npc_check_overlap(rvu, blkaddr, type, 0, intf))
		return false;
	return true;
}

static void npc_scan_exact_result(struct npc_mcam *mcam, u8 bit_number,
				  u8 key_nibble, u8 intf)
{
	u8 offset = (key_nibble * 4) % 64; /* offset within key word */
	u8 kwi = (key_nibble * 4) / 64; /* which word in key */
	u8 nr_bits = 4; /* bits in a nibble */
	u8 type;

	switch (bit_number) {
	case 40 ... 43:
		type = NPC_EXACT_RESULT;
		break;

	default:
		return;
	}
	npc_set_kw_masks(mcam, type, nr_bits, kwi, offset, intf);
}

static void npc_scan_parse_result(struct npc_mcam *mcam, u8 bit_number,
				  u8 key_nibble, u8 intf)
{
	u8 offset = (key_nibble * 4) % 64; /* offset within key word */
	u8 kwi = (key_nibble * 4) / 64; /* which word in key */
	u8 nr_bits = 4; /* bits in a nibble */
	u8 type;

	switch (bit_number) {
	case 0 ... 2:
		type = NPC_CHAN;
		break;
	case 3:
		type = NPC_ERRLEV;
		break;
	case 4 ... 5:
		type = NPC_ERRCODE;
		break;
	case 6:
		type = NPC_LXMB;
		break;
	/* check for LTYPE only as of now */
	case 9:
		type = NPC_LA;
		break;
	case 12:
		type = NPC_LB;
		break;
	case 15:
		type = NPC_LC;
		break;
	case 18:
		type = NPC_LD;
		break;
	case 21:
		type = NPC_LE;
		break;
	case 24:
		type = NPC_LF;
		break;
	case 27:
		type = NPC_LG;
		break;
	case 30:
		type = NPC_LH;
		break;
	default:
		return;
	}

	npc_set_kw_masks(mcam, type, nr_bits, kwi, offset, intf);
}

static void npc_handle_multi_layer_fields(struct rvu *rvu, int blkaddr, u8 intf)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;
	struct npc_key_field *key_fields;
	/* Ether type can come from three layers
	 * (ethernet, single tagged, double tagged)
	 */
	struct npc_key_field *etype_ether;
	struct npc_key_field *etype_tag1;
	struct npc_key_field *etype_tag2;
	/* Outer VLAN TCI can come from two layers
	 * (single tagged, double tagged)
	 */
	struct npc_key_field *vlan_tag1;
	struct npc_key_field *vlan_tag2;
	/* Inner VLAN TCI for double tagged frames */
	struct npc_key_field *vlan_tag3;
	u64 *features;
	u8 start_lid;
	int i;

	key_fields = mcam->rx_key_fields;
	features = &mcam->rx_features;

	if (is_npc_intf_tx(intf)) {
		key_fields = mcam->tx_key_fields;
		features = &mcam->tx_features;
	}

	/* Handle header fields which can come from multiple layers like
	 * etype, outer vlan tci. These fields should have same position in
	 * the key otherwise to install a mcam rule more than one entry is
	 * needed which complicates mcam space management.
	 */
	etype_ether = &key_fields[NPC_ETYPE_ETHER];
	etype_tag1 = &key_fields[NPC_ETYPE_TAG1];
	etype_tag2 = &key_fields[NPC_ETYPE_TAG2];
	vlan_tag1 = &key_fields[NPC_VLAN_TAG1];
	vlan_tag2 = &key_fields[NPC_VLAN_TAG2];
	vlan_tag3 = &key_fields[NPC_VLAN_TAG3];

	/* if key profile programmed does not extract Ethertype at all */
	if (!etype_ether->nr_kws && !etype_tag1->nr_kws && !etype_tag2->nr_kws) {
		dev_err(rvu->dev, "mkex: Ethertype is not extracted.\n");
		goto vlan_tci;
	}

	/* if key profile programmed extracts Ethertype from one layer */
	if (etype_ether->nr_kws && !etype_tag1->nr_kws && !etype_tag2->nr_kws)
		key_fields[NPC_ETYPE] = *etype_ether;
	if (!etype_ether->nr_kws && etype_tag1->nr_kws && !etype_tag2->nr_kws)
		key_fields[NPC_ETYPE] = *etype_tag1;
	if (!etype_ether->nr_kws && !etype_tag1->nr_kws && etype_tag2->nr_kws)
		key_fields[NPC_ETYPE] = *etype_tag2;

	/* if key profile programmed extracts Ethertype from multiple layers */
	if (etype_ether->nr_kws && etype_tag1->nr_kws) {
		for (i = 0; i < NPC_MAX_KWS_IN_KEY; i++) {
			if (etype_ether->kw_mask[i] != etype_tag1->kw_mask[i]) {
				dev_err(rvu->dev, "mkex: Etype pos is different for untagged and tagged pkts.\n");
				goto vlan_tci;
			}
		}
		key_fields[NPC_ETYPE] = *etype_tag1;
	}
	if (etype_ether->nr_kws && etype_tag2->nr_kws) {
		for (i = 0; i < NPC_MAX_KWS_IN_KEY; i++) {
			if (etype_ether->kw_mask[i] != etype_tag2->kw_mask[i]) {
				dev_err(rvu->dev, "mkex: Etype pos is different for untagged and double tagged pkts.\n");
				goto vlan_tci;
			}
		}
		key_fields[NPC_ETYPE] = *etype_tag2;
	}
	if (etype_tag1->nr_kws && etype_tag2->nr_kws) {
		for (i = 0; i < NPC_MAX_KWS_IN_KEY; i++) {
			if (etype_tag1->kw_mask[i] != etype_tag2->kw_mask[i]) {
				dev_err(rvu->dev, "mkex: Etype pos is different for tagged and double tagged pkts.\n");
				goto vlan_tci;
			}
		}
		key_fields[NPC_ETYPE] = *etype_tag2;
	}

	/* check none of higher layers overwrite Ethertype */
	start_lid = key_fields[NPC_ETYPE].layer_mdata.lid + 1;
	if (npc_check_overlap(rvu, blkaddr, NPC_ETYPE, start_lid, intf)) {
		dev_err(rvu->dev, "mkex: Ethertype is overwritten by higher layers.\n");
		goto vlan_tci;
	}
	*features |= BIT_ULL(NPC_ETYPE);
vlan_tci:
	/* if key profile does not extract outer vlan tci at all */
	if (!vlan_tag1->nr_kws && !vlan_tag2->nr_kws) {
		dev_err(rvu->dev, "mkex: Outer vlan tci is not extracted.\n");
		goto done;
	}

	/* if key profile extracts outer vlan tci from one layer */
	if (vlan_tag1->nr_kws && !vlan_tag2->nr_kws)
		key_fields[NPC_OUTER_VID] = *vlan_tag1;
	if (!vlan_tag1->nr_kws && vlan_tag2->nr_kws)
		key_fields[NPC_OUTER_VID] = *vlan_tag2;

	/* if key profile extracts outer vlan tci from multiple layers */
	if (vlan_tag1->nr_kws && vlan_tag2->nr_kws) {
		for (i = 0; i < NPC_MAX_KWS_IN_KEY; i++) {
			if (vlan_tag1->kw_mask[i] != vlan_tag2->kw_mask[i]) {
				dev_err(rvu->dev, "mkex: Out vlan tci pos is different for tagged and double tagged pkts.\n");
				goto done;
			}
		}
		key_fields[NPC_OUTER_VID] = *vlan_tag2;
	}
	/* check none of higher layers overwrite outer vlan tci */
	start_lid = key_fields[NPC_OUTER_VID].layer_mdata.lid + 1;
	if (npc_check_overlap(rvu, blkaddr, NPC_OUTER_VID, start_lid, intf)) {
		dev_err(rvu->dev, "mkex: Outer vlan tci is overwritten by higher layers.\n");
		goto done;
	}
	*features |= BIT_ULL(NPC_OUTER_VID);

	/* If key profile extracts inner vlan tci */
	if (vlan_tag3->nr_kws) {
		key_fields[NPC_INNER_VID] = *vlan_tag3;
		*features |= BIT_ULL(NPC_INNER_VID);
	}
done:
	return;
}

static void npc_scan_ldata(struct rvu *rvu, int blkaddr, u8 lid,
			   u8 lt, u64 cfg, u8 intf)
{
	struct npc_mcam_kex_hash *mkex_hash = rvu->kpu.mkex_hash;
	struct npc_mcam *mcam = &rvu->hw->mcam;
	u8 hdr, key, nr_bytes, bit_offset;
	u8 la_ltype, la_start;
	/* starting KW index and starting bit position */
	int start_kwi, offset;

	nr_bytes = FIELD_GET(NPC_BYTESM, cfg) + 1;
	hdr = FIELD_GET(NPC_HDR_OFFSET, cfg);
	key = FIELD_GET(NPC_KEY_OFFSET, cfg);

	/* For Tx, Layer A has NIX_INST_HDR_S(64 bytes) preceding
	 * ethernet header.
	 */
	if (is_npc_intf_tx(intf)) {
		la_ltype = NPC_LT_LA_IH_NIX_ETHER;
		la_start = 8;
	} else {
		la_ltype = NPC_LT_LA_ETHER;
		la_start = 0;
	}

#define NPC_SCAN_HDR(name, hlid, hlt, hstart, hlen)			       \
do {									       \
	start_kwi = key / 8;						       \
	offset = (key * 8) % 64;					       \
	if (lid == (hlid) && lt == (hlt)) {				       \
		if ((hstart) >= hdr &&					       \
		    ((hstart) + (hlen)) <= (hdr + nr_bytes)) {	               \
			bit_offset = (hdr + nr_bytes - (hstart) - (hlen)) * 8; \
			npc_set_layer_mdata(mcam, (name), cfg, lid, lt, intf); \
			offset += bit_offset;				       \
			start_kwi += offset / 64;			       \
			offset %= 64;					       \
			npc_set_kw_masks(mcam, (name), (hlen) * 8,	       \
					 start_kwi, offset, intf);	       \
		}							       \
	}								       \
} while (0)

	/* List LID, LTYPE, start offset from layer and length(in bytes) of
	 * packet header fields below.
	 * Example: Source IP is 4 bytes and starts at 12th byte of IP header
	 */
	NPC_SCAN_HDR(NPC_TOS, NPC_LID_LC, NPC_LT_LC_IP, 1, 1);
	NPC_SCAN_HDR(NPC_IPFRAG_IPV4, NPC_LID_LC, NPC_LT_LC_IP, 6, 1);
	NPC_SCAN_HDR(NPC_SIP_IPV4, NPC_LID_LC, NPC_LT_LC_IP, 12, 4);
	NPC_SCAN_HDR(NPC_DIP_IPV4, NPC_LID_LC, NPC_LT_LC_IP, 16, 4);
	NPC_SCAN_HDR(NPC_IPFRAG_IPV6, NPC_LID_LC, NPC_LT_LC_IP6_EXT, 6, 1);
	if (rvu->hw->cap.npc_hash_extract) {
		if (mkex_hash->lid_lt_ld_hash_en[intf][lid][lt][0])
			NPC_SCAN_HDR(NPC_SIP_IPV6, NPC_LID_LC, NPC_LT_LC_IP6, 8, 4);
		else
			NPC_SCAN_HDR(NPC_SIP_IPV6, NPC_LID_LC, NPC_LT_LC_IP6, 8, 16);

		if (mkex_hash->lid_lt_ld_hash_en[intf][lid][lt][1])
			NPC_SCAN_HDR(NPC_DIP_IPV6, NPC_LID_LC, NPC_LT_LC_IP6, 24, 4);
		else
			NPC_SCAN_HDR(NPC_DIP_IPV6, NPC_LID_LC, NPC_LT_LC_IP6, 24, 16);
	} else {
		NPC_SCAN_HDR(NPC_SIP_IPV6, NPC_LID_LC, NPC_LT_LC_IP6, 8, 16);
		NPC_SCAN_HDR(NPC_DIP_IPV6, NPC_LID_LC, NPC_LT_LC_IP6, 24, 16);
	}

	NPC_SCAN_HDR(NPC_SPORT_UDP, NPC_LID_LD, NPC_LT_LD_UDP, 0, 2);
	NPC_SCAN_HDR(NPC_DPORT_UDP, NPC_LID_LD, NPC_LT_LD_UDP, 2, 2);
	NPC_SCAN_HDR(NPC_SPORT_TCP, NPC_LID_LD, NPC_LT_LD_TCP, 0, 2);
	NPC_SCAN_HDR(NPC_DPORT_TCP, NPC_LID_LD, NPC_LT_LD_TCP, 2, 2);
	NPC_SCAN_HDR(NPC_SPORT_SCTP, NPC_LID_LD, NPC_LT_LD_SCTP, 0, 2);
	NPC_SCAN_HDR(NPC_DPORT_SCTP, NPC_LID_LD, NPC_LT_LD_SCTP, 2, 2);
	NPC_SCAN_HDR(NPC_TYPE_ICMP, NPC_LID_LD, NPC_LT_LD_ICMP, 0, 1);
	NPC_SCAN_HDR(NPC_CODE_ICMP, NPC_LID_LD, NPC_LT_LD_ICMP, 1, 1);
	NPC_SCAN_HDR(NPC_TCP_FLAGS, NPC_LID_LD, NPC_LT_LD_TCP, 12, 2);
	NPC_SCAN_HDR(NPC_ETYPE_ETHER, NPC_LID_LA, NPC_LT_LA_ETHER, 12, 2);
	NPC_SCAN_HDR(NPC_ETYPE_TAG1, NPC_LID_LB, NPC_LT_LB_CTAG, 4, 2);
	NPC_SCAN_HDR(NPC_ETYPE_TAG2, NPC_LID_LB, NPC_LT_LB_STAG_QINQ, 8, 2);
	NPC_SCAN_HDR(NPC_VLAN_TAG1, NPC_LID_LB, NPC_LT_LB_CTAG, 2, 2);
	NPC_SCAN_HDR(NPC_VLAN_TAG2, NPC_LID_LB, NPC_LT_LB_STAG_QINQ, 2, 2);
	NPC_SCAN_HDR(NPC_VLAN_TAG3, NPC_LID_LB, NPC_LT_LB_STAG_QINQ, 6, 2);
	NPC_SCAN_HDR(NPC_DMAC, NPC_LID_LA, la_ltype, la_start, 6);

	NPC_SCAN_HDR(NPC_IPSEC_SPI, NPC_LID_LD, NPC_LT_LD_AH, 4, 4);
	NPC_SCAN_HDR(NPC_IPSEC_SPI, NPC_LID_LE, NPC_LT_LE_ESP, 0, 4);
	NPC_SCAN_HDR(NPC_MPLS1_LBTCBOS, NPC_LID_LC, NPC_LT_LC_MPLS, 0, 3);
	NPC_SCAN_HDR(NPC_MPLS1_TTL, NPC_LID_LC, NPC_LT_LC_MPLS, 3, 1);
	NPC_SCAN_HDR(NPC_MPLS2_LBTCBOS, NPC_LID_LC, NPC_LT_LC_MPLS, 4, 3);
	NPC_SCAN_HDR(NPC_MPLS2_TTL, NPC_LID_LC, NPC_LT_LC_MPLS, 7, 1);
	NPC_SCAN_HDR(NPC_MPLS3_LBTCBOS, NPC_LID_LC, NPC_LT_LC_MPLS, 8, 3);
	NPC_SCAN_HDR(NPC_MPLS3_TTL, NPC_LID_LC, NPC_LT_LC_MPLS, 11, 1);
	NPC_SCAN_HDR(NPC_MPLS4_LBTCBOS, NPC_LID_LC, NPC_LT_LC_MPLS, 12, 3);
	NPC_SCAN_HDR(NPC_MPLS4_TTL, NPC_LID_LC, NPC_LT_LC_MPLS, 15, 1);

	/* SMAC follows the DMAC(which is 6 bytes) */
	NPC_SCAN_HDR(NPC_SMAC, NPC_LID_LA, la_ltype, la_start + 6, 6);
	/* PF_FUNC is 2 bytes at 0th byte of NPC_LT_LA_IH_NIX_ETHER */
	NPC_SCAN_HDR(NPC_PF_FUNC, NPC_LID_LA, NPC_LT_LA_IH_NIX_ETHER, 0, 2);
}

static void npc_set_features(struct rvu *rvu, int blkaddr, u8 intf)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;
	u64 *features = &mcam->rx_features;
	u64 proto_flags;
	int hdr;

	if (is_npc_intf_tx(intf))
		features = &mcam->tx_features;

	for (hdr = NPC_DMAC; hdr < NPC_HEADER_FIELDS_MAX; hdr++) {
		if (npc_check_field(rvu, blkaddr, hdr, intf))
			*features |= BIT_ULL(hdr);
	}

	proto_flags = BIT_ULL(NPC_SPORT_TCP) | BIT_ULL(NPC_SPORT_UDP) |
		       BIT_ULL(NPC_DPORT_TCP) | BIT_ULL(NPC_DPORT_UDP) |
		       BIT_ULL(NPC_SPORT_SCTP) | BIT_ULL(NPC_DPORT_SCTP) |
		       BIT_ULL(NPC_SPORT_SCTP) | BIT_ULL(NPC_DPORT_SCTP) |
		       BIT_ULL(NPC_TYPE_ICMP) | BIT_ULL(NPC_CODE_ICMP) |
		       BIT_ULL(NPC_TCP_FLAGS);

	/* for tcp/udp/sctp corresponding layer type should be in the key */
	if (*features & proto_flags) {
		if (!npc_check_field(rvu, blkaddr, NPC_LD, intf))
			*features &= ~proto_flags;
		else
			*features |= BIT_ULL(NPC_IPPROTO_TCP) |
				     BIT_ULL(NPC_IPPROTO_UDP) |
				     BIT_ULL(NPC_IPPROTO_SCTP) |
				     BIT_ULL(NPC_IPPROTO_ICMP);
	}

	/* for AH/ICMP/ICMPv6/, check if corresponding layer type is present in the key */
	if (npc_check_field(rvu, blkaddr, NPC_LD, intf)) {
		*features |= BIT_ULL(NPC_IPPROTO_AH);
		*features |= BIT_ULL(NPC_IPPROTO_ICMP);
		*features |= BIT_ULL(NPC_IPPROTO_ICMP6);
	}

	/* for ESP, check if corresponding layer type is present in the key */
	if (npc_check_field(rvu, blkaddr, NPC_LE, intf))
		*features |= BIT_ULL(NPC_IPPROTO_ESP);

	/* for vlan corresponding layer type should be in the key */
	if (*features & BIT_ULL(NPC_OUTER_VID))
		if (!npc_check_field(rvu, blkaddr, NPC_LB, intf))
			*features &= ~BIT_ULL(NPC_OUTER_VID);

	/* Set SPI flag only if AH/ESP and IPSEC_SPI are in the key */
	if (npc_check_field(rvu, blkaddr, NPC_IPSEC_SPI, intf) &&
	    (*features & (BIT_ULL(NPC_IPPROTO_ESP) | BIT_ULL(NPC_IPPROTO_AH))))
		*features |= BIT_ULL(NPC_IPSEC_SPI);

	/* for vlan ethertypes corresponding layer type should be in the key */
	if (npc_check_field(rvu, blkaddr, NPC_LB, intf))
		*features |= BIT_ULL(NPC_VLAN_ETYPE_CTAG) |
			     BIT_ULL(NPC_VLAN_ETYPE_STAG);

	/* for L2M/L2B/L3M/L3B, check if the type is present in the key */
	if (npc_check_field(rvu, blkaddr, NPC_LXMB, intf))
		*features |= BIT_ULL(NPC_LXMB);

	for (hdr = NPC_MPLS1_LBTCBOS; hdr <= NPC_MPLS4_TTL; hdr++) {
		if (npc_check_field(rvu, blkaddr, hdr, intf))
			*features |= BIT_ULL(hdr);
	}
}

/* Scan key extraction profile and record how fields of our interest
 * fill the key structure. Also verify Channel and DMAC exists in
 * key and not overwritten by other header fields.
 */
static int npc_scan_kex(struct rvu *rvu, int blkaddr, u8 intf)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;
	u8 lid, lt, ld, bitnr;
	u64 cfg, masked_cfg;
	u8 key_nibble = 0;

	/* Scan and note how parse result is going to be in key.
	 * A bit set in PARSE_NIBBLE_ENA corresponds to a nibble from
	 * parse result in the key. The enabled nibbles from parse result
	 * will be concatenated in key.
	 */
	cfg = rvu_read64(rvu, blkaddr, NPC_AF_INTFX_KEX_CFG(intf));
	masked_cfg = cfg & NPC_PARSE_NIBBLE;
	for_each_set_bit(bitnr, (unsigned long *)&masked_cfg, 31) {
		npc_scan_parse_result(mcam, bitnr, key_nibble, intf);
		key_nibble++;
	}

	/* Ignore exact match bits for mcam entries except the first rule
	 * which is drop on hit. This first rule is configured explitcitly by
	 * exact match code.
	 */
	masked_cfg = cfg & NPC_EXACT_NIBBLE;
	bitnr = NPC_EXACT_NIBBLE_START;
	for_each_set_bit_from(bitnr, (unsigned long *)&masked_cfg, NPC_EXACT_NIBBLE_END + 1) {
		npc_scan_exact_result(mcam, bitnr, key_nibble, intf);
		key_nibble++;
	}

	/* Scan and note how layer data is going to be in key */
	for (lid = 0; lid < NPC_MAX_LID; lid++) {
		for (lt = 0; lt < NPC_MAX_LT; lt++) {
			for (ld = 0; ld < NPC_MAX_LD; ld++) {
				cfg = rvu_read64(rvu, blkaddr,
						 NPC_AF_INTFX_LIDX_LTX_LDX_CFG
						 (intf, lid, lt, ld));
				if (!FIELD_GET(NPC_LDATA_EN, cfg))
					continue;
				npc_scan_ldata(rvu, blkaddr, lid, lt, cfg,
					       intf);
			}
		}
	}

	return 0;
}

static int npc_scan_verify_kex(struct rvu *rvu, int blkaddr)
{
	int err;

	err = npc_scan_kex(rvu, blkaddr, NIX_INTF_RX);
	if (err)
		return err;

	err = npc_scan_kex(rvu, blkaddr, NIX_INTF_TX);
	if (err)
		return err;

	/* Channel is mandatory */
	if (!npc_is_field_present(rvu, NPC_CHAN, NIX_INTF_RX)) {
		dev_err(rvu->dev, "Channel not present in Key\n");
		return -EINVAL;
	}
	/* check that none of the fields overwrite channel */
	if (npc_check_overlap(rvu, blkaddr, NPC_CHAN, 0, NIX_INTF_RX)) {
		dev_err(rvu->dev, "Channel cannot be overwritten\n");
		return -EINVAL;
	}

	npc_set_features(rvu, blkaddr, NIX_INTF_TX);
	npc_set_features(rvu, blkaddr, NIX_INTF_RX);
	npc_handle_multi_layer_fields(rvu, blkaddr, NIX_INTF_TX);
	npc_handle_multi_layer_fields(rvu, blkaddr, NIX_INTF_RX);

	return 0;
}

int npc_flow_steering_init(struct rvu *rvu, int blkaddr)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;

	INIT_LIST_HEAD(&mcam->mcam_rules);

	return npc_scan_verify_kex(rvu, blkaddr);
}

static int npc_check_unsupported_flows(struct rvu *rvu, u64 features, u8 intf)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;
	u64 *mcam_features = &mcam->rx_features;
	u64 unsupported;
	u8 bit;

	if (is_npc_intf_tx(intf))
		mcam_features = &mcam->tx_features;

	unsupported = (*mcam_features ^ features) & ~(*mcam_features);
	if (unsupported) {
		dev_warn(rvu->dev, "Unsupported flow(s):\n");
		for_each_set_bit(bit, (unsigned long *)&unsupported, 64)
			dev_warn(rvu->dev, "%s ", npc_get_field_name(bit));
		return -EOPNOTSUPP;
	}

	return 0;
}

/* npc_update_entry - Based on the masks generated during
 * the key scanning, updates the given entry with value and
 * masks for the field of interest. Maximum 16 bytes of a packet
 * header can be extracted by HW hence lo and hi are sufficient.
 * When field bytes are less than or equal to 8 then hi should be
 * 0 for value and mask.
 *
 * If exact match of value is required then mask should be all 1's.
 * If any bits in mask are 0 then corresponding bits in value are
 * dont care.
 */
void npc_update_entry(struct rvu *rvu, enum key_fields type,
		      struct mcam_entry *entry, u64 val_lo,
		      u64 val_hi, u64 mask_lo, u64 mask_hi, u8 intf)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;
	struct mcam_entry dummy = { {0} };
	struct npc_key_field *field;
	u64 kw1, kw2, kw3;
	u8 shift;
	int i;

	field = &mcam->rx_key_fields[type];
	if (is_npc_intf_tx(intf))
		field = &mcam->tx_key_fields[type];

	if (!field->nr_kws)
		return;

	for (i = 0; i < NPC_MAX_KWS_IN_KEY; i++) {
		if (!field->kw_mask[i])
			continue;
		/* place key value in kw[x] */
		shift = __ffs64(field->kw_mask[i]);
		/* update entry value */
		kw1 = (val_lo << shift) & field->kw_mask[i];
		dummy.kw[i] = kw1;
		/* update entry mask */
		kw1 = (mask_lo << shift) & field->kw_mask[i];
		dummy.kw_mask[i] = kw1;

		if (field->nr_kws == 1)
			break;
		/* place remaining bits of key value in kw[x + 1] */
		if (field->nr_kws == 2) {
			/* update entry value */
			kw2 = shift ? val_lo >> (64 - shift) : 0;
			kw2 |= (val_hi << shift);
			kw2 &= field->kw_mask[i + 1];
			dummy.kw[i + 1] = kw2;
			/* update entry mask */
			kw2 = shift ? mask_lo >> (64 - shift) : 0;
			kw2 |= (mask_hi << shift);
			kw2 &= field->kw_mask[i + 1];
			dummy.kw_mask[i + 1] = kw2;
			break;
		}
		/* place remaining bits of key value in kw[x + 1], kw[x + 2] */
		if (field->nr_kws == 3) {
			/* update entry value */
			kw2 = shift ? val_lo >> (64 - shift) : 0;
			kw2 |= (val_hi << shift);
			kw2 &= field->kw_mask[i + 1];
			kw3 = shift ? val_hi >> (64 - shift) : 0;
			kw3 &= field->kw_mask[i + 2];
			dummy.kw[i + 1] = kw2;
			dummy.kw[i + 2] = kw3;
			/* update entry mask */
			kw2 = shift ? mask_lo >> (64 - shift) : 0;
			kw2 |= (mask_hi << shift);
			kw2 &= field->kw_mask[i + 1];
			kw3 = shift ? mask_hi >> (64 - shift) : 0;
			kw3 &= field->kw_mask[i + 2];
			dummy.kw_mask[i + 1] = kw2;
			dummy.kw_mask[i + 2] = kw3;
			break;
		}
	}
	/* dummy is ready with values and masks for given key
	 * field now clear and update input entry with those
	 */
	for (i = 0; i < NPC_MAX_KWS_IN_KEY; i++) {
		if (!field->kw_mask[i])
			continue;
		entry->kw[i] &= ~field->kw_mask[i];
		entry->kw_mask[i] &= ~field->kw_mask[i];

		entry->kw[i] |= dummy.kw[i];
		entry->kw_mask[i] |= dummy.kw_mask[i];
	}
}

static void npc_update_ipv6_flow(struct rvu *rvu, struct mcam_entry *entry,
				 u64 features, struct flow_msg *pkt,
				 struct flow_msg *mask,
				 struct rvu_npc_mcam_rule *output, u8 intf)
{
	u32 src_ip[IPV6_WORDS], src_ip_mask[IPV6_WORDS];
	u32 dst_ip[IPV6_WORDS], dst_ip_mask[IPV6_WORDS];
	struct flow_msg *opkt = &output->packet;
	struct flow_msg *omask = &output->mask;
	u64 mask_lo, mask_hi;
	u64 val_lo, val_hi;

	/* For an ipv6 address fe80::2c68:63ff:fe5e:2d0a the packet
	 * values to be programmed in MCAM should as below:
	 * val_high: 0xfe80000000000000
	 * val_low: 0x2c6863fffe5e2d0a
	 */
	if (features & BIT_ULL(NPC_SIP_IPV6)) {
		be32_to_cpu_array(src_ip_mask, mask->ip6src, IPV6_WORDS);
		be32_to_cpu_array(src_ip, pkt->ip6src, IPV6_WORDS);

		mask_hi = (u64)src_ip_mask[0] << 32 | src_ip_mask[1];
		mask_lo = (u64)src_ip_mask[2] << 32 | src_ip_mask[3];
		val_hi = (u64)src_ip[0] << 32 | src_ip[1];
		val_lo = (u64)src_ip[2] << 32 | src_ip[3];

		npc_update_entry(rvu, NPC_SIP_IPV6, entry, val_lo, val_hi,
				 mask_lo, mask_hi, intf);
		memcpy(opkt->ip6src, pkt->ip6src, sizeof(opkt->ip6src));
		memcpy(omask->ip6src, mask->ip6src, sizeof(omask->ip6src));
	}
	if (features & BIT_ULL(NPC_DIP_IPV6)) {
		be32_to_cpu_array(dst_ip_mask, mask->ip6dst, IPV6_WORDS);
		be32_to_cpu_array(dst_ip, pkt->ip6dst, IPV6_WORDS);

		mask_hi = (u64)dst_ip_mask[0] << 32 | dst_ip_mask[1];
		mask_lo = (u64)dst_ip_mask[2] << 32 | dst_ip_mask[3];
		val_hi = (u64)dst_ip[0] << 32 | dst_ip[1];
		val_lo = (u64)dst_ip[2] << 32 | dst_ip[3];

		npc_update_entry(rvu, NPC_DIP_IPV6, entry, val_lo, val_hi,
				 mask_lo, mask_hi, intf);
		memcpy(opkt->ip6dst, pkt->ip6dst, sizeof(opkt->ip6dst));
		memcpy(omask->ip6dst, mask->ip6dst, sizeof(omask->ip6dst));
	}
}

static void npc_update_vlan_features(struct rvu *rvu, struct mcam_entry *entry,
				     u64 features, u8 intf)
{
	bool ctag = !!(features & BIT_ULL(NPC_VLAN_ETYPE_CTAG));
	bool stag = !!(features & BIT_ULL(NPC_VLAN_ETYPE_STAG));
	bool vid = !!(features & BIT_ULL(NPC_OUTER_VID));

	/* If only VLAN id is given then always match outer VLAN id */
	if (vid && !ctag && !stag) {
		npc_update_entry(rvu, NPC_LB, entry,
				 NPC_LT_LB_STAG_QINQ | NPC_LT_LB_CTAG, 0,
				 NPC_LT_LB_STAG_QINQ & NPC_LT_LB_CTAG, 0, intf);
		return;
	}
	if (ctag)
		npc_update_entry(rvu, NPC_LB, entry, NPC_LT_LB_CTAG, 0,
				 ~0ULL, 0, intf);
	if (stag)
		npc_update_entry(rvu, NPC_LB, entry, NPC_LT_LB_STAG_QINQ, 0,
				 ~0ULL, 0, intf);
}

static void npc_update_flow(struct rvu *rvu, struct mcam_entry *entry,
			    u64 features, struct flow_msg *pkt,
			    struct flow_msg *mask,
			    struct rvu_npc_mcam_rule *output, u8 intf,
			    int blkaddr)
{
	u64 dmac_mask = ether_addr_to_u64(mask->dmac);
	u64 smac_mask = ether_addr_to_u64(mask->smac);
	u64 dmac_val = ether_addr_to_u64(pkt->dmac);
	u64 smac_val = ether_addr_to_u64(pkt->smac);
	struct flow_msg *opkt = &output->packet;
	struct flow_msg *omask = &output->mask;

	if (!features)
		return;

	/* For tcp/udp/sctp LTYPE should be present in entry */
	if (features & BIT_ULL(NPC_IPPROTO_TCP))
		npc_update_entry(rvu, NPC_LD, entry, NPC_LT_LD_TCP,
				 0, ~0ULL, 0, intf);
	if (features & BIT_ULL(NPC_IPPROTO_UDP))
		npc_update_entry(rvu, NPC_LD, entry, NPC_LT_LD_UDP,
				 0, ~0ULL, 0, intf);
	if (features & BIT_ULL(NPC_IPPROTO_SCTP))
		npc_update_entry(rvu, NPC_LD, entry, NPC_LT_LD_SCTP,
				 0, ~0ULL, 0, intf);
	if (features & BIT_ULL(NPC_IPPROTO_ICMP))
		npc_update_entry(rvu, NPC_LD, entry, NPC_LT_LD_ICMP,
				 0, ~0ULL, 0, intf);
	if (features & BIT_ULL(NPC_IPPROTO_ICMP6))
		npc_update_entry(rvu, NPC_LD, entry, NPC_LT_LD_ICMP6,
				 0, ~0ULL, 0, intf);

	/* For AH, LTYPE should be present in entry */
	if (features & BIT_ULL(NPC_IPPROTO_AH))
		npc_update_entry(rvu, NPC_LD, entry, NPC_LT_LD_AH,
				 0, ~0ULL, 0, intf);
	/* For ESP, LTYPE should be present in entry */
	if (features & BIT_ULL(NPC_IPPROTO_ESP))
		npc_update_entry(rvu, NPC_LE, entry, NPC_LT_LE_ESP,
				 0, ~0ULL, 0, intf);

	if (features & BIT_ULL(NPC_LXMB)) {
		output->lxmb = is_broadcast_ether_addr(pkt->dmac) ? 2 : 1;
		npc_update_entry(rvu, NPC_LXMB, entry, output->lxmb, 0,
				 output->lxmb, 0, intf);
	}
#define NPC_WRITE_FLOW(field, member, val_lo, val_hi, mask_lo, mask_hi)	      \
do {									      \
	if (features & BIT_ULL((field))) {				      \
		npc_update_entry(rvu, (field), entry, (val_lo), (val_hi),     \
				 (mask_lo), (mask_hi), intf);		      \
		memcpy(&opkt->member, &pkt->member, sizeof(pkt->member));     \
		memcpy(&omask->member, &mask->member, sizeof(mask->member));  \
	}								      \
} while (0)

	NPC_WRITE_FLOW(NPC_DMAC, dmac, dmac_val, 0, dmac_mask, 0);

	NPC_WRITE_FLOW(NPC_SMAC, smac, smac_val, 0, smac_mask, 0);
	NPC_WRITE_FLOW(NPC_ETYPE, etype, ntohs(pkt->etype), 0,
		       ntohs(mask->etype), 0);
	NPC_WRITE_FLOW(NPC_TOS, tos, pkt->tos, 0, mask->tos, 0);
	NPC_WRITE_FLOW(NPC_IPFRAG_IPV4, ip_flag, pkt->ip_flag, 0,
		       mask->ip_flag, 0);
	NPC_WRITE_FLOW(NPC_SIP_IPV4, ip4src, ntohl(pkt->ip4src), 0,
		       ntohl(mask->ip4src), 0);
	NPC_WRITE_FLOW(NPC_DIP_IPV4, ip4dst, ntohl(pkt->ip4dst), 0,
		       ntohl(mask->ip4dst), 0);
	NPC_WRITE_FLOW(NPC_SPORT_TCP, sport, ntohs(pkt->sport), 0,
		       ntohs(mask->sport), 0);
	NPC_WRITE_FLOW(NPC_SPORT_UDP, sport, ntohs(pkt->sport), 0,
		       ntohs(mask->sport), 0);
	NPC_WRITE_FLOW(NPC_DPORT_TCP, dport, ntohs(pkt->dport), 0,
		       ntohs(mask->dport), 0);
	NPC_WRITE_FLOW(NPC_DPORT_UDP, dport, ntohs(pkt->dport), 0,
		       ntohs(mask->dport), 0);
	NPC_WRITE_FLOW(NPC_SPORT_SCTP, sport, ntohs(pkt->sport), 0,
		       ntohs(mask->sport), 0);
	NPC_WRITE_FLOW(NPC_DPORT_SCTP, dport, ntohs(pkt->dport), 0,
		       ntohs(mask->dport), 0);
	NPC_WRITE_FLOW(NPC_TYPE_ICMP, icmp_type, pkt->icmp_type, 0,
		       mask->icmp_type, 0);
	NPC_WRITE_FLOW(NPC_CODE_ICMP, icmp_code, pkt->icmp_code, 0,
		       mask->icmp_code, 0);
	NPC_WRITE_FLOW(NPC_TCP_FLAGS, tcp_flags, ntohs(pkt->tcp_flags), 0,
		       ntohs(mask->tcp_flags), 0);
	NPC_WRITE_FLOW(NPC_IPSEC_SPI, spi, ntohl(pkt->spi), 0,
		       ntohl(mask->spi), 0);

	NPC_WRITE_FLOW(NPC_OUTER_VID, vlan_tci, ntohs(pkt->vlan_tci), 0,
		       ntohs(mask->vlan_tci), 0);
	NPC_WRITE_FLOW(NPC_INNER_VID, vlan_itci, ntohs(pkt->vlan_itci), 0,
		       ntohs(mask->vlan_itci), 0);

	NPC_WRITE_FLOW(NPC_MPLS1_LBTCBOS, mpls_lse,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_NON_TTL,
				 pkt->mpls_lse[0]), 0,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_NON_TTL,
				 mask->mpls_lse[0]), 0);
	NPC_WRITE_FLOW(NPC_MPLS1_TTL, mpls_lse,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_TTL,
				 pkt->mpls_lse[0]), 0,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_TTL,
				 mask->mpls_lse[0]), 0);
	NPC_WRITE_FLOW(NPC_MPLS2_LBTCBOS, mpls_lse,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_NON_TTL,
				 pkt->mpls_lse[1]), 0,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_NON_TTL,
				 mask->mpls_lse[1]), 0);
	NPC_WRITE_FLOW(NPC_MPLS2_TTL, mpls_lse,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_TTL,
				 pkt->mpls_lse[1]), 0,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_TTL,
				 mask->mpls_lse[1]), 0);
	NPC_WRITE_FLOW(NPC_MPLS3_LBTCBOS, mpls_lse,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_NON_TTL,
				 pkt->mpls_lse[2]), 0,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_NON_TTL,
				 mask->mpls_lse[2]), 0);
	NPC_WRITE_FLOW(NPC_MPLS3_TTL, mpls_lse,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_TTL,
				 pkt->mpls_lse[2]), 0,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_TTL,
				 mask->mpls_lse[2]), 0);
	NPC_WRITE_FLOW(NPC_MPLS4_LBTCBOS, mpls_lse,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_NON_TTL,
				 pkt->mpls_lse[3]), 0,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_NON_TTL,
				 mask->mpls_lse[3]), 0);
	NPC_WRITE_FLOW(NPC_MPLS4_TTL, mpls_lse,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_TTL,
				 pkt->mpls_lse[3]), 0,
		       FIELD_GET(OTX2_FLOWER_MASK_MPLS_TTL,
				 mask->mpls_lse[3]), 0);

	NPC_WRITE_FLOW(NPC_IPFRAG_IPV6, next_header, pkt->next_header, 0,
		       mask->next_header, 0);
	npc_update_ipv6_flow(rvu, entry, features, pkt, mask, output, intf);
	npc_update_vlan_features(rvu, entry, features, intf);

	npc_update_field_hash(rvu, intf, entry, blkaddr, features,
			      pkt, mask, opkt, omask);
}

static struct rvu_npc_mcam_rule *rvu_mcam_find_rule(struct npc_mcam *mcam, u16 entry)
{
	struct rvu_npc_mcam_rule *iter;

	mutex_lock(&mcam->lock);
	list_for_each_entry(iter, &mcam->mcam_rules, list) {
		if (iter->entry == entry) {
			mutex_unlock(&mcam->lock);
			return iter;
		}
	}
	mutex_unlock(&mcam->lock);

	return NULL;
}

static void rvu_mcam_add_rule(struct npc_mcam *mcam,
			      struct rvu_npc_mcam_rule *rule)
{
	struct list_head *head = &mcam->mcam_rules;
	struct rvu_npc_mcam_rule *iter;

	mutex_lock(&mcam->lock);
	list_for_each_entry(iter, &mcam->mcam_rules, list) {
		if (iter->entry > rule->entry)
			break;
		head = &iter->list;
	}

	list_add(&rule->list, head);
	mutex_unlock(&mcam->lock);
}

static void rvu_mcam_remove_counter_from_rule(struct rvu *rvu, u16 pcifunc,
					      struct rvu_npc_mcam_rule *rule)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;

	mutex_lock(&mcam->lock);

	__rvu_mcam_remove_counter_from_rule(rvu, pcifunc, rule);

	mutex_unlock(&mcam->lock);
}

static void rvu_mcam_add_counter_to_rule(struct rvu *rvu, u16 pcifunc,
					 struct rvu_npc_mcam_rule *rule,
					 struct npc_install_flow_rsp *rsp)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;

	mutex_lock(&mcam->lock);

	__rvu_mcam_add_counter_to_rule(rvu, pcifunc, rule, rsp);

	mutex_unlock(&mcam->lock);
}

static int npc_mcast_update_action_index(struct rvu *rvu, struct npc_install_flow_req *req,
					 u64 op, void *action)
{
	int mce_index;

	/* If a PF/VF is installing a multicast rule then it is expected
	 * that the PF/VF should have created a group for the multicast/mirror
	 * list. Otherwise reject the configuration.
	 * During this scenario, req->index is set as multicast/mirror
	 * group index.
	 */
	if (req->hdr.pcifunc &&
	    (op == NIX_RX_ACTIONOP_MCAST || op == NIX_TX_ACTIONOP_MCAST)) {
		mce_index = rvu_nix_mcast_get_mce_index(rvu, req->hdr.pcifunc, req->index);
		if (mce_index < 0)
			return mce_index;

		if (op == NIX_RX_ACTIONOP_MCAST)
			((struct nix_rx_action *)action)->index = mce_index;
		else
			((struct nix_tx_action *)action)->index = mce_index;
	}

	return 0;
}

static int npc_update_rx_entry(struct rvu *rvu, struct rvu_pfvf *pfvf,
			       struct mcam_entry *entry,
			       struct npc_install_flow_req *req,
			       u16 target, bool pf_set_vfs_mac)
{
	struct rvu_switch *rswitch = &rvu->rswitch;
	struct nix_rx_action action;
	int ret;

	if (rswitch->mode == DEVLINK_ESWITCH_MODE_SWITCHDEV && pf_set_vfs_mac)
		req->chan_mask = 0x0; /* Do not care channel */

	npc_update_entry(rvu, NPC_CHAN, entry, req->channel, 0, req->chan_mask,
			 0, NIX_INTF_RX);

	*(u64 *)&action = 0x00;
	action.pf_func = target;
	action.op = req->op;
	action.index = req->index;

	ret = npc_mcast_update_action_index(rvu, req, action.op, (void *)&action);
	if (ret)
		return ret;

	action.match_id = req->match_id;
	action.flow_key_alg = req->flow_key_alg;

	if (req->op == NIX_RX_ACTION_DEFAULT) {
		if (pfvf->def_ucast_rule) {
			action = pfvf->def_ucast_rule->rx_action;
		} else {
			/* For profiles which do not extract DMAC, the default
			 * unicast entry is unused. Hence modify action for the
			 * requests which use same action as default unicast
			 * entry
			 */
			*(u64 *)&action = 0;
			action.pf_func = target;
			action.op = NIX_RX_ACTIONOP_UCAST;
		}
		if (req->match_id)
			action.match_id = req->match_id;
	}

	entry->action = *(u64 *)&action;

	/* VTAG0 starts at 0th byte of LID_B.
	 * VTAG1 starts at 4th byte of LID_B.
	 */
	entry->vtag_action = FIELD_PREP(RX_VTAG0_VALID_BIT, req->vtag0_valid) |
			     FIELD_PREP(RX_VTAG0_TYPE_MASK, req->vtag0_type) |
			     FIELD_PREP(RX_VTAG0_LID_MASK, NPC_LID_LB) |
			     FIELD_PREP(RX_VTAG0_RELPTR_MASK, 0) |
			     FIELD_PREP(RX_VTAG1_VALID_BIT, req->vtag1_valid) |
			     FIELD_PREP(RX_VTAG1_TYPE_MASK, req->vtag1_type) |
			     FIELD_PREP(RX_VTAG1_LID_MASK, NPC_LID_LB) |
			     FIELD_PREP(RX_VTAG1_RELPTR_MASK, 4);

	return 0;
}

static int npc_update_tx_entry(struct rvu *rvu, struct rvu_pfvf *pfvf,
			       struct mcam_entry *entry,
			       struct npc_install_flow_req *req, u16 target)
{
	struct nix_tx_action action;
	u64 mask = ~0ULL;
	int ret;

	/* If AF is installing then do not care about
	 * PF_FUNC in Send Descriptor
	 */
	if (is_pffunc_af(req->hdr.pcifunc))
		mask = 0;

	npc_update_entry(rvu, NPC_PF_FUNC, entry, (__force u16)htons(target),
			 0, mask, 0, NIX_INTF_TX);

	*(u64 *)&action = 0x00;
	action.op = req->op;
	action.index = req->index;

	ret = npc_mcast_update_action_index(rvu, req, action.op, (void *)&action);
	if (ret)
		return ret;

	action.match_id = req->match_id;

	entry->action = *(u64 *)&action;

	/* VTAG0 starts at 0th byte of LID_B.
	 * VTAG1 starts at 4th byte of LID_B.
	 */
	entry->vtag_action = FIELD_PREP(TX_VTAG0_DEF_MASK, req->vtag0_def) |
			     FIELD_PREP(TX_VTAG0_OP_MASK, req->vtag0_op) |
			     FIELD_PREP(TX_VTAG0_LID_MASK, NPC_LID_LA) |
			     FIELD_PREP(TX_VTAG0_RELPTR_MASK, 20) |
			     FIELD_PREP(TX_VTAG1_DEF_MASK, req->vtag1_def) |
			     FIELD_PREP(TX_VTAG1_OP_MASK, req->vtag1_op) |
			     FIELD_PREP(TX_VTAG1_LID_MASK, NPC_LID_LA) |
			     FIELD_PREP(TX_VTAG1_RELPTR_MASK, 24);

	return 0;
}

static int npc_install_flow(struct rvu *rvu, int blkaddr, u16 target,
			    int nixlf, struct rvu_pfvf *pfvf,
			    struct npc_install_flow_req *req,
			    struct npc_install_flow_rsp *rsp, bool enable,
			    bool pf_set_vfs_mac)
{
	struct rvu_npc_mcam_rule *def_ucast_rule = pfvf->def_ucast_rule;
	u64 features, installed_features, missing_features = 0;
	struct npc_mcam_write_entry_req write_req = { 0 };
	struct npc_mcam *mcam = &rvu->hw->mcam;
	struct rvu_npc_mcam_rule dummy = { 0 };
	struct rvu_npc_mcam_rule *rule;
	u16 owner = req->hdr.pcifunc;
	struct msg_rsp write_rsp;
	struct mcam_entry *entry;
	bool new = false;
	u16 entry_index;
	int err;

	installed_features = req->features;
	features = req->features;
	entry = &write_req.entry_data;
	entry_index = req->entry;

	npc_update_flow(rvu, entry, features, &req->packet, &req->mask, &dummy,
			req->intf, blkaddr);

	if (is_npc_intf_rx(req->intf)) {
		err = npc_update_rx_entry(rvu, pfvf, entry, req, target, pf_set_vfs_mac);
		if (err)
			return err;
	} else {
		err = npc_update_tx_entry(rvu, pfvf, entry, req, target);
		if (err)
			return err;
	}

	/* Default unicast rules do not exist for TX */
	if (is_npc_intf_tx(req->intf))
		goto find_rule;

	if (req->default_rule) {
		entry_index = npc_get_nixlf_mcam_index(mcam, target, nixlf,
						       NIXLF_UCAST_ENTRY);
		enable = is_mcam_entry_enabled(rvu, mcam, blkaddr, entry_index);
	}

	/* update mcam entry with default unicast rule attributes */
	if (def_ucast_rule && (req->default_rule && req->append)) {
		missing_features = (def_ucast_rule->features ^ features) &
					def_ucast_rule->features;
		if (missing_features)
			npc_update_flow(rvu, entry, missing_features,
					&def_ucast_rule->packet,
					&def_ucast_rule->mask,
					&dummy, req->intf,
					blkaddr);
		installed_features = req->features | missing_features;
	}

find_rule:
	rule = rvu_mcam_find_rule(mcam, entry_index);
	if (!rule) {
		rule = kzalloc(sizeof(*rule), GFP_KERNEL);
		if (!rule)
			return -ENOMEM;
		new = true;
	}

	/* allocate new counter if rule has no counter */
	if (!req->default_rule && req->set_cntr && !rule->has_cntr)
		rvu_mcam_add_counter_to_rule(rvu, owner, rule, rsp);

	/* if user wants to delete an existing counter for a rule then
	 * free the counter
	 */
	if (!req->set_cntr && rule->has_cntr)
		rvu_mcam_remove_counter_from_rule(rvu, owner, rule);

	write_req.hdr.pcifunc = owner;

	/* AF owns the default rules so change the owner just to relax
	 * the checks in rvu_mbox_handler_npc_mcam_write_entry
	 */
	if (req->default_rule)
		write_req.hdr.pcifunc = 0;

	write_req.entry = entry_index;
	write_req.intf = req->intf;
	write_req.enable_entry = (u8)enable;
	/* if counter is available then clear and use it */
	if (req->set_cntr && rule->has_cntr) {
		rvu_write64(rvu, blkaddr, NPC_AF_MATCH_STATX(rule->cntr), req->cntr_val);
		write_req.set_cntr = 1;
		write_req.cntr = rule->cntr;
	}

	/* update rule */
	memcpy(&rule->packet, &dummy.packet, sizeof(rule->packet));
	memcpy(&rule->mask, &dummy.mask, sizeof(rule->mask));
	rule->entry = entry_index;
	memcpy(&rule->rx_action, &entry->action, sizeof(struct nix_rx_action));
	if (is_npc_intf_tx(req->intf))
		memcpy(&rule->tx_action, &entry->action,
		       sizeof(struct nix_tx_action));
	rule->vtag_action = entry->vtag_action;
	rule->features = installed_features;
	rule->default_rule = req->default_rule;
	rule->owner = owner;
	rule->enable = enable;
	rule->chan_mask = write_req.entry_data.kw_mask[0] & NPC_KEX_CHAN_MASK;
	rule->chan = write_req.entry_data.kw[0] & NPC_KEX_CHAN_MASK;
	rule->chan &= rule->chan_mask;
	rule->lxmb = dummy.lxmb;
	if (is_npc_intf_tx(req->intf))
		rule->intf = pfvf->nix_tx_intf;
	else
		rule->intf = pfvf->nix_rx_intf;

	if (new)
		rvu_mcam_add_rule(mcam, rule);
	if (req->default_rule)
		pfvf->def_ucast_rule = rule;

	/* write to mcam entry registers */
	err = rvu_mbox_handler_npc_mcam_write_entry(rvu, &write_req,
						    &write_rsp);
	if (err) {
		rvu_mcam_remove_counter_from_rule(rvu, owner, rule);
		if (new) {
			list_del(&rule->list);
			kfree(rule);
		}
		return err;
	}

	/* VF's MAC address is being changed via PF  */
	if (pf_set_vfs_mac) {
		ether_addr_copy(pfvf->default_mac, req->packet.dmac);
		ether_addr_copy(pfvf->mac_addr, req->packet.dmac);
		set_bit(PF_SET_VF_MAC, &pfvf->flags);
	}

	if (test_bit(PF_SET_VF_CFG, &pfvf->flags) &&
	    req->vtag0_type == NIX_AF_LFX_RX_VTAG_TYPE7)
		rule->vfvlan_cfg = true;

	if (is_npc_intf_rx(req->intf) && req->match_id &&
	    (req->op == NIX_RX_ACTIONOP_UCAST || req->op == NIX_RX_ACTIONOP_RSS))
		return rvu_nix_setup_ratelimit_aggr(rvu, req->hdr.pcifunc,
					     req->index, req->match_id);

	if (owner && req->op == NIX_RX_ACTIONOP_MCAST)
		return rvu_nix_mcast_update_mcam_entry(rvu, req->hdr.pcifunc,
						       req->index, entry_index);

	return 0;
}

int rvu_mbox_handler_npc_install_flow(struct rvu *rvu,
				      struct npc_install_flow_req *req,
				      struct npc_install_flow_rsp *rsp)
{
	bool from_vf = !!(req->hdr.pcifunc & RVU_PFVF_FUNC_MASK);
	bool from_rep_dev = !!is_rep_dev(rvu, req->hdr.pcifunc);
	struct rvu_switch *rswitch = &rvu->rswitch;
	int blkaddr, nixlf, err;
	struct rvu_pfvf *pfvf;
	bool pf_set_vfs_mac = false;
	bool enable = true;
	u16 target;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_NPC, 0);
	if (blkaddr < 0) {
		dev_err(rvu->dev, "%s: NPC block not implemented\n", __func__);
		return NPC_MCAM_INVALID_REQ;
	}

	if (!is_npc_interface_valid(rvu, req->intf))
		return NPC_FLOW_INTF_INVALID;

	/* If DMAC is not extracted in MKEX, rules installed by AF
	 * can rely on L2MB bit set by hardware protocol checker for
	 * broadcast and multicast addresses.
	 */
	if (npc_check_field(rvu, blkaddr, NPC_DMAC, req->intf))
		goto process_flow;

	if (is_pffunc_af(req->hdr.pcifunc) &&
	    req->features & BIT_ULL(NPC_DMAC)) {
		if (is_unicast_ether_addr(req->packet.dmac)) {
			dev_warn(rvu->dev,
				 "%s: mkex profile does not support ucast flow\n",
				 __func__);
			return NPC_FLOW_NOT_SUPPORTED;
		}

		if (!npc_is_field_present(rvu, NPC_LXMB, req->intf)) {
			dev_warn(rvu->dev,
				 "%s: mkex profile does not support bcast/mcast flow",
				 __func__);
			return NPC_FLOW_NOT_SUPPORTED;
		}

		/* Modify feature to use LXMB instead of DMAC */
		req->features &= ~BIT_ULL(NPC_DMAC);
		req->features |= BIT_ULL(NPC_LXMB);
	}

process_flow:
	if (from_vf && req->default_rule)
		return NPC_FLOW_VF_PERM_DENIED;

	/* Each PF/VF info is maintained in struct rvu_pfvf.
	 * rvu_pfvf for the target PF/VF needs to be retrieved
	 * hence modify pcifunc accordingly.
	 */

	if (!req->hdr.pcifunc) {
		/* AF installing for a PF/VF */
		target = req->vf;
	} else if (!from_vf && req->vf && !from_rep_dev) {
		/* PF installing for its VF */
		target = (req->hdr.pcifunc & ~RVU_PFVF_FUNC_MASK) | req->vf;
		pf_set_vfs_mac = req->default_rule &&
				(req->features & BIT_ULL(NPC_DMAC));
	} else if (from_rep_dev && req->vf) {
		/* Representor device installing for a representee */
		target = req->vf;
	} else {
		/* msg received from PF/VF */
		target = req->hdr.pcifunc;
	}

	/* ignore chan_mask in case pf func is not AF, revisit later */
	if (!is_pffunc_af(req->hdr.pcifunc))
		req->chan_mask = 0xFFF;

	err = npc_check_unsupported_flows(rvu, req->features, req->intf);
	if (err)
		return NPC_FLOW_NOT_SUPPORTED;

	pfvf = rvu_get_pfvf(rvu, target);

	if (from_rep_dev)
		req->channel = pfvf->rx_chan_base;
	/* PF installing for its VF */
	if (req->hdr.pcifunc && !from_vf && req->vf && !from_rep_dev)
		set_bit(PF_SET_VF_CFG, &pfvf->flags);

	/* update req destination mac addr */
	if ((req->features & BIT_ULL(NPC_DMAC)) && is_npc_intf_rx(req->intf) &&
	    is_zero_ether_addr(req->packet.dmac)) {
		ether_addr_copy(req->packet.dmac, pfvf->mac_addr);
		eth_broadcast_addr((u8 *)&req->mask.dmac);
	}

	/* Proceed if NIXLF is attached or not for TX rules */
	err = nix_get_nixlf(rvu, target, &nixlf, NULL);
	if (err && is_npc_intf_rx(req->intf) && !pf_set_vfs_mac)
		return NPC_FLOW_NO_NIXLF;

	/* don't enable rule when nixlf not attached or initialized */
	if (!(is_nixlf_attached(rvu, target) &&
	      test_bit(NIXLF_INITIALIZED, &pfvf->flags)))
		enable = false;

	/* Packets reaching NPC in Tx path implies that a
	 * NIXLF is properly setup and transmitting.
	 * Hence rules can be enabled for Tx.
	 */
	if (is_npc_intf_tx(req->intf))
		enable = true;

	/* Do not allow requests from uninitialized VFs */
	if (from_vf && !enable)
		return NPC_FLOW_VF_NOT_INIT;

	/* PF sets VF mac & VF NIXLF is not attached, update the mac addr */
	if (pf_set_vfs_mac && !enable) {
		ether_addr_copy(pfvf->default_mac, req->packet.dmac);
		ether_addr_copy(pfvf->mac_addr, req->packet.dmac);
		set_bit(PF_SET_VF_MAC, &pfvf->flags);
		return 0;
	}

	mutex_lock(&rswitch->switch_lock);
	err = npc_install_flow(rvu, blkaddr, target, nixlf, pfvf,
			       req, rsp, enable, pf_set_vfs_mac);
	mutex_unlock(&rswitch->switch_lock);

	return err;
}

static int npc_delete_flow(struct rvu *rvu, struct rvu_npc_mcam_rule *rule,
			   u16 pcifunc)
{
	struct npc_mcam_ena_dis_entry_req dis_req = { 0 };
	struct msg_rsp dis_rsp;

	if (rule->default_rule)
		return 0;

	if (rule->has_cntr)
		rvu_mcam_remove_counter_from_rule(rvu, pcifunc, rule);

	dis_req.hdr.pcifunc = pcifunc;
	dis_req.entry = rule->entry;

	list_del(&rule->list);
	kfree(rule);

	return rvu_mbox_handler_npc_mcam_dis_entry(rvu, &dis_req, &dis_rsp);
}

int rvu_mbox_handler_npc_delete_flow(struct rvu *rvu,
				     struct npc_delete_flow_req *req,
				     struct npc_delete_flow_rsp *rsp)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;
	struct rvu_npc_mcam_rule *iter, *tmp;
	u16 pcifunc = req->hdr.pcifunc;
	struct list_head del_list;
	int blkaddr;

	INIT_LIST_HEAD(&del_list);

	mutex_lock(&mcam->lock);
	list_for_each_entry_safe(iter, tmp, &mcam->mcam_rules, list) {
		if (iter->owner == pcifunc) {
			/* All rules */
			if (req->all) {
				list_move_tail(&iter->list, &del_list);
			/* Range of rules */
			} else if (req->end && iter->entry >= req->start &&
				   iter->entry <= req->end) {
				list_move_tail(&iter->list, &del_list);
			/* single rule */
			} else if (req->entry == iter->entry) {
				blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_NPC, 0);
				if (blkaddr)
					rsp->cntr_val = rvu_read64(rvu, blkaddr,
								   NPC_AF_MATCH_STATX(iter->cntr));
				list_move_tail(&iter->list, &del_list);
				break;
			}
		}
	}
	mutex_unlock(&mcam->lock);

	list_for_each_entry_safe(iter, tmp, &del_list, list) {
		u16 entry = iter->entry;

		/* clear the mcam entry target pcifunc */
		mcam->entry2target_pffunc[entry] = 0x0;
		if (npc_delete_flow(rvu, iter, pcifunc))
			dev_err(rvu->dev, "rule deletion failed for entry:%u",
				entry);
	}

	return 0;
}

static int npc_update_dmac_value(struct rvu *rvu, int npcblkaddr,
				 struct rvu_npc_mcam_rule *rule,
				 struct rvu_pfvf *pfvf)
{
	struct npc_mcam_write_entry_req write_req = { 0 };
	struct mcam_entry *entry = &write_req.entry_data;
	struct npc_mcam *mcam = &rvu->hw->mcam;
	struct msg_rsp rsp;
	u8 intf, enable;
	int err;

	ether_addr_copy(rule->packet.dmac, pfvf->mac_addr);

	npc_read_mcam_entry(rvu, mcam, npcblkaddr, rule->entry,
			    entry, &intf,  &enable);

	npc_update_entry(rvu, NPC_DMAC, entry,
			 ether_addr_to_u64(pfvf->mac_addr), 0,
			 0xffffffffffffull, 0, intf);

	write_req.hdr.pcifunc = rule->owner;
	write_req.entry = rule->entry;
	write_req.intf = pfvf->nix_rx_intf;

	mutex_unlock(&mcam->lock);
	err = rvu_mbox_handler_npc_mcam_write_entry(rvu, &write_req, &rsp);
	mutex_lock(&mcam->lock);

	return err;
}

void npc_mcam_enable_flows(struct rvu *rvu, u16 target)
{
	struct rvu_pfvf *pfvf = rvu_get_pfvf(rvu, target);
	struct rvu_npc_mcam_rule *def_ucast_rule;
	struct npc_mcam *mcam = &rvu->hw->mcam;
	struct rvu_npc_mcam_rule *rule;
	int blkaddr, bank, index;
	u64 def_action;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_NPC, 0);
	if (blkaddr < 0)
		return;

	def_ucast_rule = pfvf->def_ucast_rule;

	mutex_lock(&mcam->lock);
	list_for_each_entry(rule, &mcam->mcam_rules, list) {
		if (is_npc_intf_rx(rule->intf) &&
		    rule->rx_action.pf_func == target && !rule->enable) {
			if (rule->default_rule) {
				npc_enable_mcam_entry(rvu, mcam, blkaddr,
						      rule->entry, true);
				rule->enable = true;
				continue;
			}

			if (rule->vfvlan_cfg)
				npc_update_dmac_value(rvu, blkaddr, rule, pfvf);

			if (rule->rx_action.op == NIX_RX_ACTION_DEFAULT) {
				if (!def_ucast_rule)
					continue;
				/* Use default unicast entry action */
				rule->rx_action = def_ucast_rule->rx_action;
				def_action = *(u64 *)&def_ucast_rule->rx_action;
				bank = npc_get_bank(mcam, rule->entry);
				rvu_write64(rvu, blkaddr,
					    NPC_AF_MCAMEX_BANKX_ACTION
					    (rule->entry, bank), def_action);
			}

			npc_enable_mcam_entry(rvu, mcam, blkaddr,
					      rule->entry, true);
			rule->enable = true;
		}
	}

	/* Enable MCAM entries installed by PF with target as VF pcifunc */
	for (index = 0; index < mcam->bmap_entries; index++) {
		if (mcam->entry2target_pffunc[index] == target)
			npc_enable_mcam_entry(rvu, mcam, blkaddr,
					      index, true);
	}
	mutex_unlock(&mcam->lock);
}

void npc_mcam_disable_flows(struct rvu *rvu, u16 target)
{
	struct npc_mcam *mcam = &rvu->hw->mcam;
	int blkaddr, index;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_NPC, 0);
	if (blkaddr < 0)
		return;

	mutex_lock(&mcam->lock);
	/* Disable MCAM entries installed by PF with target as VF pcifunc */
	for (index = 0; index < mcam->bmap_entries; index++) {
		if (mcam->entry2target_pffunc[index] == target)
			npc_enable_mcam_entry(rvu, mcam, blkaddr,
					      index, false);
	}
	mutex_unlock(&mcam->lock);
}

/* single drop on non hit rule starting from 0th index. This an extension
 * to RPM mac filter to support more rules.
 */
int npc_install_mcam_drop_rule(struct rvu *rvu, int mcam_idx, u16 *counter_idx,
			       u64 chan_val, u64 chan_mask, u64 exact_val, u64 exact_mask,
			       u64 bcast_mcast_val, u64 bcast_mcast_mask)
{
	struct npc_mcam_alloc_counter_req cntr_req = { 0 };
	struct npc_mcam_alloc_counter_rsp cntr_rsp = { 0 };
	struct npc_mcam_write_entry_req req = { 0 };
	struct npc_mcam *mcam = &rvu->hw->mcam;
	struct rvu_npc_mcam_rule *rule;
	struct msg_rsp rsp;
	bool enabled;
	int blkaddr;
	int err;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_NPC, 0);
	if (blkaddr < 0) {
		dev_err(rvu->dev, "%s: NPC block not implemented\n", __func__);
		return -ENODEV;
	}

	/* Bail out if no exact match support */
	if (!rvu_npc_exact_has_match_table(rvu)) {
		dev_info(rvu->dev, "%s: No support for exact match feature\n", __func__);
		return -EINVAL;
	}

	/* If 0th entry is already used, return err */
	enabled = is_mcam_entry_enabled(rvu, mcam, blkaddr, mcam_idx);
	if (enabled) {
		dev_err(rvu->dev, "%s: failed to add single drop on non hit rule at %d th index\n",
			__func__, mcam_idx);
		return	-EINVAL;
	}

	/* Add this entry to mcam rules list */
	rule = kzalloc(sizeof(*rule), GFP_KERNEL);
	if (!rule)
		return -ENOMEM;

	/* Disable rule by default. Enable rule when first dmac filter is
	 * installed
	 */
	rule->enable = false;
	rule->chan = chan_val;
	rule->chan_mask = chan_mask;
	rule->entry = mcam_idx;
	rvu_mcam_add_rule(mcam, rule);

	/* Reserve slot 0 */
	npc_mcam_rsrcs_reserve(rvu, blkaddr, mcam_idx);

	/* Allocate counter for this single drop on non hit rule */
	cntr_req.hdr.pcifunc = 0; /* AF request */
	cntr_req.contig = true;
	cntr_req.count = 1;
	err = rvu_mbox_handler_npc_mcam_alloc_counter(rvu, &cntr_req, &cntr_rsp);
	if (err) {
		dev_err(rvu->dev, "%s: Err to allocate cntr for drop rule (err=%d)\n",
			__func__, err);
		return	-EFAULT;
	}
	*counter_idx = cntr_rsp.cntr;

	/* Fill in fields for this mcam entry */
	npc_update_entry(rvu, NPC_EXACT_RESULT, &req.entry_data, exact_val, 0,
			 exact_mask, 0, NIX_INTF_RX);
	npc_update_entry(rvu, NPC_CHAN, &req.entry_data, chan_val, 0,
			 chan_mask, 0, NIX_INTF_RX);
	npc_update_entry(rvu, NPC_LXMB, &req.entry_data, bcast_mcast_val, 0,
			 bcast_mcast_mask, 0, NIX_INTF_RX);

	req.intf = NIX_INTF_RX;
	req.set_cntr = true;
	req.cntr = cntr_rsp.cntr;
	req.entry = mcam_idx;

	err = rvu_mbox_handler_npc_mcam_write_entry(rvu, &req, &rsp);
	if (err) {
		dev_err(rvu->dev, "%s: Installation of single drop on non hit rule at %d failed\n",
			__func__, mcam_idx);
		return err;
	}

	dev_err(rvu->dev, "%s: Installed single drop on non hit rule at %d, cntr=%d\n",
		__func__, mcam_idx, req.cntr);

	/* disable entry at Bank 0, index 0 */
	npc_enable_mcam_entry(rvu, mcam, blkaddr, mcam_idx, false);

	return 0;
}

int rvu_mbox_handler_npc_get_field_status(struct rvu *rvu,
					  struct npc_get_field_status_req *req,
					  struct npc_get_field_status_rsp *rsp)
{
	int blkaddr;

	blkaddr = rvu_get_blkaddr(rvu, BLKTYPE_NPC, 0);
	if (blkaddr < 0)
		return NPC_MCAM_INVALID_REQ;

	if (!is_npc_interface_valid(rvu, req->intf))
		return NPC_FLOW_INTF_INVALID;

	if (npc_check_field(rvu, blkaddr, req->field, req->intf))
		rsp->enable = 1;

	return 0;
}
