//
// Created by Anders Cedronius (Edgeware AB) on 2020-03-14.
//

#include "RISTNet.h"
#include "RISTNetInternal.h"

//---------------------------------------------------------------------------------------------------------------------
//
//
// RIST Network tools
//
//
//---------------------------------------------------------------------------------------------------------------------

bool RISTNetTools::isIPv4(const std::string &rStr) {
    struct sockaddr_in lsa;
    return inet_pton(AF_INET, rStr.c_str(), &(lsa.sin_addr)) != 0;
}

bool RISTNetTools::isIPv6(const std::string &rStr) {
    struct sockaddr_in6 lsa;
    return inet_pton(AF_INET6, rStr.c_str(), &(lsa.sin6_addr)) != 0;
}

bool RISTNetTools::buildRISTURL(std::string lIP, std::string lPort, std::string &rURL, bool lListen) {
    int lIPType;
    if (isIPv4(lIP)) {
        lIPType = AF_INET;
    } else if (isIPv6(lIP)) {
        lIPType = AF_INET6;
    } else {
        LOGGER(true, LOGG_ERROR, " " << "Provided IP-Address not valid.")
        return false;
    }
    int lPortNum = 0;
    std::stringstream lPortNumStr(lPort);
    lPortNumStr >> lPortNum;
    if (lPortNum < 1 || lPortNum > INT16_MAX) {
        LOGGER(true, LOGG_ERROR, " " << "Provided Port number not valid.")
        return false;
    }
    std::string lRistURL = "";
    if (lIPType == AF_INET) {
        lRistURL += "rist://";
    } else {
        lRistURL += "rist6://";
    }
    if (lListen) {
        lRistURL += "@";
    }
    if (lIPType == AF_INET) {
        lRistURL += lIP + ":" + lPort;
    } else {
        lRistURL += "[" + lIP + "]:" + lPort;
    }
    rURL = lRistURL;
    return true;
}

//---------------------------------------------------------------------------------------------------------------------
//
//
// RISTNetReceiver  --  RECEIVER
//
//
//---------------------------------------------------------------------------------------------------------------------

RISTNetReceiver::RISTNetReceiver() {
    // Set the callback stubs
    validateConnectionCallback = std::bind(&RISTNetReceiver::validateConnectionStub, this, std::placeholders::_1,
                                           std::placeholders::_2);
    networkDataCallback = std::bind(&RISTNetReceiver::dataFromClientStub, this, std::placeholders::_1,
                                    std::placeholders::_2, std::placeholders::_3);
    LOGGER(false, LOGG_NOTIFY, "RISTNetReceiver constructed")
}

RISTNetReceiver::~RISTNetReceiver() {
    if (mRistContext) {
        int lStatus = rist_destroy(mRistContext);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_receiver_destroy failure")
        }
    }
    LOGGER(false, LOGG_NOTIFY, "RISTNetReceiver destruct")
}

//---------------------------------------------------------------------------------------------------------------------
// RISTNetReceiver  --  Callbacks --- Start
//---------------------------------------------------------------------------------------------------------------------


std::shared_ptr<NetworkConnection> RISTNetReceiver::validateConnectionStub(std::string lIPAddress, uint16_t lPort) {
    LOGGER(true, LOGG_ERROR,
           "validateConnectionCallback not implemented. Will not accept connection from: " << lIPAddress << ":"
                                                                                           << unsigned(lPort))
    return nullptr;
}

int RISTNetReceiver::dataFromClientStub(const uint8_t *pBuf, size_t lSize,
                                         std::shared_ptr<NetworkConnection> &rConnection) {
    LOGGER(true, LOGG_ERROR, "networkDataCallback not implemented. Data is lost")
    return -1;
}

int RISTNetReceiver::receiveData(void *pArg, const rist_data_block *pDataBlock) {
    RISTNetReceiver *lWeakSelf = (RISTNetReceiver *) pArg;
    std::lock_guard<std::mutex> lLock(lWeakSelf->mClientListMtx);

    auto netObj = lWeakSelf->mClientList.find(pDataBlock->peer);
    if (netObj != lWeakSelf->mClientList.end()) {
        auto netCon = netObj->second;
        return lWeakSelf->networkDataCallback((const uint8_t *) pDataBlock->payload, pDataBlock->payload_len, netCon, pDataBlock->peer, pDataBlock->flow_id);
    } else {
        LOGGER(true, LOGG_ERROR, "receivesendDataData mClientList <-> peer mismatch.")
    }
    return -1;
}

