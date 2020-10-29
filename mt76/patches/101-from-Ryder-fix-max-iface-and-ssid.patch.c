Index: mt76-2020-09-23-b22977c2/mt7615/init.c
===================================================================
--- mt76-2020-09-23-b22977c2.orig/mt7615/init.c
+++ mt76-2020-09-23-b22977c2/mt7615/init.c
@@ -221,7 +221,7 @@ static const struct ieee80211_iface_combination if_comb_radar[] = {
 	{
 		.limits = if_limits,
 		.n_limits = ARRAY_SIZE(if_limits),
-		.max_interfaces = 4,
+		.max_interfaces = MT7615_MAX_INTERFACES,
 		.num_different_channels = 1,
 		.beacon_int_infra_match = true,
 		.radar_detect_widths = BIT(NL80211_CHAN_WIDTH_20_NOHT) |
@@ -237,7 +237,7 @@ static const struct ieee80211_iface_combination if_comb[] = {
 	{
 		.limits = if_limits,
 		.n_limits = ARRAY_SIZE(if_limits),
-		.max_interfaces = 4,
+		.max_interfaces = MT7615_MAX_INTERFACES,
 		.num_different_channels = 1,
 		.beacon_int_infra_match = true,
 	}

Index: mt76-2020-09-23-b22977c2/mt7615/main.c
--- mt76-2020-09-23-b22977c2.orig/mt7615/main.c
+++ mt76-2020-09-23-b22977c2/mt7615/main.c
@@ -115,29 +115,50 @@ static void mt7615_stop(struct ieee80211_hw *hw)
 	mt7615_mutex_release(dev);
 }
 
-static int get_omac_idx(enum nl80211_iftype type, u32 mask)
+static inline int get_free_idx(u32 mask, u8 start, u8 end)
+{
+	return ffs(~mask & GENMASK(end, start));
+}
+
+static int get_omac_idx(enum nl80211_iftype type, u64 mask)
 {
 	int i;
 
 	switch (type) {
-	case NL80211_IFTYPE_MONITOR:
-	case NL80211_IFTYPE_AP:
 	case NL80211_IFTYPE_MESH_POINT:
 	case NL80211_IFTYPE_ADHOC:
-		/* ap use hw bssid 0 and ext bssid */
+	case NL80211_IFTYPE_STATION:
+		/* prefer hw bssid slot 1-3 */
+		i = get_free_idx(mask, HW_BSSID_1, HW_BSSID_3);
+		if (i)
+			return i - 1;
+
+		if (type != NL80211_IFTYPE_STATION)
+			break;
+
+		/* next, try to find a free repeater entry for the sta */
+		i = get_free_idx(mask >> REPEATER_BSSID_START, 0,
+				 REPEATER_BSSID_MAX - REPEATER_BSSID_START);
+		if (i)
+			return i + 32 - 1;
+
+		i = get_free_idx(mask, EXT_BSSID_1, EXT_BSSID_MAX);
+		if (i)
+			return i - 1;
+
 		if (~mask & BIT(HW_BSSID_0))
 			return HW_BSSID_0;
 
-		for (i = EXT_BSSID_1; i < EXT_BSSID_END; i++)
-			if (~mask & BIT(i))
-				return i;
-
 		break;
-	case NL80211_IFTYPE_STATION:
-		/* sta use hw bssid other than 0 */
-		for (i = HW_BSSID_1; i < HW_BSSID_MAX; i++)
-			if (~mask & BIT(i))
-				return i;
+	case NL80211_IFTYPE_MONITOR:
+	case NL80211_IFTYPE_AP:
+		/* ap uses hw bssid 0 and ext bssid */
+		if (~mask & BIT(HW_BSSID_0))
+			return HW_BSSID_0;
+
+		i = get_free_idx(mask, EXT_BSSID_1, EXT_BSSID_MAX);
+		if (i)
+			return i - 1;
 
 		break;
 	default:
@@ -187,8 +208,8 @@ static int mt7615_add_interface(struct ieee80211_hw *hw,
 		mvif->wmm_idx = mvif->idx % MT7615_MAX_WMM_SETS;
 
 	dev->mphy.vif_mask |= BIT(mvif->idx);
-	dev->omac_mask |= BIT(mvif->omac_idx);
-	phy->omac_mask |= BIT(mvif->omac_idx);
+	dev->omac_mask |= BIT_ULL(mvif->omac_idx);
+	phy->omac_mask |= BIT_ULL(mvif->omac_idx);
 
 	mt7615_mcu_set_dbdc(dev);
 
@@ -257,8 +278,8 @@ static void mt7615_remove_interface(struct ieee80211_hw *hw,
 	rcu_assign_pointer(dev->mt76.wcid[idx], NULL);
 
 	dev->mphy.vif_mask &= ~BIT(mvif->idx);
-	dev->omac_mask &= ~BIT(mvif->omac_idx);
-	phy->omac_mask &= ~BIT(mvif->omac_idx);
+	dev->omac_mask &= ~BIT_ULL(mvif->omac_idx);
+	phy->omac_mask &= ~BIT_ULL(mvif->omac_idx);
 
 	mt7615_mutex_release(dev);
 
Index: mt76-2020-09-23-b22977c2/mt7615/mcu.c
--- mt76-2020-09-23-b22977c2.orig/mt7615/mcu.c
+++ mt76-2020-09-23-b22977c2/mt7615/mcu.c
@@ -599,6 +599,42 @@ static int mt7615_mcu_init_download(struct mt7615_dev *dev, u32 addr,
 				 &req, sizeof(req), true);
 }
 
+static int
+mt7615_mcu_muar_config(struct mt7615_dev *dev, struct ieee80211_vif *vif,
+		       bool bssid, bool enable)
+{
+	struct mt7615_vif *mvif = (struct mt7615_vif *)vif->drv_priv;
+	u32 idx = mvif->omac_idx - REPEATER_BSSID_START;
+	u32 mask = dev->omac_mask >> 32 & ~BIT(idx);
+	const u8 *addr = vif->addr;
+	struct {
+		u8 mode;
+		u8 force_clear;
+		u8 clear_bitmap[8];
+		u8 entry_count;
+		u8 write;
+
+		u8 index;
+		u8 bssid;
+		u8 addr[ETH_ALEN];
+	} __packed req = {
+		.mode = !!mask || enable,
+		.entry_count = 1,
+		.write = 1,
+
+		.index = idx * 2 + bssid,
+	};
+
+	if (bssid)
+		addr = vif->bss_conf.bssid;
+
+	if (enable)
+		ether_addr_copy(req.addr, addr);
+
+	return __mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD_MUAR_UPDATE, &req,
+				 sizeof(req), true);
+}
+
 static int
 mt7615_mcu_add_dev(struct mt7615_dev *dev, struct ieee80211_vif *vif,
 		   bool enable)
@@ -634,6 +670,9 @@ mt7615_mcu_add_dev(struct mt7615_dev *dev, struct ieee80211_vif *vif,
 		},
 	};
 
+	if (mvif->omac_idx >= REPEATER_BSSID_START)
+		return mt7615_mcu_muar_config(dev, vif, false, enable);
+
 	memcpy(data.tlv.omac_addr, vif->addr, ETH_ALEN);
 	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD_DEV_INFO_UPDATE,
 				 &data, sizeof(data), true);
@@ -1216,6 +1255,9 @@ mt7615_mcu_add_bss(struct mt7615_phy *phy, struct ieee80211_vif *vif,
 	struct mt7615_dev *dev = phy->dev;
 	struct sk_buff *skb;
 
+	if (mvif->omac_idx >= REPEATER_BSSID_START)
+		mt7615_mcu_muar_config(dev, vif, true, enable);
+
 	skb = mt7615_mcu_alloc_sta_req(dev, mvif, NULL);
 	if (IS_ERR(skb))
 		return PTR_ERR(skb);
@@ -1225,7 +1267,8 @@ mt7615_mcu_add_bss(struct mt7615_phy *phy, struct ieee80211_vif *vif,
 
 	mt7615_mcu_bss_basic_tlv(skb, vif, sta, enable);
 
-	if (enable && mvif->omac_idx > EXT_BSSID_START)
+	if (enable && mvif->omac_idx >= EXT_BSSID_START &&
+	    mvif->omac_idx < REPEATER_BSSID_START)
 		mt7615_mcu_bss_ext_tlv(skb, mvif);
 
 	return mt76_mcu_skb_send_msg(&dev->mt76, skb,
@@ -2603,13 +2646,13 @@ int mt7615_mcu_set_dbdc(struct mt7615_dev *dev)
 	} while (0)
 
 	for (i = 0; i < 4; i++) {
-		bool band = !!(ext_phy->omac_mask & BIT(i));
+		bool band = !!(ext_phy->omac_mask & BIT_ULL(i));
 
 		ADD_DBDC_ENTRY(DBDC_TYPE_BSS, i, band);
 	}
 
 	for (i = 0; i < 14; i++) {
-		bool band = !!(ext_phy->omac_mask & BIT(0x11 + i));
+		bool band = !!(ext_phy->omac_mask & BIT_ULL(0x11 + i));
 
 		ADD_DBDC_ENTRY(DBDC_TYPE_MBSS, i, band);
 	}
Index: mt76-2020-09-23-b22977c2/mt7615/mcu.h
--- mt76-2020-09-23-b22977c2.orig/mt7615/mcu.h
+++ mt76-2020-09-23-b22977c2/mt7615/mcu.h
@@ -275,6 +275,7 @@ enum {
 	MCU_EXT_CMD_PROTECT_CTRL = 0x3e,
 	MCU_EXT_CMD_DBDC_CTRL = 0x45,
 	MCU_EXT_CMD_MAC_INIT_CTRL = 0x46,
+	MCU_EXT_CMD_MUAR_UPDATE = 0x48,
 	MCU_EXT_CMD_BCN_OFFLOAD = 0x49,
 	MCU_EXT_CMD_SET_RX_PATH = 0x4e,
 	MCU_EXT_CMD_TX_POWER_FEATURE_CTRL = 0x58,
Index: mt76-2020-09-23-b22977c2/mt7615/mt7615.h
--- mt76-2020-09-23-b22977c2.orig/mt7615/mt7615.h
+++ mt76-2020-09-23-b22977c2/mt7615/mt7615.h
@@ -11,7 +11,7 @@
 #include "../mt76.h"
 #include "regs.h"
 
-#define MT7615_MAX_INTERFACES		4
+#define MT7615_MAX_INTERFACES		16
 #define MT7615_MAX_WMM_SETS		4
 #define MT7663_WTBL_SIZE		32
 #define MT7615_WTBL_SIZE		128
@@ -176,7 +176,7 @@ struct mt7615_phy {
 	struct ieee80211_vif *monitor_vif;
 
 	u32 rxfilter;
-	u32 omac_mask;
+	u64 omac_mask;
 
 	u16 noise;
 
@@ -346,24 +346,13 @@ enum {
 	HW_BSSID_1,
 	HW_BSSID_2,
 	HW_BSSID_3,
-	HW_BSSID_MAX,
+	HW_BSSID_MAX = HW_BSSID_3,
 	EXT_BSSID_START = 0x10,
 	EXT_BSSID_1,
-	EXT_BSSID_2,
-	EXT_BSSID_3,
-	EXT_BSSID_4,
-	EXT_BSSID_5,
-	EXT_BSSID_6,
-	EXT_BSSID_7,
-	EXT_BSSID_8,
-	EXT_BSSID_9,
-	EXT_BSSID_10,
-	EXT_BSSID_11,
-	EXT_BSSID_12,
-	EXT_BSSID_13,
-	EXT_BSSID_14,
-	EXT_BSSID_15,
-	EXT_BSSID_END
+	EXT_BSSID_15 = 0x1f,
+	EXT_BSSID_MAX = EXT_BSSID_15,
+	REPEATER_BSSID_START = 0x20,
+	REPEATER_BSSID_MAX = 0x3f,
 };
 
 enum {
-- 
2.18.0

