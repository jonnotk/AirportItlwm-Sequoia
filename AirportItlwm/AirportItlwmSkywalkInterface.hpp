//
//  AirportItlwmSkywalkInterface.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef AirportItlwmSkywalkInterface_hpp
#define AirportItlwmSkywalkInterface_hpp

#include <Airport/Apple80211.h>
#include <Airport/IOSkywalkPacketQueues.h>
#if __IO80211_TARGET >= __MAC_15_0
#include "AiritlNativeWcl.h"
#include "AiritlNativeConfig.h"
#include "AiritlNativeBss.h"
#include "AiritlNativeRoam.h"
#include <HAL/ItlNativeScanProfile.hpp>
#endif
#include "AirportItlwmPacketTransport.hpp"

class IOMapper;
class IO80211WorkQueue;
class AirportItlwmPacketOperations;

// Legacy request helpers are ordinary methods on Sequoia, outside its SPI table.
#if __IO80211_TARGET >= __MAC_15_0
#define AIRITL_LEGACY_HANDLER
#define AIRITL_LEGACY_HANDLER_OVERRIDE
#else
#define AIRITL_LEGACY_HANDLER virtual
#define AIRITL_LEGACY_HANDLER_OVERRIDE override
#endif

class AirportItlwmSkywalkInterface : public IO80211InfraProtocol {
    OSDeclareDefaultStructors(AirportItlwmSkywalkInterface)

public:
#if __IO80211_TARGET >= __MAC_15_0
    bool init(IOService *); // Convenience helper; the native virtual takes a MAC pointer too.
#else
    virtual bool init(IOService *) override;
#endif
    virtual bool start(IOService *) override;
    virtual void stop(IOService *) override;
    virtual void free() override;
    // The controller calls this under its gate after observing provider loss.
    bool controllerStopComplete() const;
    bool controllerCleanupReady() const;
    bool controllerFunctionalReady() const;
    bool datapathTeardownComplete() const;

    // Copied packet transport. The optional mapper belongs only to the disabled
    // DMA diagnostic subclass; the production bridge uses native CPU buffers.
    bool provisionDatapath(IOMapper *mapper);
    bool teardownDatapath(void);
    // Bind queues after native start, then let the framework create its link.
    IOReturn registerDatapath(IOSkywalkEthernetInterface::RegistrationInfo *reginfo);
#if __IO80211_TARGET >= __MAC_15_0
    bool nativeTransportAvailable() const;
    IOReturn publishNativeRegulatoryUpdate();
    static void inputNativePacket(void *context, mbuf_t packet);
    static void outputNativeReady(void *context);
    UInt32 submitNativePackets(IOSkywalkPacket *const *, UInt32);
    UInt32 completeNativeTx(IOSkywalkPacket **, UInt32);
    void logTransportGuards(const char *where);
    UInt32 completeNativeRx(IOSkywalkPacket **, UInt32);
    UInt32 nativeTxFreeSpace();
#endif
    // Return the concrete objects owned by this interface.
    virtual void *getTxPacketPool(void) override;
    virtual void *getRxPacketPool(void) override;
    virtual void *getTxCompQueue(void) override;
    virtual void *getRxCompQueue(void) override;
    virtual void *getTxSubQueue(apple80211_wme_ac) override;
    virtual void *getMultiCastQueue(void) override;
    virtual int   getNumTxQueues(void) override;
    virtual UInt64 getTxQueueDepth(void) override;
    virtual UInt64 getRxQueueCapacity(void) override;
    virtual int   getTxHeadroom(void) override;
#if __IO80211_TARGET >= __MAC_15_0
    virtual UInt getHardwareAssists(void) override;
    virtual IOReturn enableDatapath(void) override;
    virtual IOReturn disableDatapath(void) override;
    // IOSkywalkNetworkBSDClient::enableInterface/disableInterface call these.
    // Apple's AppleBCMWLANSkywalkInterface::enable(UInt) calls enableDatapath
    // here; the IO80211 base does not, so without them the queues stay closed.
    virtual IOReturn enable(UInt) override;
    virtual IOReturn disable(UInt) override;
#else
    virtual void enableDatapath(void) override;
    virtual void disableDatapath(void) override;
#endif
//    virtual ifnet_t getBSDInterface(void) override;
#if __IO80211_TARGET >= __MAC_15_0
    void setEnabledBySystem(bool);    // non-virtual: not an Apple vtable slot
    bool enabledBySystem(void);
    bool willRoam(ether_addr *, UInt);
    void setPoweredOnByUser(bool);     // non-virtual: not an Apple vtable slot
    void setCurrentBssid(ether_addr *); // non-virtual: not an Apple vtable slot
    void *getCurrentBssid(void);          // non-virtual: not an Apple vtable slot
#endif
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setMacAddress(ether_addr &) override;
    void setSelfMacAddr(ether_addr *);
#else
    virtual void setSelfMacAddr(ether_addr *) override;
#endif
    