int RISTNetReceiver::receiveOOBData(void *pArg, const rist_oob_block *pOOBBlock) {
    RISTNetReceiver *lWeakSelf = (RISTNetReceiver *) pArg;
    if (lWeakSelf->networkOOBDataCallback) {  //This is a optional callback
        std::lock_guard<std::mutex> lLock(lWeakSelf->mClientListMtx);

        auto netObj = lWeakSelf->mClientList.find(pOOBBlock->peer);
        if (netObj != lWeakSelf->mClientList.end()) {
            auto netCon = netObj->second;
            lWeakSelf->networkOOBDataCallback((const uint8_t *) pOOBBlock->payload, pOOBBlock->payload_len, netCon, pOOBBlock->peer);
            return 0;
        }
    }
    return 0;
}


int RISTNetReceiver::clientConnect(void *pArg, const char* pConnectingIP, uint16_t lConnectingPort, const char* pIP, uint16_t lPort, struct rist_peer *pPeer) {
    RISTNetReceiver *lWeakSelf = (RISTNetReceiver *) pArg;
    auto lNetObj = lWeakSelf->validateConnectionCallback(std::string(pConnectingIP), lConnectingPort);
    if (lNetObj) {
        std::lock_guard<std::mutex> lLock(lWeakSelf->mClientListMtx);

        lWeakSelf->mClientList[pPeer] = lNetObj;
        return 0; // Accept the connection
    }
    return -1; // Reject the connection
}

int RISTNetReceiver::clientDisconnect(void *pArg, struct rist_peer *pPeer) {
    RISTNetReceiver *lWeakSelf = (RISTNetReceiver *) pArg;
    std::lock_guard<std::mutex> lLock(lWeakSelf->mClientListMtx);
    if (lWeakSelf->mClientList.empty()) {
        return 0;
    }

    if (lWeakSelf->mClientList.find(pPeer) == lWeakSelf->mClientList.end()) {
        LOGGER(true, LOGG_ERROR, "RISTNetReceiver::clientDisconnect unknown peer")
        return 0;
    } else {
        lWeakSelf->mClientList.erase(lWeakSelf->mClientList.find(pPeer)->first);
    }
    return 0;
}

//---------------------------------------------------------------------------------------------------------------------
// RISTNetReceiver  --  Callbacks --- End
//---------------------------------------------------------------------------------------------------------------------

void RISTNetReceiver::getActiveClients(
        std::function<void(std::map<struct rist_peer *, std::shared_ptr<NetworkConnection>> &)> lFunction) {
    std::lock_guard<std::mutex> lLock(mClientListMtx);

    if (lFunction) {
        lFunction(mClientList);
    }
}

bool RISTNetReceiver::closeClientConnection(struct rist_peer *lPeer) {
    std::lock_guard<std::mutex> lLock(mClientListMtx);
    auto netObj = mClientList.find(lPeer);
    if (netObj == mClientList.end()) {
        LOGGER(true, LOGG_ERROR, "Could not find peer")
        return false;
    }
    mClientList.erase(lPeer);
    int lStatus = rist_peer_destroy(mRistContext, lPeer);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_peer_destroy failed: ")
        return false;
    }
    return true;
}

void RISTNetReceiver::closeAllClientConnections() {
    std::lock_guard<std::mutex> lLock(mClientListMtx);
    for (auto &rPeer: mClientList) {
        struct rist_peer *lPeer = rPeer.first;
        int lStatus = rist_peer_destroy(mRistContext, lPeer);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_receiver_peer_destroy failed: ")
        }
    }
    mClientList.clear();
}

bool RISTNetReceiver::destroyReceiver() {
    if (mRistContext) {
        int lStatus = rist_destroy(mRistContext);
        mRistContext = nullptr;
        std::lock_guard<std::mutex> lLock(mClientListMtx);
        mClientList.clear();
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_receiver_destroy fail.")
            return false;
        }
    } else {
        LOGGER(true, LOGG_WARN, "RIST receiver not initialised.")
        return false;
    }
    return true;
}

bool RISTNetReceiver::initReceiver(std::vector<std::string> &rURLList,
                                   RISTNetReceiver::RISTNetReceiverSettings &rSettings) {
    if (!rURLList.size()) {
        LOGGER(true, LOGG_ERROR, "URL list is empty.")
        return false;
    }

    int lStatus;

    // Default log settings
    rist_logging_settings* lSettingsPtr = rSettings.mLogSetting.get();
	lStatus = rist_logging_set(&lSettingsPtr, rSettings.mLogLevel, nullptr, nullptr, nullptr, stderr);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_logging_set failed.")
        return false;
	}


    lStatus = rist_receiver_create(&mRistContext, rSettings.mProfile, rSettings.mLogSetting.get());
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_create fail.")
        return false;
    }
    for (auto &rURL: rURLList) {
        int keysize = 0;
        if (rSettings.mPSK.size()) {
            keysize = 128;
        }
        mRistPeerConfig.version = RIST_PEER_CONFIG_VERSION;
        mRistPeerConfig.virt_dst_port = RIST_DEFAULT_VIRT_DST_PORT;
        mRistPeerConfig.recovery_mode = rSettings.mPeerConfig.recovery_mode;
        mRistPeerConfig.recovery_maxbitrate = rSettings.mPeerConfig.recovery_maxbitrate;
        mRistPeerConfig.recovery_maxbitrate_return = rSettings.mPeerConfig.recovery_maxbitrate_return;
        mRistPeerConfig.recovery_length_min = rSettings.mPeerConfig.recovery_length_min;
        mRistPeerConfig.recovery_length_max = rSettings.mPeerConfig.recovery_length_max;
        mRistPeerConfig.recovery_rtt_min = rSettings.mPeerConfig.recovery_rtt_min;
        mRistPeerConfig.recovery_rtt_max = rSettings.mPeerConfig.recovery_rtt_max;
        mRistPeerConfig.weight = 5;
        mRistPeerConfig.congestion_control_mode = rSettings.mPeerConfig.congestion_control_mode;
        mRistPeerConfig.min_retries = rSettings.mPeerConfig.min_retries;
        mRistPeerConfig.max_retries = rSettings.mPeerConfig.max_retries;
        mRistPeerConfig.session_timeout = rSettings.mSessionTimeout;
        mRistPeerConfig.keepalive_interval =  rSettings.mKeepAliveInterval;
        mRistPeerConfig.key_size = keysize;

        if (keysize) {
            strncpy((char *) &mRistPeerConfig.secret[0], rSettings.mPSK.c_str(), 128);
        }

        if (rSettings.mCNAME.size()) {
            strncpy((char *) &mRistPeerConfig.cname[0], rSettings.mCNAME.c_str(), 128);
        }

        const struct rist_peer_config* lTmp = &mRistPeerConfig;
        lStatus = rist_parse_address(rURL.c_str(), &lTmp);
        if (lStatus)
        {
            LOGGER(true, LOGG_ERROR, "rist_parse_address fail: " << rURL)
            destroyReceiver();
            return false;
        }

        struct rist_peer *peer;
        lStatus =  rist_peer_create(mRistContext, &peer, &mRistPeerConfig);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_receiver_peer_create fail: " << rURL)
            destroyReceiver();
            return false;
        }
    }

    if (rSettings.mMaxjitter) {
        lStatus = rist_jitter_max_set(mRistContext, rSettings.mMaxjitter);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_receiver_jitter_max_set fail.")
            destroyReceiver();
            return false;
        }
    }

    lStatus = rist_oob_callback_set(mRistContext, receiveOOBData, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_oob_set fail.")
        destroyReceiver();
        return false;
    }

    lStatus = rist_receiver_data_callback_set(mRistContext, receiveData, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_data_callback_set fail.")
        destroyReceiver();
        return false;
    }

    lStatus = rist_auth_handler_set(mRistContext, clientConnect, clientDisconnect, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_auth_handler_set fail.")
        destroyReceiver();
        return false;
    }

    lStatus = rist_start(mRistContext);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_start fail.")
        destroyReceiver();
        return false;
    }
    return true;
}