    IOReturn associateSSID(uint8_t *ssid, uint32_t ssid_len, const struct ether_addr &bssid, uint32_t authtype_lower, uint32_t authtype_upper, uint8_t *key, uint32_t key_len, int key_index);
    IOReturn setPTK(const u_int8_t *key, size_t key_len);
    IOReturn setGTK(const u_int8_t *key, size_t key_len, u_int8_t kid, u_int8_t *rsc);
    
public:
    AIRITL_LEGACY_HANDLER IOReturn getSSID(apple80211_ssid_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getAUTH_TYPE(apple80211_authtype_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    virtual IOReturn getCHANNEL(apple80211_channel_data *) override;
    virtual IOReturn getPOWERSAVE(apple80211_powersave_data *) override;
    virtual IOReturn getTXPOWER(apple80211_txpower_data *) override;
    virtual IOReturn getRATE(apple80211_rate_data *) override;
    AIRITL_LEGACY_HANDLER IOReturn getBSSID(apple80211_bssid_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getSCAN_RESULT(apple80211_scan_result *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getSTATE(apple80211_state_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getPHY_MODE(apple80211_phymode_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    virtual IOReturn getOP_MODE(apple80211_opmode_data *) override;
    virtual IOReturn getRSSI(apple80211_rssi_data *) override;
    AIRITL_LEGACY_HANDLER IOReturn getNOISE(apple80211_noise_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    virtual IOReturn getSUPPORTED_CHANNELS(apple80211_sup_channel_data *) override;
    AIRITL_LEGACY_HANDLER IOReturn getLOCALE(apple80211_locale_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getDEAUTH(apple80211_deauth_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getRATE_SET(apple80211_rate_set_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getDTIM_INT(apple80211_dtim_int_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getSTATION_LIST(apple80211_sta_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getRSN_IE(apple80211_rsn_ie_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getAP_IE_LIST(apple80211_ap_ie_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getSTATS(apple80211_stats_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getASSOCIATION_STATUS(apple80211_assoc_status_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    virtual IOReturn getGUARD_INTERVAL(apple80211_guard_interval_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getMCS(apple80211_mcs_data *) override;
    AIRITL_LEGACY_HANDLER IOReturn getMCS_INDEX_SET(apple80211_mcs_index_set_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getWOW_PARAMETERS(apple80211_wow_parameter_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getWOW_ENABLED(apple80211_state_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getPID_LOCK(apple80211_state_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getSTA_IE_LIST(apple80211_sta_ie_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getSTA_STATS(apple80211_sta_stats_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getBT_COEX_FLAGS(apple80211_state_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getCURRENT_NETWORK(apple80211_scan_result *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getRSSI_BOUNDS(apple80211_rssi_bounds_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getPOWER_DEBUG_INFO(apple80211_power_debug_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getHT_CAPABILITY(apple80211_ht_capability *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getLINK_CHANGED_EVENT_DATA(apple80211_link_changed_event_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getEXTENDED_STATS(apple80211_extended_stats *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getBEACON_PERIOD(apple80211_beacon_period_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getVHT_MCS_INDEX_SET(apple80211_vht_mcs_index_set_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    virtual IOReturn getMCS_VHT(apple80211_mcs_vht_data *) override;
    AIRITL_LEGACY_HANDLER IOReturn getGAS_RESULTS(apple80211_gas_result_t *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getCHANNELS_INFO(apple80211_channels_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getVHT_CAPABILITY(apple80211_vht_capability *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getBGSCAN_CACHE_RESULTS(apple80211_bgscan_cached_network_data_list *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getROAM_PROFILE(apple80211_roam_profile_band_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getCHIP_COUNTER_STATS(apple80211_chip_stats *) override { return kIOReturnUnsupported; }
    virtual IOReturn getDBG_GUARD_TIME_PARAMS(apple80211_dbg_guard_time_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn getLEAKY_AP_STATS_MODE(apple80211_leaky_ap_setting *) override { return kIOReturnUnsupported; }
    virtual IOReturn getCOUNTRY_CHANNELS(apple80211_country_channel_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getPRIVATE_MAC(apple80211_private_mac_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getRANGING_ENABLE(apple80211_ranging_enable_request_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getRANGING_START(apple80211_ranging_start_request_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getAWDL_RSDB_CAPS(apple80211_rsdb_capability *) override { return kIOReturnUnsupported; }
    virtual IOReturn getTKO_PARAMS(apple80211_tko_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn getTKO_DUMP(apple80211_tko_dump *) override { return kIOReturnUnsupported; }
    virtual IOReturn getHW_SUPPORTED_CHANNELS(apple80211_sup_channel_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getBTCOEX_PROFILE(apple80211_btcoex_profile *) override { return kIOReturnUnsupported; }
    virtual IOReturn getBTCOEX_PROFILE_ACTIVE(apple80211_btcoex_profile_active_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getTRAP_INFO(apple80211_trap_info_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getTHERMAL_INDEX(apple80211_thermal_index_t *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn getMAX_NSS_FOR_AP(apple80211_btcoex_max_nss_for_ap_data *) override;
#else
    virtual IOReturn getMAX_NSS_FOR_AP(apple80211_btcoex_max_nss_for_ap_data *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getBTCOEX_2G_CHAIN_DISABLE(apple80211_btcoex_2g_chain_disable *) override { return kIOReturnUnsupported; }
    virtual IOReturn getPOWER_BUDGET(apple80211_power_budget_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getOFFLOAD_TCPKA_ENABLE(apple80211_offload_tcpka_enable_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getRANGING_CAPS(apple80211_ranging_capabilities_t *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getSUPPRESS_SCANS(apple80211_suppress_scans_t *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getHOST_AP_MODE_HIDDEN(apple80211_host_ap_mode_hidden_t *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getLQM_CONFIG(apple80211_lqm_config_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn getTRAP_CRASHTRACER_MINI_DUMP(apple80211_trap_mini_dump_data *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getHE_CAPABILITY(apple80211_he_capability *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn getBEACON_INFO(apple80211_beacon_info_t *) override;
#else
    virtual IOReturn getBEACON_INFO(apple80211_beacon_info_t *) override { return kIOReturnUnsupported; }
#endif
    AIRITL_LEGACY_HANDLER IOReturn getSOFTAP_PARAMS(apple80211_softap_params *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getCHIP_POWER_RANGE(apple80211_chip_power_limit *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getSOFTAP_STATS(apple80211_softap_stats *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getNSS(apple80211_nss_data *) override;
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn getHW_ADDR(apple80211_hw_mac_address *) override;
#else
    virtual IOReturn getHW_ADDR(apple80211_hw_mac_address *) override { return kIOReturnUnsupported; }
#endif
    AIRITL_LEGACY_HANDLER IOReturn getHE_MCS_INDEX_SET(apple80211_he_mcs_index_set_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getCHIP_DIAGS(appl80211_chip_diags_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getHP2P_CTRL(apple80211_hp2p_ctrl *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getREQUEST_BSS_BLACKLIST(void *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getASSOC_READY_STATUS(apple80211_assoc_ready *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getTXRX_CHAIN_INFO(apple80211_txrx_chain_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getMIMO_STATUS(apple80211_mimo_status *) override { return kIOReturnUnsupported; }
    virtual IOReturn getCUR_PMK(apple80211_pmk *) override { return kIOReturnUnsupported; }
    virtual IOReturn getDYNSAR_DETAIL(apple80211_dynsar_detail *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getRANDOMISATION_STATUS(apple80211_mac_randomisation_status *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getCOUNTRY_CHANNELS_INFO(apple80211_channels_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getLQM_SUMMARY(apple80211_lqm_summary *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getCOLOCATED_NETWORK_SCOPE_ID(apple80211_colocated_network_scope_id *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn getBEACON_SCAN_CACHE_REQ(apple80211_scan_result *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getSLOW_WIFI_FEATURE_ENABLED(apple80211_slow_wifi_feature_enabled *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getCCA(apple80211_interface_cca_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getRX_RATE(apple80211_rate_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getTIMESYNC_INFO(apple80211_timesync_info *) override { return kIOReturnUnsupported; }
    virtual IOReturn getSENSING_DATA(apple80211_sensing_data_t *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getCOUNTRY_BAND_SUPPORT(apple80211_country_band_support *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn getWCL_FW_HOT_CHANNELS(apple80211_fw_hot_channels *) override;
#else
    virtual IOReturn getWCL_FW_HOT_CHANNELS(apple80211_fw_hot_channels *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info *) override;
#else
    virtual IOReturn getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn getWCL_BSS_INFO(apple80211_beacon_msg *) override;
#else
    virtual IOReturn getWCL_BSS_INFO(apple80211_beacon_msg *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters *) override;
#else
    virtual IOReturn getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getWCL_GET_TX_BLANKING_STATUS(uint *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getSSID_TRANSITION_SUPPORT(apple80211_ssid_transition_feature_enabled *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getWCL_VALID_CHANNEL_COUNT(unsigned long *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getWCL_P2P_STATUS_FOR_SCAN(p2pStatusForScan *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn getWCL_CHANNELS_INFO(apple80211ChannelInfo *) override;
#else
    virtual IOReturn getWCL_CHANNELS_INFO(apple80211ChannelInfo *) override { return kIOReturnUnsupported; }
#endif
    AIRITL_LEGACY_HANDLER IOReturn getP2P_STEERING_METRIC(apple80211_p2p_steering_metrics *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn getRSN_XE(apple80211_rsn_xe_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn getSIB_COEX_STATUS(apple80211_sib_coex_status *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *) override;
#else
    virtual IOReturn getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn getWCL_LOW_LATENCY_INFO_STATS(apple80211_wcl_low_latency_stats *) override { return kIOReturnUnsupported; }
    virtual IOReturn getWCL_BGSCAN_CACHE_RESULT(apple80211_bgscan_cached_network_data_list *) override { return kIOReturnUnsupported; }
    virtual IOReturn getWIFI_NOISE_PER_ANT(apple80211_noise_per_ant_t *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn getBLOCKED_BANDS(apple80211_blocked_bands *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSSID(apple80211_ssid_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setAUTH_TYPE(apple80211_authtype_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    virtual IOReturn setCIPHER_KEY(apple80211_key *) override;
    virtual IOReturn setCHANNEL(apple80211_channel_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setPOWERSAVE(apple80211_powersave_data *) override;
#else
    virtual IOReturn setPOWERSAVE(apple80211_powersave_data *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setTXPOWER(apple80211_txpower_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setRATE(apple80211_rate_data *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSCAN_REQ(apple80211_scan_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    IOReturn setSCAN_REQ_MULTIPLE(apple80211_scan_multiple_data *);
    AIRITL_LEGACY_HANDLER IOReturn setASSOCIATE(apple80211_assoc_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn setDISASSOCIATE(apple80211_disassoc_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    virtual IOReturn setIBSS_MODE(apple80211_network_data *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setHOST_AP_MODE(apple80211_network_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setAP_MODE(apple80211_apmode_data *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setDEAUTH(apple80211_deauth_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn setTX_ANTENNA(void *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setANTENNA_DIVERSITY(void *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setRSN_IE(apple80211_rsn_ie_data *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    AIRITL_LEGACY_HANDLER IOReturn setBACKGROUND_SCAN(apple80211_bgscan_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setWOW_PARAMETERS(apple80211_wow_parameter_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setWOW_ENABLED(apple80211_state_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setPID_LOCK(apple80211_state_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSTA_AUTHORIZE(apple80211_sta_authorize_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSTA_DISASSOCIATE(apple80211_sta_disassoc_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSTA_DEAUTH(apple80211_sta_disassoc_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setRSN_CONF(apple80211_rsn_conf_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setIE(apple80211_ie_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWOW_TEST(apple80211_wow_test_data *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSCANCACHE_CLEAR(void *) AIRITL_LEGACY_HANDLER_OVERRIDE;
    virtual IOReturn setVIRTUAL_IF_CREATE(apple80211_virt_if_create_data *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setBT_COEX_FLAGS(apple80211_state_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setROAM(apple80211_sta_roam_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setHT_CAPABILITY(apple80211_ht_capability *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setAWDL_FORCED_ROAM_CONFIG(apple80211_awdl_forced_roam_config *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setOFFLOAD_ARP(apple80211_offload_arp_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setOFFLOAD_NDP(apple80211_offload_ndp_data *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setOFFLOAD_SCAN(apple80211_offload_scan_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setGAS_REQ(apple80211_gas_query_t *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setGAS_START(apple80211_gas_query_t *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setGAS_SET_PEER(apple80211_gas_peer_t *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setVHT_CAPABILITY(apple80211_vht_capability *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setROAM_PROFILE(apple80211_roam_profile_band_data *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setAWDL_ENABLE_ROAMING(void *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setDBG_GUARD_TIME_PARAMS(apple80211_dbg_guard_time_params *) override { return kIOReturnUnsupported; }
    virtual IOReturn setLEAKY_AP_STATS_MODE(apple80211_leaky_ap_setting *) override { return kIOReturnUnsupported; }
    virtual IOReturn setPRIVATE_MAC(apple80211_private_mac_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setRESET_CHIP(apple80211_reset_command *) override { return kIOReturnUnsupported; }
    virtual IOReturn setCRASH(apple80211_crash_command *) override { return kIOReturnUnsupported; }
    virtual IOReturn setRANGING_ENABLE(apple80211_ranging_enable_request_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setRANGING_START(apple80211_ranging_start_request_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setRANGING_AUTHENTICATE(apple80211_ranging_authenticate_request_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setTKO_PARAMS(apple80211_tko_params *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setBTCOEX_PROFILE(apple80211_btcoex_profile *) override;
#else
    virtual IOReturn setBTCOEX_PROFILE(apple80211_btcoex_profile *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setBTCOEX_PROFILE_ACTIVE(apple80211_btcoex_profile_active_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setTHERMAL_INDEX(apple80211_thermal_index_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setBTCOEX_2G_CHAIN_DISABLE(apple80211_btcoex_2g_chain_disable *) override { return kIOReturnUnsupported; }
    virtual IOReturn setPOWER_BUDGET(apple80211_power_budget_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setOFFLOAD_TCPKA_ENABLE(apple80211_offload_tcpka_enable_t *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSUPPRESS_SCANS(apple80211_suppress_scans_t *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setHOST_AP_MODE_HIDDEN(apple80211_host_ap_mode_hidden_t *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setLQM_CONFIG(apple80211_lqm_config_t *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSOFTAP_PARAMS(apple80211_softap_params *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSOFTAP_TRIGGER_CSA(apple80211_softap_csa_params *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSOFTAP_WIFI_NETWORK_INFO_IE(apple80211_softap_wifi_network_info *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setBTCOEX_DISABLE_ULOFDMA(uint *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSCAN_CONTROL(apple80211_scan_control_params *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setUSB_HOST_NOTIFICATION(apple80211_usb_host_notification_data *) override;
#else
    virtual IOReturn setUSB_HOST_NOTIFICATION(apple80211_usb_host_notification_data *) override { return kIOReturnUnsupported; }
#endif
    AIRITL_LEGACY_HANDLER IOReturn setSET_MAC_ADDRESS(apple80211_set_mac_address *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setHP2P_CTRL(apple80211_hp2p_ctrl *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setABORT_SCAN(apple80211_abort_scan *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setSET_PROPERTY(apple80211_set_property_unserialized_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setROAM_CACHE_UPDATE(apple80211_roam_cache_data *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setPM_MODE(apple80211_pm_mode *) override;
#else
    virtual IOReturn setPM_MODE(apple80211_pm_mode *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setSET_WIFI_ASSERTION_STATE(apple80211_wifi_assertion_data *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setREASSOCIATE_WITH_CORECAPTURE(apple80211_capture_debug_info_t *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setLINKDOWN_DEBOUNCE_STATUS(apple80211_linkdown_debounce_status *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setSOFTAP_EXTENDED_CAPABILITIES_IE(apple80211_softap_extended_capabilities_info *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setREALTIME_QOS_MSCS(apple80211_state_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setSENSING_ENABLE(apple80211_sensing_enable_t *) override { return kIOReturnUnsupported; }
    virtual IOReturn setSENSING_DISABLE(apple80211_sensing_disable_t *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setNANPHS_ASSOCIATION(apple80211_nan_link_association_info *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setNANPHS_TERMINATED(apple80211_nan_link_association_info *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn set6G_MODE(apple80211_6G_mode *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_LEAVE_NETWORK(apple80211_leave_network *) override
#if __IO80211_TARGET >= __MAC_15_0
    ;
#else
    { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setWCL_REASSOC(apple80211_reassoc *) override;
#else
    virtual IOReturn setWCL_REASSOC(apple80211_reassoc *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setWCL_SET_ROAM_LOCK(apple80211_set_roam_lock *) override;
#else
    virtual IOReturn setWCL_SET_ROAM_LOCK(apple80211_set_roam_lock *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setWCL_ROAM_PROFILE_CONFIG(apple80211_roam_profile_config *) override;
#else
    virtual IOReturn setWCL_ROAM_PROFILE_CONFIG(apple80211_roam_profile_config *) override { return kIOReturnUnsupported; }
#endif
    AIRITL_LEGACY_HANDLER IOReturn setWCL_ROAM_PROFILE_CONFIGV1(apple80211_roam_profile_configV1 *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_ROAM_USER_CACHE(apple80211_user_roam_cache *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setWCL_SET_MULTI_AP_ENV(apple80211_set_multi_ap_env *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_SCAN_ABORT(void *) override
#if __IO80211_TARGET >= __MAC_15_0
    ;
#else
    { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setWCL_REAL_TIME_MODE(apple80211_wcl_real_time_mode *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setWCL_GARP_MODE(apple80211_wcl_garp_mode *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_JOIN_ABORT(void *) override
#if __IO80211_TARGET >= __MAC_15_0
    ;
#else
    { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setWCL_TRIGGER_CC(triggerCC *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_SCAN_REQ(apple80211ScanRequest *) override
#if __IO80211_TARGET >= __MAC_15_0
    ;
#else
    { return kIOReturnUnsupported; }
#endif
    AIRITL_LEGACY_HANDLER IOReturn setWCL_ASSOCIATE(apple80211_assoc_candidates *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setWCL_PROTECT_IP(apple80211_wcl_protect_ip_mode *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setWCL_LINK_UP_DONE(void *) override;
#else
    virtual IOReturn setWCL_LINK_UP_DONE(void *) override { return kIOReturnUnsupported; }
#endif
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setWCL_SET_SCAN_HOME_AWAY_TIME(scanHomeAndAwayTime *) override;
#else
    virtual IOReturn setWCL_SET_SCAN_HOME_AWAY_TIME(scanHomeAndAwayTime *) override { return kIOReturnUnsupported; }
#endif
    AIRITL_LEGACY_HANDLER IOReturn setWCL_VOLUNTARY_NETWORK_DISCONNECT(apple80211_wcl_voluntary_network_disconnect *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_LINK_STATE_UPDATE(apple80211_wcl_update_link_state *) override
#if __IO80211_TARGET >= __MAC_15_0
    ;
#else
    { return kIOReturnUnsupported; }
#endif
    AIRITL_LEGACY_HANDLER IOReturn setSLOW_WIFI_RECOVERY(void *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setRSN_XE(apple80211_rsn_xe_data *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_ULOFDMA_STATE(apple80211_wcl_ulofdma_state *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_ACTION_FRAME(apple80211_wcl_action_frame *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setWCL_REAL_TIME_POLICY(apple80211_wcl_real_time_policy *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setGAS_ABORT(void *) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setOS_FEATURE_FLAGS(apple80211_feature_flags *) override;
#else
    virtual IOReturn setOS_FEATURE_FLAGS(apple80211_feature_flags *) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setDHCP_RENEWAL_DATA(apple80211_dhcp_renewal_data *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setMOVING_NETWORK(apple80211_network_flags *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
    virtual IOReturn setBATTERY_POWERSAVE_CONFIG(apple80211_battery_ps_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setMIMO_CONFIG(apple80211_mimo_config *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_CONFIG_BG_MOTIONPROFILE(apple80211_bg_motion_profile *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_CONFIG_BG_NETWORK(apple80211_bg_network *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_CONFIG_BGSCAN(apple80211_bg_scan *) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_CONFIG_BG_PARAMS(apple80211_bg_params *) override { return kIOReturnUnsupported; }
    AIRITL_LEGACY_HANDLER IOReturn setBLOCKED_BANDS(apple80211_blocked_bands *) AIRITL_LEGACY_HANDLER_OVERRIDE { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    // Exact typed entries for Sequoia requests whose payloads are not implemented.
    virtual IOReturn getROAM_PROFILE(apple80211_roam_profile_all_bands*) override { return kIOReturnUnsupported; }
    virtual IOReturn getBSS_BLACKLIST(bss_blacklist*) override { return kIOReturnUnsupported; }
    virtual IOReturn getHE_COUNTERS(apple80211_he_counters_ctl*) override { return kIOReturnUnsupported; }
    virtual IOReturn getWIFI_BT_5G_POLICY(apple80211_wifi_bt_5g_policy_t*) override { return kIOReturnUnsupported; }
    virtual IOReturn getWCL_WNM_OFFLOAD(apple80211_wcl_wnm_offload_t*) override { return kIOReturnUnsupported; }
    virtual IOReturn getFW_CLOCK_INFO(apple80211_fw_clock_info*) override { return kIOReturnUnsupported; }
    virtual IOReturn getTIMESYNC_STATS(apple80211_timesync_stats*) override { return kIOReturnUnsupported; }
    virtual IOReturn getSYSTEM_SLEEP_CONFIG(apple80211_system_sleep_config*) override { return kIOReturnUnsupported; }
    virtual IOReturn setCLEAR_PMKSA_CACHE(void*) override { return kIOReturnUnsupported; }
    virtual IOReturn setROAM_PROFILE(apple80211_roam_profile_all_bands*) override { return kIOReturnUnsupported; }
    virtual IOReturn setDYNAMIC_RSSI_WINDOW_CONFIG(apple80211_dynamic_rssi_window_config*) override { return kIOReturnUnsupported; }
    virtual IOReturn setBSS_BLACKLIST(bss_blacklist*) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_LEGACY_ROAM_PROFILE_CONFIG(apple80211_legacy_roam_profile_config*) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_ARP_MODE(apple80211_wcl_arp_mode*) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_ASSOCIATE(apple80211AssocCandidates*) override;
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setWCL_QOS_PARAMS(apple80211_wcl_qos_params*) override;
#else
    virtual IOReturn setWCL_QOS_PARAMS(apple80211_wcl_qos_params*) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setVOICE_IND_STATE(apple80211_voice_ind_state*) override { return kIOReturnUnsupported; }
    virtual IOReturn setWIFI_BT_5G_POLICY(apple80211_wifi_bt_5g_policy_t*) override { return kIOReturnUnsupported; }
    virtual IOReturn setPOWER_PROFILE(apple80211_power_profile*) override { return kIOReturnUnsupported; }
    virtual IOReturn setHEARTBEAT(void*) override { return kIOReturnUnsupported; }
    virtual IOReturn setINTERFACE_SETTING(apple80211_interface_setting*) override { return kIOReturnUnsupported; }
    virtual IOReturn setBYPASS_TX_POWER_CAP(apple80211_bypass_tx_power_cap*) override { return kIOReturnUnsupported; }
    virtual IOReturn setFACETIME_WIFICALLING_PARAMS(apple80211_facetime_wificalling_params*) override { return kIOReturnUnsupported; }
    virtual IOReturn setIPV4_PARAMS(apple80211_ipv4_params*) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_WNM_OPS(apple80211_wcl_wnm_config_t*) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_WNM_OFFLOAD(apple80211_wcl_wnm_offload_t*) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_LIMITED_AGGREGATION(apple80211_limited_aggregation_config*) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_BCN_MUTE_CONFIG(apple80211_bcn_mute_config*) override { return kIOReturnUnsupported; }
    virtual IOReturn setEAP_FILTER_CONFIG(apple80211_eap_filter_config*) override { return kIOReturnUnsupported; }
    virtual IOReturn setWOW_LOW_POWER_MODE(apple80211_wow_low_power_mode*) override { return kIOReturnUnsupported; }
    virtual IOReturn setDUAL_POWER_MODE(apple80211_dual_power_mode_params*) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setWCL_UPDATE_FAST_LANE(apple80211_fastlane*) override;
#else
    virtual IOReturn setWCL_UPDATE_FAST_LANE(apple80211_fastlane*) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setWCL_ASSOCIATED_SLEEP(apple80211_associated_sleep_config*) override { return kIOReturnUnsupported; }
    virtual IOReturn setCONGESTION_CTRL_IND(apple80211_congestion_control_indication*) override { return kIOReturnUnsupported; }
    virtual IOReturn setSTAND_ALONE_MODE_STATE(apple80211_standalone_state*) override { return kIOReturnUnsupported; }
    virtual IOReturn setIPV6_PARAMS(apple80211_ipv6_params*) override { return kIOReturnUnsupported; }
#if __IO80211_TARGET >= __MAC_15_0
    virtual IOReturn setINFRA_ENUMERATED(apple80211_infra_enumerated*) override;
#else
    virtual IOReturn setINFRA_ENUMERATED(apple80211_infra_enumerated*) override { return kIOReturnUnsupported; }
#endif
    virtual IOReturn setLMTPC_CONFIG(apple80211_lmtpc_config*) override { return kIOReturnUnsupported; }
    virtual IOReturn setTRAFFIC_ENG_PARAMS(apple80211_traffic_eng_params*) override { return kIOReturnUnsupported; }
    virtual IOReturn setLE_SCAN_PARAM(apple80211_le_scan_params*) override { return kIOReturnUnsupported; }
    virtual IOReturn setTIMESYNC_GPIO(apple80211_timesync_gpio*) override { return kIOReturnUnsupported; }
    virtual IOReturn setHOST_CLOCK_INFO(apple80211_host_clock_info*) override { return kIOReturnUnsupported; }
    virtual IOReturn setFW_CLOCK_SOURCE(apple80211_fw_clock_source*) override { return kIOReturnUnsupported; }
    virtual IOReturn setTIMESYNC_TX_POLICY(apple80211_timesync_tx_policy*) override { return kIOReturnUnsupported; }
    virtual IOReturn setTIMESYNC_RX_POLICY(apple80211_timesync_rx_policy*) override { return kIOReturnUnsupported; }
    virtual IOReturn setTIMESTAMPING_EN(apple80211_timestamping_en*) override { return kIOReturnUnsupported; }
    virtual IOReturn setWCL_SOI_CONFIG(appl80211_sleep_on_inactivity_config*) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_TIME_SHARING_WIFI_ENH(apple80211_mws_time_sharing*) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_WIFI_TYPE_7_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_COEX_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_DISABLE_OCL_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_RFEM_CONFIG_WIFI_ENH(apple80211_mws_rfem_config*) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_ASSOC_PROTECTION_BITMAP_WIFI_ENH(apple80211_mws_wifi_channel_bitmap*) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_SCAN_FREQ_WIFI_ENH(apple80211_mws_scan_freq*) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_SCAN_FREQ_MODE_WIFI_ENH(apple80211_mws_scan_freq_mode*) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_CONDITION_ID_BITMAP_WIFI_ENH(apple80211_mws_condition_id_config*) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_ANTENNA_SELECTION_WIFI_ENH(apple80211_mws_antenna_selection*) override { return kIOReturnUnsupported; }
    virtual IOReturn setNDD_REQ(apple80211_ndd_data*) override { return kIOReturnUnsupported; }
    virtual IOReturn setDBRG_ENTROPY(apple80211_drbg_entropy*) override { return kIOReturnUnsupported; }
    virtual IOReturn setSDB_ENABLE(apple80211_sdb_enable*) override { return kIOReturnUnsupported; }
    virtual IOReturn setMWS_ACCESSORY_POWER_LIMIT_WIFI_ENH(apple80211_mws_accessory_power_limit*) override { return kIOReturnUnsupported; }
#endif

#if __IO80211_TARGET >= __MAC_15_0
    bool nativeControlReady() const;
#ifndef AIRITL_ENABLE_BSDIF
    bool nativeLinkDownReady() const;
#endif
    void setNativeControlObserverReady(bool ready) { fNativeObserverReady = ready; }
    uint64_t nativeScanGeneration() const { return fNativeScanState.generation; }
    uint64_t nativeJoinGeneration() const { return fNativeJoinState.generation; }
    void handleHalScanComplete(int status, uint64_t generation);
    void handleHalJoinEvent(int event, int ieeeStatus, uint64_t generation);
    void cancelNativeRequests(IOReturn reason);
    void nativeHalQuiesced();
    void nativeResetBegin();
    void nativeResetFinished(bool ready) { fNativeResetPending = false; fNativeObserverReady = ready; }
    bool publishNativeBeacon(const uint8_t *, size_t, const AiritlWcl::BeaconInput &);
    void pollNativeJoin();
    void pollNativeScan();
#endif
    void detachControllerReferences();
    void completeScanRequest(bool cacheOnly = false);
    
private:
    friend class AirportItlwm;
    friend class AirportItlwmPacketOperations;
    bool finishUnstartedCleanup();
    bool finishControllerCleanup(AirportItlwm *, IOCommandGate *, IOWorkLoop *, bool);
    AirportItlwm *instance;
    ItlHalService *fHalService;

#if __IO80211_TARGET >= __MAC_15_0
    apple80211ScanRequest *fNativeScan;
    apple80211AssocCandidates *fNativeJoin;
    AiritlWcl::ScanRequestState fNativeScanState;
    AiritlWcl::RequestState fNativeJoinState;
    ItlNativeScanProfile fNativeScanProfile;
    bool fNativeAssocReported, fNativeFirstBeaconReported, fNativeConnectReported;
    bool fNativeLinkReported, fNativeJoinDispatching, fNativeBssValid, fNativeBssConfigured;
    uint64_t fNativeBssGeneration;
    uint64_t fNativeControlSequence, fNativeControlOwner;
    IOReturn waitForNativeReady();
    apple80211_beacon_msg fNativeBss;
    AiritlNativeRoamState fNativeRoam;
    bool fNativeJoinAbortPending, fNativeObserverReady;
    bool fNativeResetPending, fNativeResetQuiescencePending;
    static IOReturn nativeSetMacGated(OSObject *, void *, void *, void *, void *);
    uint8_t fNativeJoinBssid[6];
    static IOReturn nativeScanGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeAbortGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeEventGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeLinkGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeLeaveGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeJoinGated(OSObject *, void *, void *, void *, void *);
    static void nativeScanCompleted(void *, uint64_t, int32_t, uint8_t);
    IOReturn startNativeScan(apple80211ScanRequest *);
    IOReturn startNativeJoin(apple80211AssocCandidates *);
    IOReturn setNativeCipherKey(const void *);
    static IOReturn nativeCipherGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeHardwareAddressGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeChannelInfoGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeRegulatoryGated(OSObject *, void *, void *, void *, void *);
    static IOReturn nativeBssGated(OSObject *, void *, void *, void *, void *);
    bool nativeJoinCompletionMatches(uint64_t, const uint8_t *) const;
    void clearNativeBss();
    static IOReturn nativeRoamGated(OSObject *, void *, void *, void *, void *);
    IOReturn startNativeRoam(const apple80211_reassoc &);
    void handleNativeRoamEvent(int, int, uint64_t);
    void pollNativeRoam();
    void finishNativeRoam(IOReturn, int);
    void cancelNativeRoam();
    void acknowledgeNativeRoamDown(const uint8_t *);
    void releaseNativeScan();
    void releaseNativeJoin();
#endif
    // Datapath objects (provisioned in provisionDatapath()).
    IOSkywalkPacketBufferPool   *fPacketPool;
    IOSkywalkTxSubmissionQueue  *fTxSubQueue;
    IOSkywalkTxCompletionQueue  *fTxCompQueue;
    IOSkywalkRxSubmissionQueue  *fRxSubQueue;
    IOSkywalkRxCompletionQueue  *fRxCompQueue;
    IOSkywalkTxSubmissionQueue  *fMCastQueue;   // distinct multicast queue (Apple keeps it separate at ctx+0x98)
    bool                         fDatapathProvisioned;
    bool                         fDatapathEnabled;
    bool                         fFrameworkStarted;
    bool fNativeInitComplete = false;
    bool fNativeIdentityInitAttempted = false;
    bool fNativeIdentityInitComplete = false;
    bool fCustomInitComplete = false;
    enum class NativeStartPhase { NotAttempted, InProgress, Succeeded, Failed };
    NativeStartPhase fNativeStartPhase = NativeStartPhase::NotAttempted;
    enum class NativeStopPhase { NotCalled, InProgress, Returned };
    NativeStopPhase fNativeStopPhase = NativeStopPhase::NotCalled;
    enum class LocalCleanupState { Idle, InProgress, Pending, Complete, Exhausted };
    LocalCleanupState fLocalCleanupState = LocalCleanupState::Idle;
    UInt32 fLocalCleanupAttempts = 0;
    static constexpr UInt32 kMaxLocalCleanupAttempts = 3;
    bool fCleanupDisableComplete = false;
    IOReturn fLocalCleanupFailure = kIOReturnSuccess;
    const char *fLocalCleanupReason = nullptr;
    bool beginLocalCleanup();
    bool validateLocalCleanup();
    bool disableForCleanup();
    bool finishDatapathCleanup();
    bool failLocalCleanup(const char *, IOReturn, bool = false);
    bool                         fRegistrationAttempted;
    IO80211WorkQueue             *fDatapathWorkQueue;
    UInt32                       fBoundQueueMask;
#if __IO80211_TARGET >= __MAC_15_0
    AirportItlwmPacketTransport<IOSkywalkPacket, mbuf_t, 256> fPacketTransport;
    IOThread fStopThread;
    bool fRegistrationSucceeded;
    IOTimerEventSource *fTransportRetryTimer;
    bool fTransportRetryTimerBound;
    bool fTransportRetryPending;
    bool fTransportRetryRequired;
    static void transportRetryAction(OSObject *, IOTimerEventSource *);
    static IOReturn inputNativeAction(OSObject *, void *, void *, void *, void *);
    static IOReturn outputNativeAction(OSObject *, void *, void *, void *, void *);
    static IOReturn stopGatedAction(OSObject *, void *, void *, void *, void *);
    static IOReturn cleanupDisableGatedAction(OSObject *, void *, void *, void *, void *);
    IOReturn retireDatapath();
    static IOReturn retireDatapathAction(OSObject *, void *, void *, void *, void *);
    static IOReturn setNativeDatapathAction(OSObject *, void *, void *, void *, void *);
#endif
    
    //IO80211
    struct ieee80211_node *fNextNodeToSend;
    IOTimerEventSource *scanSource;
    bool fScanResultWrapping;
    volatile UInt32 fScanRequestPending;
    IOReturn beginScanRequest(bool cacheOnly);
    
    u_int32_t current_authtype_lower;
    u_int32_t current_authtype_upper;
    bool disassocIsVoluntary;
#if __IO80211_TARGET >= __MAC_15_0
    bool fDatapathRetiring = false;
#endif
    enum class FirstNativeInitPhase : UInt8 { NotAttempted = 0, InProgress, Failed, Succeeded };
    FirstNativeInitPhase fFirstNativeInitPhase = FirstNativeInitPhase::NotAttempted;
#if __IO80211_TARGET >= __MAC_15_0
    enum class SourceInitPhase : UInt8 { NotAttempted = 0, InProgress, ReturnedFailed, Succeeded };
    SourceInitPhase fSourceInitPhase = SourceInitPhase::NotAttempted;
    bool fFailedSourceInitDisposalPrepared = false;
    bool failedSourceInitHasEmptyResources() const;
    bool prepareFailedSourceInitDisposal(AirportItlwm *);
    bool failedSourceInitDisposalReady() const;
#endif
#if defined(__IO80211_TARGET) && defined(__MAC_15_0) && __IO80211_TARGET >= __MAC_15_0 && !defined(AIRITL_ENABLE_BSDIF) && !defined(AIRITL_DP_DMA)
    bool fFailedNativeStartDisposalPrepared = false;
    struct FailedNativeStartOwners {
        IOSkywalkRxCompletionQueue *rxCompletion = NULL;
        IOSkywalkRxSubmissionQueue *rxSubmission = NULL;
        IOSkywalkTxCompletionQueue *txCompletion = NULL;
        IOSkywalkTxSubmissionQueue *txSubmission = NULL;
        IOSkywalkTxSubmissionQueue *multicast = NULL;
        IOSkywalkPacketBufferPool *pool = NULL;
    };
    bool validateFailedNativeStartAbort(AirportItlwm *);
    bool takeFailedNativeStartOwners(FailedNativeStartOwners *);
    bool finishFailedNativeStartDetach(AirportItlwm *);
    bool failedNativeStartDisposalReady() const;
#endif

};


#undef AIRITL_LEGACY_HANDLER
#undef AIRITL_LEGACY_HANDLER_OVERRIDE

#endif /* AirportItlwmSkywalkInterface_hpp */