bool RISTNetReceiver::sendOOBData(struct rist_peer *pPeer, const uint8_t *pData, size_t lSize) {
    if (!mRistContext) {
        LOGGER(true, LOGG_ERROR, "RISTNetReceiver not initialised.")
        return false;
    }

    rist_oob_block myOOBBlock = {0};
    myOOBBlock.peer = pPeer;
    myOOBBlock.payload = pData;
    myOOBBlock.payload_len = lSize;

    int lStatus = rist_oob_write(mRistContext, &myOOBBlock);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_receiver_oob_write failed.")
        destroyReceiver();
        return false;
    }
    return true;
}

void RISTNetReceiver::getVersion(uint32_t &rCppWrapper, uint32_t &rRistMajor, uint32_t &rRistMinor) {
    rCppWrapper = CPP_WRAPPER_VERSION;
    rRistMajor = LIBRIST_API_VERSION_MAJOR;
    rRistMinor = LIBRIST_API_VERSION_MINOR;
}

//---------------------------------------------------------------------------------------------------------------------
//
//
// RISTNetSender  --  SENDER
//
//
//---------------------------------------------------------------------------------------------------------------------

RISTNetSender::RISTNetSender() {
    validateConnectionCallback = std::bind(&RISTNetSender::validateConnectionStub, this, std::placeholders::_1,
                                           std::placeholders::_2);
    LOGGER(false, LOGG_NOTIFY, "RISTNetSender constructed")
}

RISTNetSender::~RISTNetSender() {
    if (mRistContext) {
        int lStatus = rist_destroy(mRistContext);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_sender_destroy fail.")
        }
    }
    LOGGER(false, LOGG_NOTIFY, "RISTNetClient destruct.")
}

//---------------------------------------------------------------------------------------------------------------------
// RISTNetSender  --  Callbacks --- Start
//---------------------------------------------------------------------------------------------------------------------

std::shared_ptr<NetworkConnection> RISTNetSender::validateConnectionStub(std::string ipAddress, uint16_t port) {
    LOGGER(true, LOGG_ERROR,
           "validateConnectionCallback not implemented. Will not accept connection from: " << ipAddress << ":"
                                                                                           << unsigned(port))
    return 0;
}

int RISTNetSender::receiveOOBData(void *pArg, const rist_oob_block *pOOBBlock) {
    RISTNetSender *lWeakSelf = (RISTNetSender *) pArg;
    if (lWeakSelf->networkOOBDataCallback) {  //This is a optional callback
        std::lock_guard<std::mutex> lLock(lWeakSelf->mClientListMtx);
        auto netObj = lWeakSelf->mClientList.find(pOOBBlock->peer);
        if (netObj != lWeakSelf->mClientList.end()) {
            auto netCon = netObj->second;
            lWeakSelf->networkOOBDataCallback((const uint8_t *) pOOBBlock->payload, pOOBBlock->payload_len, netCon, pOOBBlock->peer);
            return 0;
        }
    }
    return 0;
}

int RISTNetSender::clientConnect(void *pArg, const char* pConnectingIP, uint16_t lConnectingPort, const char* pIP, uint16_t lPort, struct rist_peer *pPeer) {
    RISTNetSender *lWeakSelf = (RISTNetSender *) pArg;
    auto lNetObj = lWeakSelf->validateConnectionCallback(std::string(pConnectingIP), lConnectingPort);
    if (lNetObj) {
        std::lock_guard<std::mutex> lLock(lWeakSelf->mClientListMtx);
        lWeakSelf->mClientList[pPeer] = lNetObj;
        return 0; // Accept the connection
    }
    return -1; // Reject the connection
}

int RISTNetSender::clientDisconnect(void *pArg, struct rist_peer *pPeer) {
    RISTNetSender *lWeakSelf = (RISTNetSender *) pArg;
    std::lock_guard<std::mutex> lLock(lWeakSelf->mClientListMtx);
    if (lWeakSelf->mClientList.empty()) {
        return 0;
    }

    if (lWeakSelf->mClientList.find(pPeer) == lWeakSelf->mClientList.end()) {
        LOGGER(true, LOGG_ERROR, "RISTNetSender::clientDisconnect unknown peer")
        return 0;
    } else {
        lWeakSelf->mClientList.erase(lWeakSelf->mClientList.find(pPeer)->first);
    }
    return 0;
}

//---------------------------------------------------------------------------------------------------------------------
// RISTNetSender  --  Callbacks --- End
//---------------------------------------------------------------------------------------------------------------------

void RISTNetSender::getActiveClients(
        std::function<void(std::map<struct rist_peer *, std::shared_ptr<NetworkConnection>> &)> lFunction) {
    std::lock_guard<std::mutex> lLock(mClientListMtx);
    if (lFunction) {
        lFunction(mClientList);
    }
}

bool RISTNetSender::closeClientConnection(struct rist_peer *lPeer) {
    std::lock_guard<std::mutex> lLock(mClientListMtx);
    auto netObj = mClientList.find(lPeer);
    if (netObj == mClientList.end()) {
        LOGGER(true, LOGG_ERROR, "Could not find peer")
        return false;
    }
    mClientList.erase(lPeer);
    int lStatus = rist_peer_destroy(mRistContext, lPeer);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_peer_destroy failed: ")
        return false;
    }
    return true;
}

void RISTNetSender::closeAllClientConnections() {
    std::lock_guard<std::mutex> lLock(mClientListMtx);
    for (auto &rPeer: mClientList) {
        struct rist_peer *pPeer = rPeer.first;
        int status = rist_peer_destroy(mRistContext, pPeer);
        if (status) {
            LOGGER(true, LOGG_ERROR, "rist_sender_peer_destroy failed: ")
        }
    }
    mClientList.clear();
}

bool RISTNetSender::destroySender() {
    if (mRistContext) {
        int lStatus = rist_destroy(mRistContext);
        mRistContext = nullptr;
        std::lock_guard<std::mutex> lLock(mClientListMtx);
        mClientList.clear();
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_sender_destroy fail.")
            return false;
        }
    } else {
        LOGGER(true, LOGG_WARN, "RIST Sender not running.")
    }
    return true;
}

bool RISTNetSender::initSender(std::vector<std::tuple<std::string,int>> &rPeerList,
                               RISTNetSenderSettings &rSettings) {

    if (!rPeerList.size()) {
        LOGGER(true, LOGG_ERROR, "URL list is empty.")
        return false;
    }

    int lStatus;
    // Default log settings
    rist_logging_settings* lSettingsPtr = rSettings.mLogSetting.get();
	lStatus = rist_logging_set(&lSettingsPtr, rSettings.mLogLevel, nullptr, nullptr, nullptr, stderr);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_logging_set failed.")
        return false;
	}

    lStatus = rist_sender_create(&mRistContext, rSettings.mProfile, 0, rSettings.mLogSetting.get());
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_create fail.")
        return false;
    }
    for (auto &rPeerInfo: rPeerList) {

        auto peerURL = std::get<0>(rPeerInfo);

        int keysize = 0;
        if (rSettings.mPSK.size()) {
            keysize = 128;
        }
        mRistPeerConfig.version = RIST_PEER_CONFIG_VERSION;
        mRistPeerConfig.virt_dst_port = RIST_DEFAULT_VIRT_DST_PORT;
        mRistPeerConfig.recovery_mode = rSettings.mPeerConfig.recovery_mode;
        mRistPeerConfig.recovery_maxbitrate = rSettings.mPeerConfig.recovery_maxbitrate;
        mRistPeerConfig.recovery_maxbitrate_return = rSettings.mPeerConfig.recovery_maxbitrate_return;
        mRistPeerConfig.recovery_length_min = rSettings.mPeerConfig.recovery_length_min;
        mRistPeerConfig.recovery_length_max = rSettings.mPeerConfig.recovery_length_max;
        mRistPeerConfig.recovery_rtt_min = rSettings.mPeerConfig.recovery_rtt_min;
        mRistPeerConfig.recovery_rtt_max = rSettings.mPeerConfig.recovery_rtt_max;
        mRistPeerConfig.weight = std::get<1>(rPeerInfo);
        mRistPeerConfig.congestion_control_mode = rSettings.mPeerConfig.congestion_control_mode;
        mRistPeerConfig.min_retries = rSettings.mPeerConfig.min_retries;
        mRistPeerConfig.max_retries = rSettings.mPeerConfig.max_retries;
        mRistPeerConfig.session_timeout = rSettings.mSessionTimeout;
        mRistPeerConfig.keepalive_interval =  rSettings.mKeepAliveInterval;
        mRistPeerConfig.key_size = keysize;

        if (keysize) {
            strncpy((char *) &mRistPeerConfig.secret[0], rSettings.mPSK.c_str(), 128);
        }

        if (rSettings.mCNAME.size()) {
            strncpy((char *) &mRistPeerConfig.cname[0], rSettings.mCNAME.c_str(), 128);
        }

        const struct rist_peer_config* lTmp = &mRistPeerConfig;
        lStatus = rist_parse_address(peerURL.c_str(), &lTmp);
        if (lStatus)
        {
            LOGGER(true, LOGG_ERROR, "rist_parse_address fail: " << peerURL)
            destroySender();
            return false;
        }

        struct rist_peer *peer;
        lStatus =  rist_peer_create(mRistContext, &peer, &mRistPeerConfig);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_sender_peer_create fail: " << peerURL)
            destroySender();
            return false;
        }
    }

    if (rSettings.mMaxJitter) {
        lStatus = rist_jitter_max_set(mRistContext, rSettings.mMaxJitter);
        if (lStatus) {
            LOGGER(true, LOGG_ERROR, "rist_sender_jitter_max_set fail.")
            destroySender();
            return false;
        }
    }

    lStatus = rist_oob_callback_set(mRistContext, receiveOOBData, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_oob_set fail.")
        destroySender();
        return false;
    }

    lStatus = rist_auth_handler_set(mRistContext, clientConnect, clientDisconnect, this);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_auth_handler_set fail.")
        destroySender();
        return false;
    }

    lStatus = rist_start(mRistContext);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_start fail.")
        destroySender();
        return false;
    }

    return true;
}

bool RISTNetSender::sendData(const uint8_t *pData, size_t lSize, uint16_t lConnectionID) {
    if (!mRistContext) {
        LOGGER(true, LOGG_ERROR, "RISTNetSender not initialised.")
        return false;
    }

    rist_data_block myRISTDataBlock = {0};
    myRISTDataBlock.payload = pData;
    myRISTDataBlock.payload_len = lSize;
    myRISTDataBlock.flow_id = lConnectionID;

    int lStatus = rist_sender_data_write(mRistContext, &myRISTDataBlock);
    if (lStatus < 0) {
        LOGGER(true, LOGG_ERROR, "rist_client_write failed.")
        destroySender();
        return false;
    }

    if (lStatus != lSize) {
        LOGGER(true, LOGG_ERROR, "Did send " << lStatus << " bytes, out of " << lSize << " bytes." )
        return false;
    }

    return true;
}

bool RISTNetSender::sendOOBData(struct rist_peer *pPeer, const uint8_t *pData, size_t lSize) {
    if (!mRistContext) {
        LOGGER(true, LOGG_ERROR, "RISTNetSender not initialised.")
        return false;
    }

    rist_oob_block myOOBBlock = {0};
    myOOBBlock.peer = pPeer;
    myOOBBlock.payload = pData;
    myOOBBlock.payload_len = lSize;

    int lStatus = rist_oob_write(mRistContext, &myOOBBlock);
    if (lStatus) {
        LOGGER(true, LOGG_ERROR, "rist_sender_oob_write failed.")
        destroySender();
        return false;
    }
    return true;
}

void RISTNetSender::getVersion(uint32_t &rCppWrapper, uint32_t &rRistMajor, uint32_t &rRistMinor) {
    rCppWrapper = CPP_WRAPPER_VERSION;
    rRistMajor = LIBRIST_API_VERSION_MAJOR;
    rRistMinor = LIBRIST_API_VERSION_MINOR;
}
