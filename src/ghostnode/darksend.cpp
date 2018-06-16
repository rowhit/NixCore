// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2017-2018 The NIX Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeghostnode.h"
#include "wallet/coincontrol.h"
#include "consensus/validation.h"
#include "darksend.h"
#include "init.h"
#include "instantx.h"
#include "ghostnode-payments.h"
#include "ghostnode-sync.h"
#include "ghostnodeman.h"
#include "script/sign.h"
#include "txmempool.h"
#include "util.h"
#include "utilmoneystr.h"
#include "net_processing.h"
#include "netmessagemaker.h"

#include <boost/lexical_cast.hpp>

int nPrivateSendRounds = DEFAULT_PRIVATESEND_ROUNDS;
int nPrivateSendAmount = DEFAULT_PRIVATESEND_AMOUNT;
int nLiquidityProvider = DEFAULT_PRIVATESEND_LIQUIDITY;
bool fEnablePrivateSend = false;
bool fPrivateSendMultiSession = DEFAULT_PRIVATESEND_MULTISESSION;

CDarksendPool darkSendPool;
CDarkSendSigner darkSendSigner;
std::map <uint256, CDarksendBroadcastTx> mapDarksendBroadcastTxes;
std::vector <CAmount> vecPrivateSendDenominations;

void CDarksendPool::ProcessMessage(CNode *pfrom, std::string &strCommand, CDataStream &vRecv) {
    if (fLiteMode) return; // ignore all Dash related functionality
    if (!ghostnodeSync.IsBlockchainSynced()) return;

    if (strCommand == NetMsgType::DSACCEPT) {

        if (pfrom->nVersion < MIN_PRIVATESEND_PEER_PROTO_VERSION) {
            LogPrintf("DSACCEPT -- incompatible version! nVersion: %d\n", pfrom->nVersion);
            PushStatus(pfrom, STATUS_REJECTED, ERR_VERSION);
            return;
        }

        if (!fGhostNode) {
            LogPrintf("DSACCEPT -- not a Ghostnode!\n");
            PushStatus(pfrom, STATUS_REJECTED, ERR_NOT_A_MN);
            return;
        }

        if (IsSessionReady()) {
            // too many users in this session already, reject new ones
            LogPrintf("DSACCEPT -- queue is already full!\n");
            PushStatus(pfrom, STATUS_ACCEPTED, ERR_QUEUE_FULL);
            return;
        }

        int nDenom;
        CTransactionRef txCollateral;
        vRecv >> nDenom >> txCollateral;

        LogPrintf("privatesend", "DSACCEPT -- nDenom %d (%s)  txCollateral %s", nDenom, GetDenominationsToString(nDenom), txCollateral->ToString());

        CGhostnode *pmn = mnodeman.Find(activeGhostnode.vin);
        if (pmn == NULL) {
            PushStatus(pfrom, STATUS_REJECTED, ERR_MN_LIST);
            return;
        }

        if (vecSessionCollaterals.size() == 0 && pmn->nLastDsq != 0 &&
            pmn->nLastDsq + mnodeman.CountEnabled(MIN_PRIVATESEND_PEER_PROTO_VERSION) / 5 > mnodeman.nDsqCount) {
            LogPrintf("DSACCEPT -- last dsq too recent, must wait: addr=%s\n", pfrom->addr.ToString());
            PushStatus(pfrom, STATUS_REJECTED, ERR_RECENT);
            return;
        }

        PoolMessage nMessageID = MSG_NOERR;
        bool fResult = nSessionID == 0 ? CreateNewSession(nDenom, *txCollateral, nMessageID)
                                       : AddUserToExistingSession(nDenom, *txCollateral, nMessageID);
        if (fResult) {
            LogPrintf("DSACCEPT -- is compatible, please submit!\n");
            PushStatus(pfrom, STATUS_ACCEPTED, nMessageID);
            return;
        } else {
            LogPrintf("DSACCEPT -- not compatible with existing transactions!\n");
            PushStatus(pfrom, STATUS_REJECTED, nMessageID);
            return;
        }

    } else if (strCommand == NetMsgType::DSQUEUE) {
        TRY_LOCK(cs_darksend, lockRecv);
        if (!lockRecv) return;

        if (pfrom->nVersion < MIN_PRIVATESEND_PEER_PROTO_VERSION) {
            LogPrintf("privatesend", "DSQUEUE -- incompatible version! nVersion: %d\n", pfrom->nVersion);
            return;
        }

        CDarksendQueue dsq;
        vRecv >> dsq;

        // process every dsq only once
        BOOST_FOREACH(CDarksendQueue
        q, vecDarksendQueue) {
            if (q == dsq) {
                // LogPrintf("privatesend", "DSQUEUE -- %s seen\n", dsq.ToString());
                return;
            }
        }

        LogPrintf("privatesend", "DSQUEUE -- %s new\n", dsq.ToString());

        if (dsq.IsExpired() || dsq.nTime > GetTime() + PRIVATESEND_QUEUE_TIMEOUT) return;

        CGhostnode *pmn = mnodeman.Find(dsq.vin);
        if (pmn == NULL) return;

        if (!dsq.CheckSignature(pmn->pubKeyGhostnode)) {
            // we probably have outdated info
            mnodeman.AskForMN(pfrom, dsq.vin);
            return;
        }

        // if the queue is ready, submit if we can
        if (dsq.fReady) {
            if (!pSubmittedToGhostnode) return;
            if ((CNetAddr) pSubmittedToGhostnode->addr != (CNetAddr) pmn->addr) {
                LogPrintf("DSQUEUE -- message doesn't match current Ghostnode: pSubmittedToGhostnode=%s, addr=%s\n", pSubmittedToGhostnode->addr.ToString(), pmn->addr.ToString());
                return;
            }

            if (nState == POOL_STATE_QUEUE) {
                LogPrintf("privatesend", "DSQUEUE -- PrivateSend queue (%s) is ready on ghostnode %s\n", dsq.ToString(), pmn->addr.ToString());
                SubmitDenominate();
            }
        } else {
            BOOST_FOREACH(CDarksendQueue
            q, vecDarksendQueue) {
                if (q.vin == dsq.vin) {
                    // no way same mn can send another "not yet ready" dsq this soon
                    LogPrintf("privatesend", "DSQUEUE -- Ghostnode %s is sending WAY too many dsq messages\n", pmn->addr.ToString());
                    return;
                }
            }

            int nThreshold = pmn->nLastDsq + mnodeman.CountEnabled(MIN_PRIVATESEND_PEER_PROTO_VERSION) / 5;
            LogPrintf("privatesend", "DSQUEUE -- nLastDsq: %d  threshold: %d  nDsqCount: %d\n", pmn->nLastDsq, nThreshold, mnodeman.nDsqCount);
            //don't allow a few nodes to dominate the queuing process
            if (pmn->nLastDsq != 0 && nThreshold > mnodeman.nDsqCount) {
                LogPrintf("privatesend", "DSQUEUE -- Ghostnode %s is sending too many dsq messages\n", pmn->addr.ToString());
                return;
            }
            mnodeman.nDsqCount++;
            pmn->nLastDsq = mnodeman.nDsqCount;
            pmn->fAllowMixingTx = true;

            LogPrintf("privatesend", "DSQUEUE -- new PrivateSend queue (%s) from ghostnode %s\n", dsq.ToString(), pmn->addr.ToString());
            if (pSubmittedToGhostnode && pSubmittedToGhostnode->vin.prevout == dsq.vin.prevout) {
                dsq.fTried = true;
            }
            vecDarksendQueue.push_back(dsq);
            dsq.Relay();
        }

    } else if (strCommand == NetMsgType::DSVIN) {

        if (pfrom->nVersion < MIN_PRIVATESEND_PEER_PROTO_VERSION) {
            LogPrintf("DSVIN -- incompatible version! nVersion: %d\n", pfrom->nVersion);
            PushStatus(pfrom, STATUS_REJECTED, ERR_VERSION);
            return;
        }

        if (!fGhostNode) {
            LogPrintf("DSVIN -- not a Ghostnode!\n");
            PushStatus(pfrom, STATUS_REJECTED, ERR_NOT_A_MN);
            return;
        }

        //do we have enough users in the current session?
        if (!IsSessionReady()) {
            LogPrintf("DSVIN -- session not complete!\n");
            PushStatus(pfrom, STATUS_REJECTED, ERR_SESSION);
            return;
        }

        CDarkSendEntry entry;
        vRecv >> entry;

        LogPrintf("privatesend", "DSVIN -- txCollateral %s", entry.txCollateral->ToString());

        //do we have the same denominations as the current session?
        if (!IsOutputsCompatibleWithSessionDenom(entry.vecTxDSOut)) {
            LogPrintf("DSVIN -- not compatible with existing transactions!\n");
            PushStatus(pfrom, STATUS_REJECTED, ERR_EXISTING_TX);
            return;
        }

        //check it like a transaction
        {
            CAmount nValueIn = 0;
            CAmount nValueOut = 0;

            CMutableTransaction tx;

            /*
            BOOST_FOREACH(const CTxOut txout, entry.vecTxDSOut) {
                nValueOut += txout.nValue;
                tx.vout.push_back(txout);

                if (txout.scriptPubKey.size() != 25) {
                    LogPrintf("DSVIN -- non-standard pubkey detected! scriptPubKey=%s\n", ScriptToAsmStr(txout.scriptPubKey));
                    PushStatus(pfrom, STATUS_REJECTED, ERR_NON_STANDARD_PUBKEY);
                    return;
                }
                if (!txout.scriptPubKey.IsNormalPaymentScript()) {
                    LogPrintf("DSVIN -- invalid script! scriptPubKey=%s\n", ScriptToAsmStr(txout.scriptPubKey));
                    PushStatus(pfrom, STATUS_REJECTED, ERR_INVALID_SCRIPT);
                    return;
                }
            }
            */
            for (unsigned int idx = 0; idx < entry.vecTxDSOut.size(); idx++)
            {

                OUTPUT_PTR<CTxOutStandard> txOutStandard = MAKE_OUTPUT<CTxOutStandard>();
                txOutStandard->nValue = entry.vecTxDSOut[idx].GetValue();
                txOutStandard->scriptPubKey = *entry.vecTxDSOut[idx].GetPScriptPubKey();
                tx.vpout.push_back(txOutStandard);

                nValueOut += txOutStandard->GetValue();

                if (txOutStandard->GetPScriptPubKey()->size()  != 25){
                    LogPrintf("DSVIN -- non-standard pubkey detected! scriptPubKey=%s\n", ScriptToAsmStr(*txOutStandard->GetPScriptPubKey()));
                    PushStatus(pfrom, STATUS_REJECTED, ERR_NON_STANDARD_PUBKEY);
                    return;
                }
                if (!txOutStandard->GetPScriptPubKey()->IsNormalPaymentScript()){
                    LogPrintf("DSVIN -- invalid script! scriptPubKey=%s\n", ScriptToAsmStr(*txOutStandard->GetPScriptPubKey()));
                    PushStatus(pfrom, STATUS_REJECTED, ERR_INVALID_SCRIPT);
                    return;
                }
            }

            BOOST_FOREACH(
            const CTxIn txin, entry.vecTxDSIn) {
                tx.vin.push_back(txin);

                LogPrintf("privatesend", "DSVIN -- txin=%s\n", txin.ToString());

                CTransactionRef txPrev;
                uint256 hash;
                if (GetTransaction(txin.prevout.hash, txPrev, Params().GetConsensus(), hash, true)) {
                    if (txPrev->vpout.size() > txin.prevout.n)
                        nValueIn += txPrev->vpout[txin.prevout.n]->GetValue();
                } else {
                    LogPrintf("DSVIN -- missing input! tx=%s", tx.ToString());
                    PushStatus(pfrom, STATUS_REJECTED, ERR_MISSING_TX);
                    return;
                }
            }

            if (nValueIn > PRIVATESEND_POOL_MAX) {
                LogPrintf("DSVIN -- more than PrivateSend pool max! nValueIn: %lld, tx=%s", nValueIn, tx.ToString());
                PushStatus(pfrom, STATUS_REJECTED, ERR_MAXIMUM);
                return;
            }

            // Allow lowest denom (at max) as a a fee. Normally shouldn't happen though.
            // TODO: Or do not allow fees at all?
            if (nValueIn - nValueOut > vecPrivateSendDenominations.back()) {
                LogPrintf("DSVIN -- fees are too high! fees: %lld, tx=%s", nValueIn - nValueOut, tx.ToString());
                PushStatus(pfrom, STATUS_REJECTED, ERR_FEES);
                return;
            }

            {
                LOCK(cs_main);
                CValidationState validationState;
                mempool.PrioritiseTransaction(tx.GetHash(), 0.1 * COIN);
                const CTransaction txTemp(tx);
                CTransactionRef txRef(&txTemp);
                if (!AcceptToMemoryPool(mempool, validationState, txRef, nullptr, nullptr, false, 0)) {
                    LogPrintf("DSVIN -- transaction not valid! tx=%s", tx.ToString());
                    PushStatus(pfrom, STATUS_REJECTED, ERR_INVALID_TX);
                    return;
                }
            }
        }
        PoolMessage nMessageID = MSG_NOERR;

        if (AddEntry(entry, nMessageID)) {
            PushStatus(pfrom, STATUS_ACCEPTED, nMessageID);
            CheckPool();
            RelayStatus(STATUS_ACCEPTED);
        } else {
            PushStatus(pfrom, STATUS_REJECTED, nMessageID);
            SetNull();
        }

    } else if (strCommand == NetMsgType::DSSTATUSUPDATE) {

        if (pfrom->nVersion < MIN_PRIVATESEND_PEER_PROTO_VERSION) {
            LogPrintf("DSSTATUSUPDATE -- incompatible version! nVersion: %d\n", pfrom->nVersion);
            return;
        }

        if (fGhostNode) {
            // LogPrintf("DSSTATUSUPDATE -- Can't run on a Ghostnode!\n");
            return;
        }

        if (!pSubmittedToGhostnode) return;
        if ((CNetAddr) pSubmittedToGhostnode->addr != (CNetAddr) pfrom->addr) {
            //LogPrintf("DSSTATUSUPDATE -- message doesn't match current Ghostnode: pSubmittedToGhostnode %s addr %s\n", pSubmittedToGhostnode->addr.ToString(), pfrom->addr.ToString());
            return;
        }

        int nMsgSessionID;
        int nMsgState;
        int nMsgEntriesCount;
        int nMsgStatusUpdate;
        int nMsgMessageID;
        vRecv >> nMsgSessionID >> nMsgState >> nMsgEntriesCount >> nMsgStatusUpdate >> nMsgMessageID;

        LogPrintf("privatesend", "DSSTATUSUPDATE -- nMsgSessionID %d  nMsgState: %d  nEntriesCount: %d  nMsgStatusUpdate: %d  nMsgMessageID %d\n",
                 nMsgSessionID, nMsgState, nEntriesCount, nMsgStatusUpdate, nMsgMessageID);

        if (nMsgState < POOL_STATE_MIN || nMsgState > POOL_STATE_MAX) {
            LogPrintf("privatesend", "DSSTATUSUPDATE -- nMsgState is out of bounds: %d\n", nMsgState);
            return;
        }

        if (nMsgStatusUpdate < STATUS_REJECTED || nMsgStatusUpdate > STATUS_ACCEPTED) {
            LogPrintf("privatesend", "DSSTATUSUPDATE -- nMsgStatusUpdate is out of bounds: %d\n", nMsgStatusUpdate);
            return;
        }

        if (nMsgMessageID < MSG_POOL_MIN || nMsgMessageID > MSG_POOL_MAX) {
            LogPrintf("privatesend", "DSSTATUSUPDATE -- nMsgMessageID is out of bounds: %d\n", nMsgMessageID);
            return;
        }

        LogPrintf("privatesend", "DSSTATUSUPDATE -- GetMessageByID: %s\n", GetMessageByID(PoolMessage(nMsgMessageID)));

        if (!CheckPoolStateUpdate(PoolState(nMsgState), nMsgEntriesCount, PoolStatusUpdate(nMsgStatusUpdate), PoolMessage(nMsgMessageID), nMsgSessionID)) {
            LogPrintf("privatesend", "DSSTATUSUPDATE -- CheckPoolStateUpdate failed\n");
        }

    } else if (strCommand == NetMsgType::DSSIGNFINALTX) {

        if (pfrom->nVersion < MIN_PRIVATESEND_PEER_PROTO_VERSION) {
            LogPrintf("DSSIGNFINALTX -- incompatible version! nVersion: %d\n", pfrom->nVersion);
            return;
        }

        if (!fGhostNode) {
            LogPrintf("DSSIGNFINALTX -- not a Ghostnode!\n");
            return;
        }

        std::vector <CTxIn> vecTxIn;
        vRecv >> vecTxIn;

        LogPrintf("privatesend", "DSSIGNFINALTX -- vecTxIn.size() %s\n", vecTxIn.size());

        int nTxInIndex = 0;
        int nTxInsCount = (int) vecTxIn.size();

        BOOST_FOREACH(
        const CTxIn txin, vecTxIn) {
            nTxInIndex++;
            if (!AddScriptSig(txin)) {
                LogPrintf("privatesend", "DSSIGNFINALTX -- AddScriptSig() failed at %d/%d, session: %d\n", nTxInIndex, nTxInsCount, nSessionID);
                RelayStatus(STATUS_REJECTED);
                return;
            }
            LogPrintf("privatesend", "DSSIGNFINALTX -- AddScriptSig() %d/%d success\n", nTxInIndex, nTxInsCount);
        }
        // all is good
        CheckPool();

    } else if (strCommand == NetMsgType::DSFINALTX) {

        if (pfrom->nVersion < MIN_PRIVATESEND_PEER_PROTO_VERSION) {
            LogPrintf("DSFINALTX -- incompatible version! nVersion: %d\n", pfrom->nVersion);
            return;
        }

        if (fGhostNode) {
            // LogPrintf("DSFINALTX -- Can't run on a Ghostnode!\n");
            return;
        }

        if (!pSubmittedToGhostnode) return;
        if ((CNetAddr) pSubmittedToGhostnode->addr != (CNetAddr) pfrom->addr) {
            //LogPrintf("DSFINALTX -- message doesn't match current Ghostnode: pSubmittedToGhostnode %s addr %s\n", pSubmittedToGhostnode->addr.ToString(), pfrom->addr.ToString());
            return;
        }

        int nMsgSessionID;
        CTransactionRef txNew;
        vRecv >> nMsgSessionID >> txNew;

        if (nSessionID != nMsgSessionID) {
            LogPrintf("privatesend", "DSFINALTX -- message doesn't match current PrivateSend session: nSessionID: %d  nMsgSessionID: %d\n", nSessionID, nMsgSessionID);
            return;
        }

        LogPrintf("privatesend", "DSFINALTX -- txNew %s", txNew->ToString());

        //check to see if input is spent already? (and probably not confirmed)
        SignFinalTransaction(*txNew, pfrom);

    } else if (strCommand == NetMsgType::DSCOMPLETE) {

        if (pfrom->nVersion < MIN_PRIVATESEND_PEER_PROTO_VERSION) {
            LogPrintf("DSCOMPLETE -- incompatible version! nVersion: %d\n", pfrom->nVersion);
            return;
        }

        if (fGhostNode) {
            // LogPrintf("DSCOMPLETE -- Can't run on a Ghostnode!\n");
            return;
        }

        if (!pSubmittedToGhostnode) return;
        if ((CNetAddr) pSubmittedToGhostnode->addr != (CNetAddr) pfrom->addr) {
            LogPrintf("privatesend", "DSCOMPLETE -- message doesn't match current Ghostnode: pSubmittedToGhostnode=%s  addr=%s\n", pSubmittedToGhostnode->addr.ToString(), pfrom->addr.ToString());
            return;
        }

        int nMsgSessionID;
        int nMsgMessageID;
        vRecv >> nMsgSessionID >> nMsgMessageID;

        if (nMsgMessageID < MSG_POOL_MIN || nMsgMessageID > MSG_POOL_MAX) {
            LogPrintf("privatesend", "DSCOMPLETE -- nMsgMessageID is out of bounds: %d\n", nMsgMessageID);
            return;
        }

        if (nSessionID != nMsgSessionID) {
            LogPrintf("privatesend", "DSCOMPLETE -- message doesn't match current PrivateSend session: nSessionID: %d  nMsgSessionID: %d\n", darkSendPool.nSessionID, nMsgSessionID);
            return;
        }

        LogPrintf("privatesend", "DSCOMPLETE -- nMsgSessionID %d  nMsgMessageID %d (%s)\n", nMsgSessionID, nMsgMessageID, GetMessageByID(PoolMessage(nMsgMessageID)));

        CompletedTransaction(PoolMessage(nMsgMessageID));
    }
}

void CDarksendPool::InitDenominations() {
    vecPrivateSendDenominations.clear();
    /* Denominations

        A note about convertability. Within mixing pools, each denomination
        is convertable to another.

        For example:
        1DRK+1000 == (.1DRK+100)*10
        10DRK+10000 == (1DRK+1000)*10
    */
    /* Disabled
    vecPrivateSendDenominations.push_back( (100      * COIN)+100000 );
    */
    vecPrivateSendDenominations.push_back((10 * COIN) + 10000);
    vecPrivateSendDenominations.push_back((1 * COIN) + 1000);
    vecPrivateSendDenominations.push_back((.1 * COIN) + 100);
    vecPrivateSendDenominations.push_back((.01 * COIN) + 10);
    /* Disabled till we need them
    vecPrivateSendDenominations.push_back( (.001     * COIN)+1 );
    */
}

void CDarksendPool::ResetPool() {
    nCachedLastSuccessBlock = 0;
    txMyCollateral = CMutableTransaction();
    vecGhostnodesUsed.clear();
    UnlockCoins();
    SetNull();
}

void CDarksendPool::SetNull() {
    // MN side
    vecSessionCollaterals.clear();

    // Client side
    nEntriesCount = 0;
    fLastEntryAccepted = false;
    pSubmittedToGhostnode = NULL;

    // Both sides
    nState = POOL_STATE_IDLE;
    nSessionID = 0;
    nSessionDenom = 0;
    vecEntries.clear();
    finalMutableTransaction.vin.clear();
    finalMutableTransaction.vout.clear();
    nTimeLastSuccessfulStep = GetTimeMillis();
}

//
// Unlock coins after mixing fails or succeeds
//
void CDarksendPool::UnlockCoins() {
    while (true) {
        TRY_LOCK(GetHDWallet(vpwallets.front())->cs_wallet, lockWallet);
        if (!lockWallet) {
            MilliSleep(50);
            continue;
        }
        BOOST_FOREACH(COutPoint
        outpoint, vecOutPointLocked)
        GetHDWallet(vpwallets.front())->UnlockCoin(outpoint);
        break;
    }

    vecOutPointLocked.clear();
}

std::string CDarksendPool::GetStateString() const {
    switch (nState) {
        case POOL_STATE_IDLE:
            return "IDLE";
        case POOL_STATE_QUEUE:
            return "QUEUE";
        case POOL_STATE_ACCEPTING_ENTRIES:
            return "ACCEPTING_ENTRIES";
        case POOL_STATE_SIGNING:
            return "SIGNING";
        case POOL_STATE_ERROR:
            return "ERROR";
        case POOL_STATE_SUCCESS:
            return "SUCCESS";
        default:
            return "UNKNOWN";
    }
}

std::string CDarksendPool::GetStatus() {
    static int nStatusMessageProgress = 0;
    nStatusMessageProgress += 10;
    std::string strSuffix = "";

    if ((pCurrentBlockIndex && pCurrentBlockIndex->nHeight - nCachedLastSuccessBlock < nMinBlockSpacing) || !ghostnodeSync.IsBlockchainSynced())
        return strAutoDenomResult;

    switch (nState) {
        case POOL_STATE_IDLE:
            return _("PrivateSend is idle.");
        case POOL_STATE_QUEUE:
            if (nStatusMessageProgress % 70 <= 30) strSuffix = ".";
            else if (nStatusMessageProgress % 70 <= 50) strSuffix = "..";
            else if (nStatusMessageProgress % 70 <= 70) strSuffix = "...";
            return strprintf(_("Submitted to ghostnode, waiting in queue %s"), strSuffix);;
        case POOL_STATE_ACCEPTING_ENTRIES:
            if (nEntriesCount == 0) {
                nStatusMessageProgress = 0;
                return strAutoDenomResult;
            } else if (fLastEntryAccepted) {
                if (nStatusMessageProgress % 10 > 8) {
                    fLastEntryAccepted = false;
                    nStatusMessageProgress = 0;
                }
                return _("PrivateSend request complete:") + " " + _("Your transaction was accepted into the pool!");
            } else {
                if (nStatusMessageProgress % 70 <= 40) return strprintf(_("Submitted following entries to ghostnode: %u / %d"), nEntriesCount, GetMaxPoolTransactions());
                else if (nStatusMessageProgress % 70 <= 50) strSuffix = ".";
                else if (nStatusMessageProgress % 70 <= 60) strSuffix = "..";
                else if (nStatusMessageProgress % 70 <= 70) strSuffix = "...";
                return strprintf(_("Submitted to ghostnode, waiting for more entries ( %u / %d ) %s"), nEntriesCount, GetMaxPoolTransactions(), strSuffix);
            }
        case POOL_STATE_SIGNING:
            if (nStatusMessageProgress % 70 <= 40) return _("Found enough users, signing ...");
            else if (nStatusMessageProgress % 70 <= 50) strSuffix = ".";
            else if (nStatusMessageProgress % 70 <= 60) strSuffix = "..";
            else if (nStatusMessageProgress % 70 <= 70) strSuffix = "...";
            return strprintf(_("Found enough users, signing ( waiting %s )"), strSuffix);
        case POOL_STATE_ERROR:
            return _("PrivateSend request incomplete:") + " " + strLastMessage + " " + _("Will retry...");
        case POOL_STATE_SUCCESS:
            return _("PrivateSend request complete:") + " " + strLastMessage;
        default:
            return strprintf(_("Unknown state: id = %u"), nState);
    }
}

//
// Check the mixing progress and send client updates if a Ghostnode
//
void CDarksendPool::CheckPool() {
    if (fGhostNode) {
        LogPrintf("privatesend", "CDarksendPool::CheckPool -- entries count %lu\n", GetEntriesCount());

        // If entries are full, create finalized transaction
        if (nState == POOL_STATE_ACCEPTING_ENTRIES && GetEntriesCount() >= GetMaxPoolTransactions()) {
            LogPrintf("privatesend", "CDarksendPool::CheckPool -- FINALIZE TRANSACTIONS\n");
            CreateFinalTransaction();
            return;
        }

        // If we have all of the signatures, try to compile the transaction
        if (nState == POOL_STATE_SIGNING && IsSignaturesComplete()) {
            LogPrintf("privatesend", "CDarksendPool::CheckPool -- SIGNING\n");
            CommitFinalTransaction();
            return;
        }
    }

    // reset if we're here for 10 seconds
    if ((nState == POOL_STATE_ERROR || nState == POOL_STATE_SUCCESS) && GetTimeMillis() - nTimeLastSuccessfulStep >= 10000) {
        LogPrintf("privatesend", "CDarksendPool::CheckPool -- timeout, RESETTING\n");
        UnlockCoins();
        SetNull();
    }
}

void CDarksendPool::CreateFinalTransaction() {
    LogPrintf("privatesend", "CDarksendPool::CreateFinalTransaction -- FINALIZE TRANSACTIONS\n");

    CMutableTransaction txNew;

    // make our new transaction
    for (int i = 0; i < GetEntriesCount(); i++) {

        for (unsigned int idx = 0; idx < vecEntries[i].vecTxDSOut.size(); idx++)
        {

            OUTPUT_PTR<CTxOutStandard> txOutStandard = MAKE_OUTPUT<CTxOutStandard>();
            txOutStandard->nValue = vecEntries[i].vecTxDSOut[idx].GetValue();
            txOutStandard->scriptPubKey = *vecEntries[i].vecTxDSOut[idx].GetPScriptPubKey();
            txNew.vpout.push_back(txOutStandard);

        }

        BOOST_FOREACH(
        const CTxDSIn &txdsin, vecEntries[i].vecTxDSIn)
        txNew.vin.push_back(txdsin);
    }


    // BIP69 https://github.com/kristovatlas/bips/blob/master/bip-0069.mediawiki
    sort(txNew.vin.begin(), txNew.vin.end());
    sort(txNew.vpout.begin(), txNew.vpout.end());

    finalMutableTransaction = txNew;
    LogPrintf("privatesend", "CDarksendPool::CreateFinalTransaction -- finalMutableTransaction=%s", txNew.ToString());

    // request signatures from clients
    RelayFinalTransaction(finalMutableTransaction);
    SetState(POOL_STATE_SIGNING);
}

void CDarksendPool::CommitFinalTransaction() {
    if (!fGhostNode) return; // check and relay final tx only on ghostnode

    CTransaction finalTransaction = CTransaction(finalMutableTransaction);
    uint256 hashTx = finalTransaction.GetHash();

    LogPrintf("privatesend", "CDarksendPool::CommitFinalTransaction -- finalTransaction=%s", finalTransaction.ToString());

    {
        // See if the transaction is valid
        TRY_LOCK(cs_main, lockMain);
        CValidationState validationState;
        mempool.PrioritiseTransaction(hashTx, 0.1 * COIN);
        CTransactionRef txRef(&finalTransaction);

        if (!lockMain || !AcceptToMemoryPool(mempool, validationState, txRef, nullptr, nullptr, true, 0)) {
            LogPrintf("CDarksendPool::CommitFinalTransaction -- AcceptToMemoryPool() error: Transaction not valid\n");
            SetNull();
            // not much we can do in this case, just notify clients
            RelayCompletedTransaction(ERR_INVALID_TX);
            return;
        }
    }

    LogPrintf("CDarksendPool::CommitFinalTransaction -- CREATING DSTX\n");

    // create and sign ghostnode dstx transaction
    if (!mapDarksendBroadcastTxes.count(hashTx)) {
        CTransactionRef tempRef(&finalTransaction);
        CDarksendBroadcastTx dstx(tempRef, activeGhostnode.vin, GetAdjustedTime());
        dstx.Sign();
        mapDarksendBroadcastTxes.insert(std::make_pair(hashTx, dstx));
    }

    LogPrintf("CDarksendPool::CommitFinalTransaction -- TRANSMITTING DSTX\n");

    CInv inv(MSG_DSTX, hashTx);
    g_connman->RelayInv(inv);

    // Tell the clients it was successful
    RelayCompletedTransaction(MSG_SUCCESS);

    // Randomly charge clients
    ChargeRandomFees();

    // Reset
    LogPrintf("privatesend", "CDarksendPool::CommitFinalTransaction -- COMPLETED -- RESETTING\n");
    SetNull();
}

//
// Charge clients a fee if they're abusive
//
// Why bother? PrivateSend uses collateral to ensure abuse to the process is kept to a minimum.
// The submission and signing stages are completely separate. In the cases where
// a client submits a transaction then refused to sign, there must be a cost. Otherwise they
// would be able to do this over and over again and bring the mixing to a hault.
//
// How does this work? Messages to Ghostnodes come in via NetMsgType::DSVIN, these require a valid collateral
// transaction for the client to be able to enter the pool. This transaction is kept by the Ghostnode
// until the transaction is either complete or fails.
//
void CDarksendPool::ChargeFees() {
    if (!fGhostNode) return;

    //we don't need to charge collateral for every offence.
    if (GetRandInt(100) > 33) return;

    std::vector <CTransactionRef> vecOffendersCollaterals;

    if (nState == POOL_STATE_ACCEPTING_ENTRIES) {
        BOOST_FOREACH(const CTransaction &txCollateral, vecSessionCollaterals) {
            bool fFound = false;
            BOOST_FOREACH(const CDarkSendEntry &entry, vecEntries){
                CTransactionRef txCollateralRef(&txCollateral);
                if (entry.txCollateral == txCollateralRef)
                    fFound = true;
            }

            // This queue entry didn't send us the promised transaction
            if (!fFound) {
                LogPrintf("CDarksendPool::ChargeFees -- found uncooperative node (didn't send transaction), found offence\n");
                CTransactionRef txCollateralRef(&txCollateral);
                vecOffendersCollaterals.push_back(txCollateralRef);
            }
        }
    }

    if (nState == POOL_STATE_SIGNING) {
        // who didn't sign?
        BOOST_FOREACH(
        const CDarkSendEntry entry, vecEntries) {
            BOOST_FOREACH(
            const CTxDSIn txdsin, entry.vecTxDSIn) {
                if (!txdsin.fHasSig) {
                    LogPrintf("CDarksendPool::ChargeFees -- found uncooperative node (didn't sign), found offence\n");
                    vecOffendersCollaterals.push_back(entry.txCollateral);
                }
            }
        }
    }

    // no offences found
    if (vecOffendersCollaterals.empty()) return;

    //mostly offending? Charge sometimes
    if ((int) vecOffendersCollaterals.size() >= Params().PoolMaxTransactions() - 1 && GetRandInt(100) > 33) return;

    //everyone is an offender? That's not right
    if ((int) vecOffendersCollaterals.size() >= Params().PoolMaxTransactions()) return;

    //charge one of the offenders randomly
    std::random_shuffle(vecOffendersCollaterals.begin(), vecOffendersCollaterals.end());

    if (nState == POOL_STATE_ACCEPTING_ENTRIES || nState == POOL_STATE_SIGNING) {
        LogPrintf("CDarksendPool::ChargeFees -- found uncooperative node (didn't %s transaction), charging fees: %s\n",
                  (nState == POOL_STATE_SIGNING) ? "sign" : "send", vecOffendersCollaterals[0]->ToString());

        LOCK(cs_main);

        CValidationState state;
        bool fMissingInputs;

        //CTransactionRef txRef(&vecOffendersCollaterals[0]);

        if (!AcceptToMemoryPool(mempool, state, vecOffendersCollaterals[0], &fMissingInputs, nullptr, false, 0)) {
            // should never really happen
            LogPrintf("CDarksendPool::ChargeFees -- ERROR: AcceptToMemoryPool failed!\n");
        } else {
            RelayTransaction(*vecOffendersCollaterals[0], g_connman.get());
        }
    }
}

/*
    Charge the collateral randomly.
    Mixing is completely free, to pay miners we randomly pay the collateral of users.

    Collateral Fee Charges:

    Being that mixing has "no fees" we need to have some kind of cost associated
    with using it to stop abuse. Otherwise it could serve as an attack vector and
    allow endless transaction that would bloat Dash and make it unusable. To
    stop these kinds of attacks 1 in 10 successful transactions are charged. This
    adds up to a cost of 0.001DRK per transaction on average.
*/
void CDarksendPool::ChargeRandomFees() {
    if (!fGhostNode) return;

    LOCK(cs_main);

    BOOST_FOREACH(
    const CTransaction &txCollateral, vecSessionCollaterals) {

        if (GetRandInt(100) > 10) return;

        LogPrintf("CDarksendPool::ChargeRandomFees -- charging random fees, txCollateral=%s", txCollateral.ToString());

        CValidationState state;
        bool fMissingInputs;
        CTransactionRef txRef(&txCollateral);
        if (!AcceptToMemoryPool(mempool, state, txRef , &fMissingInputs, nullptr, false, 0)) {
            // should never really happen
            LogPrintf("CDarksendPool::ChargeRandomFees -- ERROR: AcceptToMemoryPool failed!\n");
        } else {
            RelayTransaction(txCollateral, g_connman.get());
        }
    }
}

//
// Check for various timeouts (queue objects, mixing, etc)
//
void CDarksendPool::CheckTimeout() {
    {
        TRY_LOCK(cs_darksend, lockDS);
        if (!lockDS) return; // it's ok to fail here, we run this quite frequently

        // check mixing queue objects for timeouts
        std::vector<CDarksendQueue>::iterator it = vecDarksendQueue.begin();
        while (it != vecDarksendQueue.end()) {
            if ((*it).IsExpired()) {
                LogPrintf("privatesend", "CDarksendPool::CheckTimeout -- Removing expired queue (%s)\n", (*it).ToString());
                it = vecDarksendQueue.erase(it);
            } else ++it;
        }
    }

    if (!fEnablePrivateSend && !fGhostNode) return;

    // catching hanging sessions
    if (!fGhostNode) {
        switch (nState) {
            case POOL_STATE_ERROR:
                LogPrintf("privatesend", "CDarksendPool::CheckTimeout -- Pool error -- Running CheckPool\n");
                CheckPool();
                break;
            case POOL_STATE_SUCCESS:
                LogPrintf("privatesend", "CDarksendPool::CheckTimeout -- Pool success -- Running CheckPool\n");
                CheckPool();
                break;
            default:
                break;
        }
    }

    int nLagTime = fGhostNode ? 0 : 10000; // if we're the client, give the server a few extra seconds before resetting.
    int nTimeout = (nState == POOL_STATE_SIGNING) ? PRIVATESEND_SIGNING_TIMEOUT : PRIVATESEND_QUEUE_TIMEOUT;
    bool fTimeout = GetTimeMillis() - nTimeLastSuccessfulStep >= nTimeout * 1000 + nLagTime;

    if (nState != POOL_STATE_IDLE && fTimeout) {
        LogPrintf("privatesend", "CDarksendPool::CheckTimeout -- %s timed out (%ds) -- restting\n",
                 (nState == POOL_STATE_SIGNING) ? "Signing" : "Session", nTimeout);
        ChargeFees();
        UnlockCoins();
        SetNull();
        SetState(POOL_STATE_ERROR);
        strLastMessage = _("Session timed out.");
    }
}

/*
    Check to see if we're ready for submissions from clients
    After receiving multiple dsa messages, the queue will switch to "accepting entries"
    which is the active state right before merging the transaction
*/
void CDarksendPool::CheckForCompleteQueue() {
    if (!fEnablePrivateSend && !fGhostNode) return;

    if (nState == POOL_STATE_QUEUE && IsSessionReady()) {
        SetState(POOL_STATE_ACCEPTING_ENTRIES);

        CDarksendQueue dsq(nSessionDenom, activeGhostnode.vin, GetTime(), true);
        LogPrintf("privatesend", "CDarksendPool::CheckForCompleteQueue -- queue is ready, signing and relaying (%s)\n", dsq.ToString());
        dsq.Sign();
        dsq.Relay();
    }
}

// Check to make sure a given input matches an input in the pool and its scriptSig is valid
bool CDarksendPool::IsInputScriptSigValid(const CTxIn &txin) {
    CMutableTransaction txNew;
    txNew.vin.clear();
    txNew.vout.clear();

    int i = 0;
    int nTxInIndex = -1;
    CScript sigPubKey = CScript();

    BOOST_FOREACH(CDarkSendEntry & entry, vecEntries)
    {

        //BOOST_FOREACH(const CTxDSOut &txdsout, entry.vecTxDSOut)
        //txNew.vout.push_back(txdsout);

        for (unsigned int idx = 0; idx < entry.vecTxDSOut.size(); idx++)
        {

            OUTPUT_PTR<CTxOutStandard> txOutStandard = MAKE_OUTPUT<CTxOutStandard>();
            txOutStandard->nValue = entry.vecTxDSOut[idx].GetValue();
            txOutStandard->scriptPubKey = *entry.vecTxDSOut[idx].GetPScriptPubKey();
            txNew.vpout.push_back(txOutStandard);

        }

        BOOST_FOREACH(
        const CTxDSIn &txdsin, entry.vecTxDSIn) {
            txNew.vin.push_back(txdsin);

            if (txdsin.prevout == txin.prevout) {
                nTxInIndex = i;
                sigPubKey = txdsin.prevPubKey;
            }
            i++;
        }
    }

    if (nTxInIndex >= 0) { //might have to do this one input at a time?
        txNew.vin[nTxInIndex].scriptSig = txin.scriptSig;
        const CAmount &amount = txNew.vpout[nTxInIndex]->GetValue();
        LogPrintf("privatesend", "CDarksendPool::IsInputScriptSigValid -- verifying scriptSig %s\n", ScriptToAsmStr(txin.scriptSig).substr(0, 24));
//        if(!VerifyScript(txNew.vin[nTxInIndex].scriptSig, sigPubKey, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC, MutableTransactionSignatureChecker(&txNew, nTxInIndex, amount))) {
//            LogPrintf("privatesend", "CDarksendPool::IsInputScriptSigValid -- VerifyScript() failed on input %d\n", nTxInIndex);
//            return false;
//        }
    } else {
        LogPrintf("privatesend", "CDarksendPool::IsInputScriptSigValid -- Failed to find matching input in pool, %s\n", txin.ToString());
        return false;
    }

    LogPrintf("privatesend", "CDarksendPool::IsInputScriptSigValid -- Successfully validated input and scriptSig\n");
    return true;
}

// check to make sure the collateral provided by the client is valid
bool CDarksendPool::IsCollateralValid(const CTransaction &txCollateral) {
    if (txCollateral.vpout.empty()) return false;
    if (txCollateral.nLockTime != 0) return false;

    CAmount nValueIn = 0;
    CAmount nValueOut = 0;
    bool fMissingTx = false;

    /*
    BOOST_FOREACH( const CTxOut txout, txCollateral.vpout) {
        nValueOut += txout.nValue;

        if (!txout.scriptPubKey.IsNormalPaymentScript()) {
            LogPrintf ("CDarksendPool::IsCollateralValid -- Invalid Script, txCollateral=%s", txCollateral.ToString());
            return false;
        }
    }
    */

    for (unsigned int idx = 0; idx < txCollateral.vpout.size(); idx++)
    {

        OUTPUT_PTR<CTxOutStandard> txOutStandard = MAKE_OUTPUT<CTxOutStandard>();
        txOutStandard->nValue = txCollateral.vpout[idx]->GetValue();
        txOutStandard->scriptPubKey = *txCollateral.vpout[idx]->GetPScriptPubKey();
        nValueOut += txOutStandard->GetValue();
        if (!txOutStandard->GetPScriptPubKey()->IsNormalPaymentScript()) {
            LogPrintf ("CDarksendPool::IsCollateralValid -- Invalid Script, txCollateral=%s", txCollateral.ToString());
            return false;
        }

    }

    BOOST_FOREACH(
    const CTxIn txin, txCollateral.vin) {
        CTransactionRef txPrev;
        uint256 hash;
        if (GetTransaction(txin.prevout.hash, txPrev, Params().GetConsensus(), hash, true)) {
            if (txPrev->vpout.size() > txin.prevout.n)
                nValueIn += txPrev->vpout[txin.prevout.n]->GetValue();
        } else {
            fMissingTx = true;
        }
    }

    if (fMissingTx) {
        LogPrintf("privatesend", "CDarksendPool::IsCollateralValid -- Unknown inputs in collateral transaction, txCollateral=%s", txCollateral.ToString());
        return false;
    }

    //collateral transactions are required to pay out PRIVATESEND_COLLATERAL as a fee to the miners
    if (nValueIn - nValueOut < PRIVATESEND_COLLATERAL) {
        LogPrintf("privatesend", "CDarksendPool::IsCollateralValid -- did not include enough fees in transaction: fees: %d, txCollateral=%s", nValueOut - nValueIn, txCollateral.ToString());
        return false;
    }

    LogPrintf("privatesend", "CDarksendPool::IsCollateralValid -- %s", txCollateral.ToString());

    {
        LOCK(cs_main);
        CValidationState validationState;
        CTransactionRef txRef(&txCollateral);
        if (!AcceptToMemoryPool(mempool, validationState, txRef, nullptr, nullptr, false, 0)) {
            LogPrintf("privatesend", "CDarksendPool::IsCollateralValid -- didn't pass AcceptToMemoryPool()\n");
            return false;
        }
    }

    return true;
}


//
// Add a clients transaction to the pool
//
bool CDarksendPool::AddEntry(const CDarkSendEntry &entryNew, PoolMessage &nMessageIDRet) {
    if (!fGhostNode) return false;

    BOOST_FOREACH(CTxIn
    txin, entryNew.vecTxDSIn) {
        if (txin.prevout.IsNull()) {
            LogPrintf("privatesend", "CDarksendPool::AddEntry -- input not valid!\n");
            nMessageIDRet = ERR_INVALID_INPUT;
            return false;
        }
    }

    if (!IsCollateralValid(*entryNew.txCollateral)) {
        LogPrintf("privatesend", "CDarksendPool::AddEntry -- collateral not valid!\n");
        nMessageIDRet = ERR_INVALID_COLLATERAL;
        return false;
    }

    if (GetEntriesCount() >= GetMaxPoolTransactions()) {
        LogPrintf("privatesend", "CDarksendPool::AddEntry -- entries is full!\n");
        nMessageIDRet = ERR_ENTRIES_FULL;
        return false;
    }

    BOOST_FOREACH(CTxIn
    txin, entryNew.vecTxDSIn) {
        LogPrintf("privatesend", "looking for txin -- %s\n", txin.ToString());
        BOOST_FOREACH(
        const CDarkSendEntry &entry, vecEntries) {
            BOOST_FOREACH(
            const CTxDSIn &txdsin, entry.vecTxDSIn) {
                if (txdsin.prevout == txin.prevout) {
                    LogPrintf("privatesend", "CDarksendPool::AddEntry -- found in txin\n");
                    nMessageIDRet = ERR_ALREADY_HAVE;
                    return false;
                }
            }
        }
    }

    vecEntries.push_back(entryNew);

    LogPrintf("privatesend", "CDarksendPool::AddEntry -- adding entry\n");
    nMessageIDRet = MSG_ENTRIES_ADDED;
    nTimeLastSuccessfulStep = GetTimeMillis();

    return true;
}

bool CDarksendPool::AddScriptSig(const CTxIn &txinNew) {
    LogPrintf("privatesend", "CDarksendPool::AddScriptSig -- scriptSig=%s\n", ScriptToAsmStr(txinNew.scriptSig).substr(0, 24));

    BOOST_FOREACH(
    const CDarkSendEntry &entry, vecEntries) {
        BOOST_FOREACH(
        const CTxDSIn &txdsin, entry.vecTxDSIn) {
            if (txdsin.scriptSig == txinNew.scriptSig) {
                LogPrintf("privatesend", "CDarksendPool::AddScriptSig -- already exists\n");
                return false;
            }
        }
    }

    if (!IsInputScriptSigValid(txinNew)) {
        LogPrintf("privatesend", "CDarksendPool::AddScriptSig -- Invalid scriptSig\n");
        return false;
    }

    LogPrintf("privatesend", "CDarksendPool::AddScriptSig -- scriptSig=%s new\n", ScriptToAsmStr(txinNew.scriptSig).substr(0, 24));

    BOOST_FOREACH(CTxIn & txin, finalMutableTransaction.vin)
    {
        if (txinNew.prevout == txin.prevout && txin.nSequence == txinNew.nSequence) {
            txin.scriptSig = txinNew.scriptSig;
            txin.prevPubKey = txinNew.prevPubKey;
            LogPrintf("privatesend", "CDarksendPool::AddScriptSig -- adding to finalMutableTransaction, scriptSig=%s\n", ScriptToAsmStr(txinNew.scriptSig).substr(0, 24));
        }
    }
    for (int i = 0; i < GetEntriesCount(); i++) {
        if (vecEntries[i].AddScriptSig(txinNew)) {
            LogPrintf("privatesend", "CDarksendPool::AddScriptSig -- adding to entries, scriptSig=%s\n", ScriptToAsmStr(txinNew.scriptSig).substr(0, 24));
            return true;
        }
    }

    LogPrintf("CDarksendPool::AddScriptSig -- Couldn't set sig!\n");
    return false;
}

// Check to make sure everything is signed
bool CDarksendPool::IsSignaturesComplete() {
    BOOST_FOREACH(
    const CDarkSendEntry &entry, vecEntries)
    BOOST_FOREACH(
    const CTxDSIn &txdsin, entry.vecTxDSIn)
    if (!txdsin.fHasSig) return false;

    return true;
}

//
// Execute a mixing denomination via a Ghostnode.
// This is only ran from clients
//
bool CDarksendPool::SendDenominate(const std::vector <CTxIn> &vecTxIn, const std::vector<OUTPUT_PTR<CTxOutStandard>> & vecTxOut) {
    if (fGhostNode) {
        LogPrintf("CDarksendPool::SendDenominate -- PrivateSend from a Ghostnode is not supported currently.\n");
        return false;
    }

    if (txMyCollateral == CMutableTransaction()) {
        LogPrintf("CDarksendPool:SendDenominate -- PrivateSend collateral not set\n");
        return false;
    }

    // lock the funds we're going to use
    BOOST_FOREACH(CTxIn
    txin, txMyCollateral.vin)
    vecOutPointLocked.push_back(txin.prevout);

    BOOST_FOREACH(CTxIn
    txin, vecTxIn)
    vecOutPointLocked.push_back(txin.prevout);

    // we should already be connected to a Ghostnode
    if (!nSessionID) {
        LogPrintf("CDarksendPool::SendDenominate -- No Ghostnode has been selected yet.\n");
        UnlockCoins();
        SetNull();
        return false;
    }

    if (!CheckDiskSpace()) {
        UnlockCoins();
        SetNull();
        fEnablePrivateSend = false;
        LogPrintf("CDarksendPool::SendDenominate -- Not enough disk space, disabling PrivateSend.\n");
        return false;
    }

    SetState(POOL_STATE_ACCEPTING_ENTRIES);
    strLastMessage = "";

    LogPrintf("CDarksendPool::SendDenominate -- Added transaction to pool.\n");

    //check it against the memory pool to make sure it's valid
    {
        CValidationState validationState;
        CMutableTransaction tx;

        BOOST_FOREACH(
        const CTxIn &txin, vecTxIn) {
            LogPrintf("privatesend", "CDarksendPool::SendDenominate -- txin=%s\n", txin.ToString());
            tx.vin.push_back(txin);
        }

        /*
        BOOST_FOREACH(
        const CTxOut &txout, vecTxOut) {
            LogPrintf("privatesend", "CDarksendPool::SendDenominate -- txout=%s\n", txout.ToString());
            tx.vout.push_back(txout);
        }
        */

        for (unsigned int idx = 0; idx < vecTxOut.size(); idx++)
        {

            OUTPUT_PTR<CTxOutStandard> txOutStandard = MAKE_OUTPUT<CTxOutStandard>();
            txOutStandard->nValue = vecTxOut[idx]->GetValue();
            txOutStandard->scriptPubKey = *vecTxOut[idx]->GetPScriptPubKey();

            LogPrintf("privatesend", "CDarksendPool::SendDenominate -- txout=%s\n", txOutStandard->ToString());
            tx.vpout.push_back(txOutStandard);

        }

        LogPrintf("CDarksendPool::SendDenominate -- Submitting partial tx %s", tx.ToString());

        mempool.PrioritiseTransaction(tx.GetHash(), 0.1 * COIN);
        TRY_LOCK(cs_main, lockMain);

        const CTransaction txTemp(tx);
        CTransactionRef txRef(&txTemp);
        if (!lockMain || !AcceptToMemoryPool(mempool, validationState, txRef, nullptr, nullptr, false, 0)) {
            LogPrintf("CDarksendPool::SendDenominate -- AcceptToMemoryPool() failed! tx=%s", tx.ToString());
            UnlockCoins();
            SetNull();
            return false;
        }
    }
    CTransaction txMyCollateralTX(txMyCollateral);
    // store our entry for later use
    CDarkSendEntry entry(vecTxIn, vecTxOut, CTransactionRef(&txMyCollateralTX));
    vecEntries.push_back(entry);
    RelayIn(entry);
    nTimeLastSuccessfulStep = GetTimeMillis();

    return true;
}

// Incoming message from Ghostnode updating the progress of mixing
bool CDarksendPool::CheckPoolStateUpdate(PoolState nStateNew, int nEntriesCountNew, PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID, int nSessionIDNew) {
    if (fGhostNode) return false;

    // do not update state when mixing client state is one of these
    if (nState == POOL_STATE_IDLE || nState == POOL_STATE_ERROR || nState == POOL_STATE_SUCCESS) return false;

    strAutoDenomResult = _("Ghostnode:") + " " + GetMessageByID(nMessageID);

    // if rejected at any state
    if (nStatusUpdate == STATUS_REJECTED) {
        LogPrintf("CDarksendPool::CheckPoolStateUpdate -- entry is rejected by Ghostnode\n");
        UnlockCoins();
        SetNull();
        SetState(POOL_STATE_ERROR);
        strLastMessage = GetMessageByID(nMessageID);
        return true;
    }

    if (nStatusUpdate == STATUS_ACCEPTED && nState == nStateNew) {
        if (nStateNew == POOL_STATE_QUEUE && nSessionID == 0 && nSessionIDNew != 0) {
            // new session id should be set only in POOL_STATE_QUEUE state
            nSessionID = nSessionIDNew;
            nTimeLastSuccessfulStep = GetTimeMillis();
            LogPrintf("CDarksendPool::CheckPoolStateUpdate -- set nSessionID to %d\n", nSessionID);
            return true;
        } else if (nStateNew == POOL_STATE_ACCEPTING_ENTRIES && nEntriesCount != nEntriesCountNew) {
            nEntriesCount = nEntriesCountNew;
            nTimeLastSuccessfulStep = GetTimeMillis();
            fLastEntryAccepted = true;
            LogPrintf("CDarksendPool::CheckPoolStateUpdate -- new entry accepted!\n");
            return true;
        }
    }

    // only situations above are allowed, fail in any other case
    return false;
}

//
// After we receive the finalized transaction from the Ghostnode, we must
// check it to make sure it's what we want, then sign it if we agree.
// If we refuse to sign, it's possible we'll be charged collateral
//
bool CDarksendPool::SignFinalTransaction(const CTransaction &finalTransactionNew, CNode *pnode) {
    if (fGhostNode || pnode == NULL) return false;

    finalMutableTransaction = finalTransactionNew;
    LogPrintf("CDarksendPool::SignFinalTransaction -- finalMutableTransaction=%s", finalMutableTransaction.ToString());

    std::vector <CTxIn> sigs;

    //make sure my inputs/outputs are present, otherwise refuse to sign
    BOOST_FOREACH(
    const CDarkSendEntry entry, vecEntries) {
        BOOST_FOREACH(
        const CTxDSIn txdsin, entry.vecTxDSIn) {
            /* Sign my transaction and all outputs */
            int nMyInputIndex = -1;
            CScript prevPubKey = CScript();
            CTxIn txin = CTxIn();

            for (unsigned int i = 0; i < finalMutableTransaction.vin.size(); i++) {
                if (finalMutableTransaction.vin[i] == txdsin) {
                    nMyInputIndex = i;
                    prevPubKey = txdsin.prevPubKey;
                    txin = txdsin;
                }
            }

            if (nMyInputIndex >= 0) { //might have to do this one input at a time?
                int nFoundOutputsCount = 0;
                CAmount nValue1 = 0;
                CAmount nValue2 = 0;

                for (unsigned int i = 0; i < finalMutableTransaction.vpout.size(); i++) {
                    /*
                    BOOST_FOREACH(const CTxOut &txout, entry.vecTxDSOut) {
                        if (finalMutableTransaction.vout[i] == txout) {
                            nFoundOutputsCount++;
                            nValue1 += finalMutableTransaction.vout[i].nValue;
                        }
                    }
                    */
                    for (unsigned int idx = 0; idx < entry.vecTxDSOut.size(); idx++)
                    {
                        OUTPUT_PTR<CTxOutStandard> txOutStandard = MAKE_OUTPUT<CTxOutStandard>();
                        txOutStandard->nValue = entry.vecTxDSOut[idx].GetValue();
                        txOutStandard->scriptPubKey = *entry.vecTxDSOut[idx].GetPScriptPubKey();

                        if (finalMutableTransaction.vpout[i]->GetPScriptPubKey() == txOutStandard->GetPScriptPubKey() && finalMutableTransaction.vpout[i]->GetValue() == txOutStandard->GetValue()) {
                            nFoundOutputsCount++;
                            nValue1 += finalMutableTransaction.vpout[i]->GetValue();
                        }
                    }
                }

                /*
                BOOST_FOREACH(const CTxOut txout, entry.vecTxDSOut)
                nValue2 += txout.nValue;
                */

                for (unsigned int idx = 0; idx < entry.vecTxDSOut.size(); idx++)
                {
                    nValue2 = entry.vecTxDSOut[idx].GetValue();


                }
                int nTargetOuputsCount = entry.vecTxDSOut.size();
                if (nFoundOutputsCount < nTargetOuputsCount || nValue1 != nValue2) {
                    // in this case, something went wrong and we'll refuse to sign. It's possible we'll be charged collateral. But that's
                    // better then signing if the transaction doesn't look like what we wanted.
                    LogPrintf("CDarksendPool::SignFinalTransaction -- My entries are not correct! Refusing to sign: nFoundOutputsCount: %d, nTargetOuputsCount: %d\n", nFoundOutputsCount, nTargetOuputsCount);
                    UnlockCoins();
                    SetNull();

                    return false;
                }

                const CKeyStore &keystore = *GetHDWallet(GetHDWallet(vpwallets.front()));

                LogPrintf("privatesend", "CDarksendPool::SignFinalTransaction -- Signing my input %i\n", nMyInputIndex);
                //CAmount amount;
                std::vector<uint8_t> amount(8);
                if (!SignSignature(keystore, prevPubKey, finalMutableTransaction, nMyInputIndex, amount, int(SIGHASH_ALL | SIGHASH_ANYONECANPAY))) { // changes scriptSig
                    LogPrintf("privatesend", "CDarksendPool::SignFinalTransaction -- Unable to sign my own transaction!\n");
                    // not sure what to do here, it will timeout...?
                }

                sigs.push_back(finalMutableTransaction.vin[nMyInputIndex]);
                LogPrintf("privatesend", "CDarksendPool::SignFinalTransaction -- nMyInputIndex: %d, sigs.size(): %d, scriptSig=%s\n", nMyInputIndex, (int) sigs.size(), ScriptToAsmStr(finalMutableTransaction.vin[nMyInputIndex].scriptSig));
            }
        }
    }

    if (sigs.empty()) {
        LogPrintf("CDarksendPool::SignFinalTransaction -- can't sign anything!\n");
        UnlockCoins();
        SetNull();

        return false;
    }

    // push all of our signatures to the Ghostnode
    LogPrintf("CDarksendPool::SignFinalTransaction -- pushing sigs to the ghostnode, finalMutableTransaction=%s", finalMutableTransaction.ToString());

    const CNetMsgMaker msgMaker(pnode->GetSendVersion());
    g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::DSSIGNFINALTX, sigs));
    SetState(POOL_STATE_SIGNING);
    nTimeLastSuccessfulStep = GetTimeMillis();

    return true;
}

void CDarksendPool::NewBlock() {
    static int64_t nTimeNewBlockReceived = 0;

    //we we're processing lots of blocks, we'll just leave
    if (GetTime() - nTimeNewBlockReceived < 10) return;
    nTimeNewBlockReceived = GetTime();
    LogPrintf("privatesend", "CDarksendPool::NewBlock\n");

    CheckTimeout();
}

// mixing transaction was completed (failed or successful)
void CDarksendPool::CompletedTransaction(PoolMessage nMessageID) {
    if (fGhostNode) return;

    if (nMessageID == MSG_SUCCESS) {
        LogPrintf("CompletedTransaction -- success\n");
        nCachedLastSuccessBlock = pCurrentBlockIndex->nHeight;
    } else {
        LogPrintf("CompletedTransaction -- error\n");
    }
    UnlockCoins();
    SetNull();
    strLastMessage = GetMessageByID(nMessageID);
}

//
// Passively run mixing in the background to anonymize funds based on the given configuration.
//
bool CDarksendPool::DoAutomaticDenominating(bool fDryRun) {
    if (!fEnablePrivateSend || fGhostNode || !pCurrentBlockIndex) return false;
    if (!GetHDWallet(vpwallets.front()) || GetHDWallet(vpwallets.front())->IsLocked(true)) return false;
    if (nState != POOL_STATE_IDLE) return false;

    if (!ghostnodeSync.IsGhostnodeListSynced()) {
        strAutoDenomResult = _("Can't mix while sync in progress.");
        return false;
    }

    switch (nWalletBackups) {
        case 0:
            LogPrintf("privatesend", "CDarksendPool::DoAutomaticDenominating -- Automatic backups disabled, no mixing available.\n");
            strAutoDenomResult = _("Automatic backups disabled") + ", " + _("no mixing available.");
            fEnablePrivateSend = false; // stop mixing
            GetHDWallet(vpwallets.front())->nKeysLeftSinceAutoBackup = 0; // no backup, no "keys since last backup"
            return false;
        case -1:
            // Automatic backup failed, nothing else we can do until user fixes the issue manually.
            // There is no way to bring user attention in daemon mode so we just update status and
            // keep spaming if debug is on.
            LogPrintf("privatesend", "CDarksendPool::DoAutomaticDenominating -- ERROR! Failed to create automatic backup.\n");
            strAutoDenomResult = _("ERROR! Failed to create automatic backup") + ", " + _("see debug.log for details.");
            return false;
        case -2:
            // We were able to create automatic backup but keypool was not replenished because wallet is locked.
            // There is no way to bring user attention in daemon mode so we just update status and
            // keep spaming if debug is on.
            LogPrintf("privatesend", "CDarksendPool::DoAutomaticDenominating -- WARNING! Failed to create replenish keypool, please unlock your wallet to do so.\n");
            strAutoDenomResult = _("WARNING! Failed to replenish keypool, please unlock your wallet to do so.") + ", " + _("see debug.log for details.");
            return false;
    }

    if (GetHDWallet(vpwallets.front())->nKeysLeftSinceAutoBackup < PRIVATESEND_KEYS_THRESHOLD_STOP) {
        // We should never get here via mixing itself but probably smth else is still actively using keypool
        LogPrintf("privatesend", "CDarksendPool::DoAutomaticDenominating -- Very low number of keys left: %d, no mixing available.\n", GetHDWallet(vpwallets.front())->nKeysLeftSinceAutoBackup);
        strAutoDenomResult = strprintf(_("Very low number of keys left: %d") + ", " + _("no mixing available."), GetHDWallet(vpwallets.front())->nKeysLeftSinceAutoBackup);
        // It's getting really dangerous, stop mixing
        fEnablePrivateSend = false;
        return false;
    } else if (GetHDWallet(vpwallets.front())->nKeysLeftSinceAutoBackup < PRIVATESEND_KEYS_THRESHOLD_WARNING) {
        // Low number of keys left but it's still more or less safe to continue
        LogPrintf("privatesend", "CDarksendPool::DoAutomaticDenominating -- Very low number of keys left: %d\n", GetHDWallet(vpwallets.front())->nKeysLeftSinceAutoBackup);
        strAutoDenomResult = strprintf(_("Very low number of keys left: %d"), GetHDWallet(vpwallets.front())->nKeysLeftSinceAutoBackup);

        if (fCreateAutoBackups) {
            LogPrintf("privatesend", "CDarksendPool::DoAutomaticDenominating -- Trying to create new backup.\n");
            std::string warningString;
            std::string errorString;

            if (!AutoBackupWallet(GetHDWallet(vpwallets.front()), "", warningString, errorString)) {
                if (!warningString.empty()) {
                    // There were some issues saving backup but yet more or less safe to continue
                    LogPrintf("CDarksendPool::DoAutomaticDenominating -- WARNING! Something went wrong on automatic backup: %s\n", warningString);
                }
                if (!errorString.empty()) {
                    // Things are really broken
                    LogPrintf("CDarksendPool::DoAutomaticDenominating -- ERROR! Failed to create automatic backup: %s\n", errorString);
                    strAutoDenomResult = strprintf(_("ERROR! Failed to create automatic backup") + ": %s", errorString);
                    return false;
                }
            }
        } else {
            // Wait for someone else (e.g. GUI action) to create automatic backup for us
            return false;
        }
    }

    LogPrintf("privatesend", "CDarksendPool::DoAutomaticDenominating -- Keys left since latest backup: %d\n", GetHDWallet(vpwallets.front())->nKeysLeftSinceAutoBackup);

    if (GetEntriesCount() > 0) {
        strAutoDenomResult = _("Mixing in progress...");
        return false;
    }

    TRY_LOCK(cs_darksend, lockDS);
    if (!lockDS) {
        strAutoDenomResult = _("Lock is already in place.");
        return false;
    }

    if (!fDryRun && GetHDWallet(vpwallets.front())->IsLocked(true)) {
        strAutoDenomResult = _("Wallet is locked.");
        return false;
    }

    if (!fPrivateSendMultiSession && pCurrentBlockIndex->nHeight - nCachedLastSuccessBlock < nMinBlockSpacing) {
        LogPrintf("CDarksendPool::DoAutomaticDenominating -- Last successful PrivateSend action was too recent\n");
        strAutoDenomResult = _("Last successful PrivateSend action was too recent.");
        return false;
    }

    if (mnodeman.size() == 0) {
        LogPrintf("privatesend", "CDarksendPool::DoAutomaticDenominating -- No Ghostnodes detected\n");
        strAutoDenomResult = _("No Ghostnodes detected.");
        return false;
    }

    CAmount nValueMin = vecPrivateSendDenominations.back();

    // if there are no confirmed DS collateral inputs yet
    if (!GetHDWallet(vpwallets.front())->HasCollateralInputs()) {
        // should have some additional amount for them
        nValueMin += PRIVATESEND_COLLATERAL * 4;
    }

    // including denoms but applying some restrictions
    CAmount nBalanceNeedsAnonymized = GetHDWallet(vpwallets.front())->GetNeedsToBeAnonymizedBalance(nValueMin);

    // anonymizable balance is way too small
    if (nBalanceNeedsAnonymized < nValueMin) {
        LogPrintf("CDarksendPool::DoAutomaticDenominating -- Not enough funds to anonymize\n");
        strAutoDenomResult = _("Not enough funds to anonymize.");
        return false;
    }

    // excluding denoms
    CAmount nBalanceAnonimizableNonDenom = GetHDWallet(vpwallets.front())->GetAnonymizableBalance(true);
    // denoms
    CAmount nBalanceDenominatedConf = GetHDWallet(vpwallets.front())->GetDenominatedBalance();
    CAmount nBalanceDenominatedUnconf = GetHDWallet(vpwallets.front())->GetDenominatedBalance(true);
    CAmount nBalanceDenominated = nBalanceDenominatedConf + nBalanceDenominatedUnconf;

    LogPrintf("privatesend", "CDarksendPool::DoAutomaticDenominating -- nValueMin: %f, nBalanceNeedsAnonymized: %f, nBalanceAnonimizableNonDenom: %f, nBalanceDenominatedConf: %f, nBalanceDenominatedUnconf: %f, nBalanceDenominated: %f\n",
             (float) nValueMin / COIN,
             (float) nBalanceNeedsAnonymized / COIN,
             (float) nBalanceAnonimizableNonDenom / COIN,
             (float) nBalanceDenominatedConf / COIN,
             (float) nBalanceDenominatedUnconf / COIN,
             (float) nBalanceDenominated / COIN);

    if (fDryRun) return true;

    // Check if we have should create more denominated inputs i.e.
    // there are funds to denominate and denominated balance does not exceed
    // max amount to mix yet.
    if (nBalanceAnonimizableNonDenom >= nValueMin + PRIVATESEND_COLLATERAL && nBalanceDenominated < nPrivateSendAmount * COIN)
        return CreateDenominated();

    //check if we have the collateral sized inputs
    if (!GetHDWallet(vpwallets.front())->HasCollateralInputs())
        return !GetHDWallet(vpwallets.front())->HasCollateralInputs(false) && MakeCollateralAmounts();

    if (nSessionID) {
        strAutoDenomResult = _("Mixing in progress...");
        return false;
    }

    // Initial phase, find a Ghostnode
    // Clean if there is anything left from previous session
    UnlockCoins();
    SetNull();

    // should be no unconfirmed denoms in non-multi-session mode
    if (!fPrivateSendMultiSession && nBalanceDenominatedUnconf > 0) {
        LogPrintf("CDarksendPool::DoAutomaticDenominating -- Found unconfirmed denominated outputs, will wait till they confirm to continue.\n");
        strAutoDenomResult = _("Found unconfirmed denominated outputs, will wait till they confirm to continue.");
        return false;
    }

    //check our collateral and create new if needed
    std::string strReason;
    if (txMyCollateral == CMutableTransaction()) {
        if (!GetHDWallet(vpwallets.front())->CreateCollateralTransaction(txMyCollateral, strReason)) {
            LogPrintf("CDarksendPool::DoAutomaticDenominating -- create collateral error:%s\n", strReason);
            return false;
        }
    } else {
        if (!IsCollateralValid(txMyCollateral)) {
            LogPrintf("CDarksendPool::DoAutomaticDenominating -- invalid collateral, recreating...\n");
            if (!GetHDWallet(vpwallets.front())->CreateCollateralTransaction(txMyCollateral, strReason)) {
                LogPrintf("CDarksendPool::DoAutomaticDenominating -- create collateral error: %s\n", strReason);
                return false;
            }
        }
    }

    int nMnCountEnabled = mnodeman.CountEnabled(MIN_PRIVATESEND_PEER_PROTO_VERSION);

    // If we've used 90% of the Ghostnode list then drop the oldest first ~30%
    int nThreshold_high = nMnCountEnabled * 0.9;
    int nThreshold_low = nThreshold_high * 0.7;
    LogPrintf("privatesend", "Checking vecGhostnodesUsed: size: %d, threshold: %d\n", (int) vecGhostnodesUsed.size(), nThreshold_high);

    if ((int) vecGhostnodesUsed.size() > nThreshold_high) {
        vecGhostnodesUsed.erase(vecGhostnodesUsed.begin(), vecGhostnodesUsed.begin() + vecGhostnodesUsed.size() - nThreshold_low);
        LogPrintf("privatesend", "  vecGhostnodesUsed: new size: %d, threshold: %d\n", (int) vecGhostnodesUsed.size(), nThreshold_high);
    }

    bool fUseQueue = GetRandInt(100) > 33;
    // don't use the queues all of the time for mixing unless we are a liquidity provider
    if (nLiquidityProvider || fUseQueue) {

        // Look through the queues and see if anything matches
        BOOST_FOREACH(CDarksendQueue & dsq, vecDarksendQueue)
        {
            // only try each queue once
            if (dsq.fTried) continue;
            dsq.fTried = true;

            if (dsq.IsExpired()) continue;

            CGhostnode *pmn = mnodeman.Find(dsq.vin);
            if (pmn == NULL) {
                LogPrintf("CDarksendPool::DoAutomaticDenominating -- dsq ghostnode is not in ghostnode list, ghostnode=%s\n", dsq.vin.prevout.ToStringShort());
                continue;
            }

            if (pmn->nProtocolVersion < MIN_PRIVATESEND_PEER_PROTO_VERSION) continue;

            std::vector<int> vecBits;
            if (!GetDenominationsBits(dsq.nDenom, vecBits)) {
                // incompatible denom
                continue;
            }

            // mixing rate limit i.e. nLastDsq check should already pass in DSQUEUE ProcessMessage
            // in order for dsq to get into vecDarksendQueue, so we should be safe to mix already,
            // no need for additional verification here

            LogPrintf("privatesend", "CDarksendPool::DoAutomaticDenominating -- found valid queue: %s\n", dsq.ToString());

            CAmount nValueInTmp = 0;
            std::vector <CTxIn> vecTxInTmp;
            std::vector <COutput> vCoinsTmp;

            // Try to match their denominations if possible, select at least 1 denominations
            if (!GetHDWallet(vpwallets.front())->SelectCoinsByDenominations(dsq.nDenom, vecPrivateSendDenominations[vecBits.front()], nBalanceNeedsAnonymized, vecTxInTmp, vCoinsTmp, nValueInTmp, 0, nPrivateSendRounds)) {
                LogPrintf("CDarksendPool::DoAutomaticDenominating -- Couldn't match denominations %d %d (%s)\n", vecBits.front(), dsq.nDenom, GetDenominationsToString(dsq.nDenom));
                continue;
            }

            vecGhostnodesUsed.push_back(dsq.vin);

            CNode *pnodeFound = NULL;
            {
                LOCK(g_connman->cs_vNodes);
                pnodeFound = g_connman->FindNode(pmn->addr);
                if (pnodeFound) {
                    if (pnodeFound->fDisconnect) {
                        continue;
                    } else {
                        pnodeFound->AddRef();
                    }
                }
            }

            LogPrintf("CDarksendPool::DoAutomaticDenominating -- attempt to connect to ghostnode from queue, addr=%s\n", pmn->addr.ToString());
            // connect to Ghostnode and submit the queue request

            CNode *pnode = (pnodeFound && pnodeFound->fGhostnode) ? pnodeFound : g_connman->ConnectNode(CAddress(pmn->addr, NODE_NETWORK), NULL, false, true);
            if (pnode) {
                pSubmittedToGhostnode = pmn;
                nSessionDenom = dsq.nDenom;

                const CNetMsgMaker msgMaker(pnode->GetSendVersion());
                g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::DSACCEPT, nSessionDenom, txMyCollateral));
                LogPrintf("CDarksendPool::DoAutomaticDenominating -- connected (from queue), sending DSACCEPT: nSessionDenom: %d (%s), addr=%s\n",
                          nSessionDenom, GetDenominationsToString(nSessionDenom), pnode->addr.ToString());
                strAutoDenomResult = _("Mixing in progress...");
                SetState(POOL_STATE_QUEUE);
                nTimeLastSuccessfulStep = GetTimeMillis();
                if (pnodeFound) {
                    pnodeFound->Release();
                }
                return true;
            } else {
                LogPrintf("CDarksendPool::DoAutomaticDenominating -- can't connect, addr=%s\n", pmn->addr.ToString());
                strAutoDenomResult = _("Error connecting to Ghostnode.");
                continue;
            }
        }
    }

    // do not initiate queue if we are a liquidity provider to avoid useless inter-mixing
    if (nLiquidityProvider) return false;

    int nTries = 0;

    // ** find the coins we'll use
    std::vector <CTxIn> vecTxIn;
    CAmount nValueInTmp = 0;
    if (!GetHDWallet(vpwallets.front())->SelectCoinsDark(nValueMin, nBalanceNeedsAnonymized, vecTxIn, nValueInTmp, 0, nPrivateSendRounds)) {
        // this should never happen
        LogPrintf("CDarksendPool::DoAutomaticDenominating -- Can't mix: no compatible inputs found!\n");
        strAutoDenomResult = _("Can't mix: no compatible inputs found!");
        return false;
    }

    // otherwise, try one randomly
    while (nTries < 10) {
        CGhostnode *pmn = mnodeman.FindRandomNotInVec(vecGhostnodesUsed, MIN_PRIVATESEND_PEER_PROTO_VERSION);
        if (pmn == NULL) {
            LogPrintf("CDarksendPool::DoAutomaticDenominating -- Can't find random ghostnode!\n");
            strAutoDenomResult = _("Can't find random Ghostnode.");
            return false;
        }
        vecGhostnodesUsed.push_back(pmn->vin);

        if (pmn->nLastDsq != 0 && pmn->nLastDsq + nMnCountEnabled / 5 > mnodeman.nDsqCount) {
            LogPrintf("CDarksendPool::DoAutomaticDenominating -- Too early to mix on this ghostnode!"
                              " ghostnode=%s  addr=%s  nLastDsq=%d  CountEnabled/5=%d  nDsqCount=%d\n",
                      pmn->vin.prevout.ToStringShort(), pmn->addr.ToString(), pmn->nLastDsq,
                      nMnCountEnabled / 5, mnodeman.nDsqCount);
            nTries++;
            continue;
        }

        CNode *pnodeFound = NULL;
        {
            LOCK(g_connman->cs_vNodes);
            pnodeFound = g_connman->FindNode(pmn->addr);
            if (pnodeFound) {
                if (pnodeFound->fDisconnect) {
                    nTries++;
                    continue;
                } else {
                    pnodeFound->AddRef();
                }
            }
        }

        LogPrintf("CDarksendPool::DoAutomaticDenominating -- attempt %d connection to Ghostnode %s\n", nTries, pmn->addr.ToString());
        CNode *pnode = (pnodeFound && pnodeFound->fGhostnode) ? pnodeFound : g_connman->ConnectNode(CAddress(pmn->addr, NODE_NETWORK), NULL, false, true);
        if (pnode) {
            LogPrintf("CDarksendPool::DoAutomaticDenominating -- connected, addr=%s\n", pmn->addr.ToString());
            pSubmittedToGhostnode = pmn;

            std::vector <CAmount> vecAmounts;
            GetHDWallet(vpwallets.front())->ConvertList(vecTxIn, vecAmounts);
            // try to get a single random denom out of vecAmounts
            while (nSessionDenom == 0) {
                nSessionDenom = GetDenominationsByAmounts(vecAmounts);
            }

            const CNetMsgMaker msgMaker(pnode->GetSendVersion());
            g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::DSACCEPT, nSessionDenom, txMyCollateral));
            LogPrintf("CDarksendPool::DoAutomaticDenominating -- connected, sending DSACCEPT, nSessionDenom: %d (%s)\n",
                      nSessionDenom, GetDenominationsToString(nSessionDenom));
            strAutoDenomResult = _("Mixing in progress...");
            SetState(POOL_STATE_QUEUE);
            nTimeLastSuccessfulStep = GetTimeMillis();
            if (pnodeFound) {
                pnodeFound->Release();
            }
            return true;
        } else {
            LogPrintf("CDarksendPool::DoAutomaticDenominating -- can't connect, addr=%s\n", pmn->addr.ToString());
            nTries++;
            continue;
        }
    }

    strAutoDenomResult = _("No compatible Ghostnode found.");
    return false;
}

bool CDarksendPool::SubmitDenominate() {
    std::string strError;
    std::vector <CTxIn> vecTxInRet;
    std::vector<OUTPUT_PTR<CTxOutStandard>> vecTxOutRet;

    // Submit transaction to the pool if we get here
    // Try to use only inputs with the same number of rounds starting from lowest number of rounds possible
    for (int i = 0; i < nPrivateSendRounds; i++) {
        if (PrepareDenominate(i, i + 1, strError, vecTxInRet, vecTxOutRet)) {
            LogPrintf("CDarksendPool::SubmitDenominate -- Running PrivateSend denominate for %d rounds, success\n", i);
            return SendDenominate(vecTxInRet, vecTxOutRet);
        }
        LogPrintf("CDarksendPool::SubmitDenominate -- Running PrivateSend denominate for %d rounds, error: %s\n", i, strError);
    }

    // We failed? That's strange but let's just make final attempt and try to mix everything
    if (PrepareDenominate(0, nPrivateSendRounds, strError, vecTxInRet, vecTxOutRet)) {
        LogPrintf("CDarksendPool::SubmitDenominate -- Running PrivateSend denominate for all rounds, success\n");
        return SendDenominate(vecTxInRet, vecTxOutRet);
    }

    // Should never actually get here but just in case
    LogPrintf("CDarksendPool::SubmitDenominate -- Running PrivateSend denominate for all rounds, error: %s\n", strError);
    strAutoDenomResult = strError;
    return false;
}

bool CDarksendPool::PrepareDenominate(int nMinRounds, int nMaxRounds, std::string &strErrorRet, std::vector <CTxIn> &vecTxInRet, std::vector<OUTPUT_PTR<CTxOutStandard>> & vecTxOutRet) {
    if (GetHDWallet(vpwallets.front())->IsLocked(true)) {
        strErrorRet = "Wallet locked, unable to create transaction!";
        return false;
    }

    if (GetEntriesCount() > 0) {
        strErrorRet = "Already have pending entries in the PrivateSend pool";
        return false;
    }

    // make sure returning vectors are empty before filling them up
    vecTxInRet.clear();
    vecTxOutRet.clear();

    // ** find the coins we'll use
    std::vector <CTxIn> vecTxIn;
    std::vector <COutput> vCoins;
    CAmount nValueIn = 0;
    CReserveKey reservekey(GetHDWallet(vpwallets.front()));

    /*
        Select the coins we'll use

        if nMinRounds >= 0 it means only denominated inputs are going in and coming out
    */
    std::vector<int> vecBits;
    if (!GetDenominationsBits(nSessionDenom, vecBits)) {
        strErrorRet = "Incorrect session denom";
        return false;
    }
    bool fSelected = GetHDWallet(vpwallets.front())->SelectCoinsByDenominations(nSessionDenom, vecPrivateSendDenominations[vecBits.front()], PRIVATESEND_POOL_MAX, vecTxIn, vCoins, nValueIn, nMinRounds, nMaxRounds);
    if (nMinRounds >= 0 && !fSelected) {
        strErrorRet = "Can't select current denominated inputs";
        return false;
    }

    LogPrintf("CDarksendPool::PrepareDenominate -- max value: %f\n", (double) nValueIn / COIN);

    {
        LOCK(GetHDWallet(vpwallets.front())->cs_wallet);
        BOOST_FOREACH(CTxIn
        txin, vecTxIn) {
            GetHDWallet(vpwallets.front())->LockCoin(txin.prevout);
        }
    }

    CAmount nValueLeft = nValueIn;

    // Try to add every needed denomination, repeat up to 5-9 times.
    // NOTE: No need to randomize order of inputs because they were
    // initially shuffled in CWallet::SelectCoinsByDenominations already.
    int nStep = 0;
    int nStepsMax = 5 + GetRandInt(5);

    while (nStep < nStepsMax) {
        BOOST_FOREACH(int
        nBit, vecBits) {
            CAmount nValueDenom = vecPrivateSendDenominations[nBit];
            if (nValueLeft - nValueDenom < 0) continue;

            // Note: this relies on a fact that both vectors MUST have same size
            std::vector<CTxIn>::iterator it = vecTxIn.begin();
            std::vector<COutput>::iterator it2 = vCoins.begin();
            while (it2 != vCoins.end()) {
                // we have matching inputs
                if ((*it2).tx->tx->vpout[(*it2).i]->GetValue() == nValueDenom) {
                    // add new input in resulting vector
                    vecTxInRet.push_back(*it);
                    // remove corresponting items from initial vectors
                    vecTxIn.erase(it);
                    vCoins.erase(it2);

                    CScript scriptChange;
                    CPubKey vchPubKey;
                    // use a unique change address
                    assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
                    scriptChange = GetScriptForDestination(vchPubKey.GetID());
                    reservekey.KeepKey();

                    // add new output
                    //CTxOut txout(nValueDenom, scriptChange);

                    OUTPUT_PTR<CTxOutStandard> txoutstandard = MAKE_OUTPUT<CTxOutStandard>();
                    txoutstandard->nValue = nValueDenom;
                    txoutstandard->scriptPubKey = scriptChange;
                    vecTxOutRet.push_back(txoutstandard);

                    // subtract denomination amount
                    nValueLeft -= nValueDenom;

                    // step is complete
                    break;
                }
                ++it;
                ++it2;
            }
        }
        if (nValueLeft == 0) break;
        nStep++;
    }

    {
        // unlock unused coins
        LOCK(GetHDWallet(vpwallets.front())->cs_wallet);
        BOOST_FOREACH(CTxIn
        txin, vecTxIn) {
            GetHDWallet(vpwallets.front())->UnlockCoin(txin.prevout);
        }
    }

    if (GetDenominations(vecTxOutRet) != nSessionDenom) {
        // unlock used coins on failure
        LOCK(GetHDWallet(vpwallets.front())->cs_wallet);
        BOOST_FOREACH(CTxIn
        txin, vecTxInRet) {
            GetHDWallet(vpwallets.front())->UnlockCoin(txin.prevout);
        }
        strErrorRet = "Can't make current denominated outputs";
        return false;
    }

    // We also do not care about full amount as long as we have right denominations
    return true;
}

// Create collaterals by looping through inputs grouped by addresses
bool CDarksendPool::MakeCollateralAmounts() {
    std::vector <CompactTallyItem> vecTally;
    if (!GetHDWallet(vpwallets.front())->SelectCoinsGrouppedByAddresses(vecTally, false)) {
        LogPrintf("privatesend", "CDarksendPool::MakeCollateralAmounts -- SelectCoinsGrouppedByAddresses can't find any inputs!\n");
        return false;
    }

    BOOST_FOREACH(CompactTallyItem & item, vecTally)
    {
        if (!MakeCollateralAmounts(item)) continue;
        return true;
    }

    LogPrintf("CDarksendPool::MakeCollateralAmounts -- failed!\n");
    return false;
}

// Split up large inputs or create fee sized inputs
bool CDarksendPool::MakeCollateralAmounts(const CompactTallyItem &tallyItem) {
    CWalletTx wtx;
    CAmount nFeeRet = 0;
    int nChangePosRet = -1;
    std::string strFail = "";
    std::vector <CRecipient> vecSend;

    // make our collateral address
    CReserveKey reservekeyCollateral(GetHDWallet(vpwallets.front()));
    // make our change address
    CReserveKey reservekeyChange(GetHDWallet(vpwallets.front()));

    CScript scriptCollateral;
    CPubKey vchPubKey;
    assert(reservekeyCollateral.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptCollateral = GetScriptForDestination(vchPubKey.GetID());

    vecSend.push_back((CRecipient) {scriptCollateral, PRIVATESEND_COLLATERAL * 4, false});

    // try to use non-denominated and not mn-like funds first, select them explicitly
    CCoinControl coinControl;
    coinControl.fAllowOtherInputs = false;
    coinControl.fAllowWatchOnly = false;
    // send change to the same address so that we were able create more denoms out of it later
    coinControl.destChange = tallyItem.address.Get();
    BOOST_FOREACH(
    const CTxIn &txin, tallyItem.vecTxIn)
    coinControl.Select(txin.prevout);
    //TODO
    //bool fSuccess = pwalletMain->CreateTransaction(vecSend, wtx, reservekeyChange, nFeeRet, nChangePosRet, strFail, &coinControl, true, ONLY_NONDENOMINATED_NOT1000IFMN);
    bool fSuccess = false;
    if (!fSuccess) {
        // if we failed (most likeky not enough funds), try to use all coins instead -
        // MN-like funds should not be touched in any case and we can't mix denominated without collaterals anyway
        LogPrintf("CDarksendPool::MakeCollateralAmounts -- ONLY_NONDENOMINATED_NOT1000IFMN Error: %s\n", strFail);
        CCoinControl *coinControlNull = NULL;
        //TODO
//        fSuccess = pwalletMain->CreateTransaction(vecSend, wtx, reservekeyChange, nFeeRet, nChangePosRet, strFail, coinControlNull, true, ONLY_NOT1000IFMN);
        fSuccess = false;
        if (!fSuccess) {
            LogPrintf("CDarksendPool::MakeCollateralAmounts -- ONLY_NOT1000IFMN Error: %s\n", strFail);
            reservekeyCollateral.ReturnKey();
            return false;
        }
    }

    reservekeyCollateral.KeepKey();

    LogPrintf("CDarksendPool::MakeCollateralAmounts -- txid=%s\n", wtx.GetHash().GetHex());

    // use the same nCachedLastSuccessBlock as for DS mixinx to prevent race
    CValidationState state;
    if (!GetHDWallet(vpwallets.front())->CommitTransaction(wtx, reservekeyChange, g_connman.get(), state)) {
        LogPrintf("CDarksendPool::MakeCollateralAmounts -- CommitTransaction failed!\n");
        return false;
    }

    nCachedLastSuccessBlock = pCurrentBlockIndex->nHeight;

    return true;
}

// Create denominations by looping through inputs grouped by addresses
bool CDarksendPool::CreateDenominated() {
    std::vector <CompactTallyItem> vecTally;
    if (!GetHDWallet(vpwallets.front())->SelectCoinsGrouppedByAddresses(vecTally)) {
        LogPrintf("privatesend", "CDarksendPool::CreateDenominated -- SelectCoinsGrouppedByAddresses can't find any inputs!\n");
        return false;
    }

    bool fCreateMixingCollaterals = !GetHDWallet(vpwallets.front())->HasCollateralInputs();

    BOOST_FOREACH(CompactTallyItem & item, vecTally)
    {
        if (!CreateDenominated(item, fCreateMixingCollaterals)) continue;
        return true;
    }

    LogPrintf("CDarksendPool::CreateDenominated -- failed!\n");
    return false;
}

// Create denominations
bool CDarksendPool::CreateDenominated(const CompactTallyItem &tallyItem, bool fCreateMixingCollaterals) {
    std::vector <CRecipient> vecSend;
    CAmount nValueLeft = tallyItem.nAmount;
    nValueLeft -= PRIVATESEND_COLLATERAL; // leave some room for fees

    LogPrintf("CreateDenominated0 nValueLeft: %f\n", (float) nValueLeft / COIN);
    // make our collateral address
    CReserveKey reservekeyCollateral(GetHDWallet(vpwallets.front()));

    CScript scriptCollateral;
    CPubKey vchPubKey;
    assert(reservekeyCollateral.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptCollateral = GetScriptForDestination(vchPubKey.GetID());

    // ****** Add collateral outputs ************ /

    if (fCreateMixingCollaterals) {
        vecSend.push_back((CRecipient) {scriptCollateral, PRIVATESEND_COLLATERAL * 4, false});
        nValueLeft -= PRIVATESEND_COLLATERAL * 4;
    }

    // ****** Add denoms ************ /

    // make our denom addresses
    CReserveKey reservekeyDenom(GetHDWallet(vpwallets.front()));

    // try few times - skipping smallest denoms first if there are too much already, if failed - use them
    int nOutputsTotal = 0;
    bool fSkip = true;
    do {

        BOOST_REVERSE_FOREACH(CAmount
        nDenomValue, vecPrivateSendDenominations) {

            if (fSkip) {
                // Note: denoms are skipped if there are already DENOMS_COUNT_MAX of them
                // and there are still larger denoms which can be used for mixing

                // check skipped denoms
                if (IsDenomSkipped(nDenomValue)) continue;

                // find new denoms to skip if any (ignore the largest one)
                if (nDenomValue != vecPrivateSendDenominations[0] && GetHDWallet(vpwallets.front())->CountInputsWithAmount(nDenomValue) > DENOMS_COUNT_MAX) {
                    strAutoDenomResult = strprintf(_("Too many %f denominations, removing."), (float) nDenomValue / COIN);
                    LogPrintf("CDarksendPool::CreateDenominated -- %s\n", strAutoDenomResult);
                    vecDenominationsSkipped.push_back(nDenomValue);
                    continue;
                }
            }

            int nOutputs = 0;

            // add each output up to 10 times until it can't be added again
            while (nValueLeft - nDenomValue >= 0 && nOutputs <= 10) {
                CScript scriptDenom;
                CPubKey vchPubKey;
                //use a unique change address
                assert(reservekeyDenom.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
                scriptDenom = GetScriptForDestination(vchPubKey.GetID());
                // TODO: do not keep reservekeyDenom here
                reservekeyDenom.KeepKey();

                vecSend.push_back((CRecipient) {scriptDenom, nDenomValue, false});

                //increment outputs and subtract denomination amount
                nOutputs++;
                nValueLeft -= nDenomValue;
                LogPrintf("CreateDenominated1: nOutputsTotal: %d, nOutputs: %d, nValueLeft: %f\n", nOutputsTotal, nOutputs, (float) nValueLeft / COIN);
            }

            nOutputsTotal += nOutputs;
            if (nValueLeft == 0) break;
        }
        LogPrintf("CreateDenominated2: nOutputsTotal: %d, nValueLeft: %f\n", nOutputsTotal, (float) nValueLeft / COIN);
        // if there were no outputs added, start over without skipping
        fSkip = !fSkip;
    } while (nOutputsTotal == 0 && !fSkip);
    LogPrintf("CreateDenominated3: nOutputsTotal: %d, nValueLeft: %f\n", nOutputsTotal, (float) nValueLeft / COIN);

    // if we have anything left over, it will be automatically send back as change - there is no need to send it manually

    CCoinControl coinControl;
    coinControl.fAllowOtherInputs = false;
    coinControl.fAllowWatchOnly = false;
    // send change to the same address so that we were able create more denoms out of it later
    coinControl.destChange = tallyItem.address.Get();
    BOOST_FOREACH(
    const CTxIn &txin, tallyItem.vecTxIn)
    coinControl.Select(txin.prevout);

    CWalletTx wtx;
    CAmount nFeeRet = 0;
    int nChangePosRet = -1;
    std::string strFail = "";
    // make our change address
    CReserveKey reservekeyChange(GetHDWallet(vpwallets.front()));
    //TODO
//    bool fSuccess = pwalletMain->CreateTransaction(vecSend, wtx, reservekeyChange, nFeeRet, nChangePosRet, strFail, &coinControl, true, ONLY_NONDENOMINATED_NOT1000IFMN);
    bool fSuccess = false;
    if (!fSuccess) {
        LogPrintf("CDarksendPool::CreateDenominated -- Error: %s\n", strFail);
        // TODO: return reservekeyDenom here
        reservekeyCollateral.ReturnKey();
        return false;
    }

    // TODO: keep reservekeyDenom here
    reservekeyCollateral.KeepKey();
    CValidationState state;
    if (!GetHDWallet(vpwallets.front())->CommitTransaction(wtx, reservekeyChange, g_connman.get(), state)) {
        LogPrintf("CDarksendPool::CreateDenominated -- CommitTransaction failed!\n");
        return false;
    }

    // use the same nCachedLastSuccessBlock as for DS mixing to prevent race
    nCachedLastSuccessBlock = pCurrentBlockIndex->nHeight;
    LogPrintf("CDarksendPool::CreateDenominated -- txid=%s\n", wtx.GetHash().GetHex());

    return true;
}

bool CDarksendPool::IsOutputsCompatibleWithSessionDenom(const std::vector <CTxDSOut> &vecTxDSOut) {
    if (GetDenominations(vecTxDSOut) == 0) return false;

    BOOST_FOREACH(
    const CDarkSendEntry entry, vecEntries) {
        LogPrintf("CDarksendPool::IsOutputsCompatibleWithSessionDenom -- vecTxDSOut denom %d, entry.vecTxDSOut denom %d\n", GetDenominations(vecTxDSOut), GetDenominations(entry.vecTxDSOut));
        if (GetDenominations(vecTxDSOut) != GetDenominations(entry.vecTxDSOut)) return false;
    }

    return true;
}

bool CDarksendPool::IsAcceptableDenomAndCollateral(int nDenom, CTransaction txCollateral, PoolMessage &nMessageIDRet) {
    if (!fGhostNode) return false;

    // is denom even smth legit?
    std::vector<int> vecBits;
    if (!GetDenominationsBits(nDenom, vecBits)) {
        LogPrintf("privatesend", "CDarksendPool::IsAcceptableDenomAndCollateral -- denom not valid!\n");
        nMessageIDRet = ERR_DENOM;
        return false;
    }

    // check collateral
    if (!fUnitTest && !IsCollateralValid(txCollateral)) {
        LogPrintf("privatesend", "CDarksendPool::IsAcceptableDenomAndCollateral -- collateral not valid!\n");
        nMessageIDRet = ERR_INVALID_COLLATERAL;
        return false;
    }

    return true;
}

bool CDarksendPool::CreateNewSession(int nDenom, CTransaction txCollateral, PoolMessage &nMessageIDRet) {
    if (!fGhostNode || nSessionID != 0) return false;

    // new session can only be started in idle mode
    if (nState != POOL_STATE_IDLE) {
        nMessageIDRet = ERR_MODE;
        LogPrintf("CDarksendPool::CreateNewSession -- incompatible mode: nState=%d\n", nState);
        return false;
    }

    if (!IsAcceptableDenomAndCollateral(nDenom, txCollateral, nMessageIDRet)) {
        return false;
    }

    // start new session
    nMessageIDRet = MSG_NOERR;
    nSessionID = GetRandInt(999999) + 1;
    nSessionDenom = nDenom;

    SetState(POOL_STATE_QUEUE);
    nTimeLastSuccessfulStep = GetTimeMillis();

    if (!fUnitTest) {
        //broadcast that I'm accepting entries, only if it's the first entry through
        CDarksendQueue dsq(nDenom, activeGhostnode.vin, GetTime(), false);
        LogPrintf("privatesend", "CDarksendPool::CreateNewSession -- signing and relaying new queue: %s\n", dsq.ToString());
        dsq.Sign();
        dsq.Relay();
        vecDarksendQueue.push_back(dsq);
    }

    vecSessionCollaterals.push_back(txCollateral);
    LogPrintf("CDarksendPool::CreateNewSession -- new session created, nSessionID: %d  nSessionDenom: %d (%s)  vecSessionCollaterals.size(): %d\n",
              nSessionID, nSessionDenom, GetDenominationsToString(nSessionDenom), vecSessionCollaterals.size());

    return true;
}

bool CDarksendPool::AddUserToExistingSession(int nDenom, CTransaction txCollateral, PoolMessage &nMessageIDRet) {
    if (!fGhostNode || nSessionID == 0 || IsSessionReady()) return false;

    if (!IsAcceptableDenomAndCollateral(nDenom, txCollateral, nMessageIDRet)) {
        return false;
    }

    // we only add new users to an existing session when we are in queue mode
    if (nState != POOL_STATE_QUEUE) {
        nMessageIDRet = ERR_MODE;
        LogPrintf("CDarksendPool::AddUserToExistingSession -- incompatible mode: nState=%d\n", nState);
        return false;
    }

    if (nDenom != nSessionDenom) {
        LogPrintf("CDarksendPool::AddUserToExistingSession -- incompatible denom %d (%s) != nSessionDenom %d (%s)\n",
                  nDenom, GetDenominationsToString(nDenom), nSessionDenom, GetDenominationsToString(nSessionDenom));
        nMessageIDRet = ERR_DENOM;
        return false;
    }

    // count new user as accepted to an existing session

    nMessageIDRet = MSG_NOERR;
    nTimeLastSuccessfulStep = GetTimeMillis();
    vecSessionCollaterals.push_back(txCollateral);

    LogPrintf("CDarksendPool::AddUserToExistingSession -- new user accepted, nSessionID: %d  nSessionDenom: %d (%s)  vecSessionCollaterals.size(): %d\n",
              nSessionID, nSessionDenom, GetDenominationsToString(nSessionDenom), vecSessionCollaterals.size());

    return true;
}

/*  Create a nice string to show the denominations
    Function returns as follows (for 4 denominations):
        ( bit on if present )
        bit 0           - 100
        bit 1           - 10
        bit 2           - 1
        bit 3           - .1
        bit 4 and so on - out-of-bounds
        none of above   - non-denom
*/
std::string CDarksendPool::GetDenominationsToString(int nDenom) {
    std::string strDenom = "";
    int nMaxDenoms = vecPrivateSendDenominations.size();

    if (nDenom >= (1 << nMaxDenoms)) {
        return "out-of-bounds";
    }

    for (int i = 0; i < nMaxDenoms; ++i) {
        if (nDenom & (1 << i)) {
            strDenom += (strDenom.empty() ? "" : "+") + FormatMoney(vecPrivateSendDenominations[i]);
        }
    }

    if (strDenom.empty()) {
        return "non-denom";
    }

    return strDenom;
}

int CDarksendPool::GetDenominations(const std::vector <CTxDSOut> &vecTxDSOut) {
    std::vector<OUTPUT_PTR<CTxOutStandard>> vecTxOut;

    //BOOST_FOREACH(CTxDSOut out, vecTxDSOut)
        //vecTxOut.push_back(out);

    for (unsigned int idx = 0; idx < vecTxDSOut.size(); idx++)
    {
        OUTPUT_PTR<CTxOutStandard> txOutStandard = MAKE_OUTPUT<CTxOutStandard>();
        txOutStandard->nValue = vecTxDSOut[idx].GetValue();
        txOutStandard->scriptPubKey = *vecTxDSOut[idx].GetPScriptPubKey();
        vecTxOut.push_back(txOutStandard);
    }
    return GetDenominations(vecTxOut);
}

/*  Return a bitshifted integer representing the denominations in this list
    Function returns as follows (for 4 denominations):
        ( bit on if present )
        100       - bit 0
        10        - bit 1
        1         - bit 2
        .1        - bit 3
        non-denom - 0, all bits off
*/
int CDarksendPool::GetDenominations(const std::vector<OUTPUT_PTR<CTxOutStandard>> &vecTxOut, bool fSingleRandomDenom) {
    std::vector <std::pair<CAmount, int>> vecDenomUsed;

    // make a list of denominations, with zero uses
    BOOST_FOREACH(CAmount
    nDenomValue, vecPrivateSendDenominations)
    vecDenomUsed.push_back(std::make_pair(nDenomValue, 0));

    // look for denominations and update uses to 1
    /*
    BOOST_FOREACH(CTxOut txout, vecTxOut) {
        bool found = false;
        BOOST_FOREACH(PAIRTYPE(CAmount, int) &s, vecDenomUsed)
        {
            if (txout.nValue == s.first) {
                s.second = 1;
                found = true;
            }
        }
        if (!found) return 0;
    }
    */

    for (unsigned int idx = 0; idx < vecTxOut.size(); idx++)
    {
        bool found = false;
        BOOST_FOREACH(PAIRTYPE(CAmount, int) &s, vecDenomUsed)
        {
            if (vecTxOut[idx]->GetValue() == s.first) {
                s.second = 1;
                found = true;
            }
        }
        if (!found) return 0;
    }

    int nDenom = 0;
    int c = 0;
    // if the denomination is used, shift the bit on
    BOOST_FOREACH(PAIRTYPE(CAmount, int) &s, vecDenomUsed)
    {
        int bit = (fSingleRandomDenom ? GetRandInt(2) : 1) & s.second;
        nDenom |= bit << c++;
        if (fSingleRandomDenom && bit) break; // use just one random denomination
    }

    return nDenom;
}

bool CDarksendPool::GetDenominationsBits(int nDenom, std::vector<int> &vecBitsRet) {
    // ( bit on if present, 4 denominations example )
    // bit 0 - 100DASH+1
    // bit 1 - 10DASH+1
    // bit 2 - 1DASH+1
    // bit 3 - .1DASH+1

    int nMaxDenoms = vecPrivateSendDenominations.size();

    if (nDenom >= (1 << nMaxDenoms)) return false;

    vecBitsRet.clear();

    for (int i = 0; i < nMaxDenoms; ++i) {
        if (nDenom & (1 << i)) {
            vecBitsRet.push_back(i);
        }
    }

    return !vecBitsRet.empty();
}

int CDarksendPool::GetDenominationsByAmounts(const std::vector <CAmount> &vecAmount) {
    CScript scriptTmp = CScript();
    std::vector<OUTPUT_PTR<CTxOutStandard>> vecTxOut;

    BOOST_REVERSE_FOREACH(CAmount nAmount, vecAmount) {
        OUTPUT_PTR<CTxOutStandard> txOutStandard = MAKE_OUTPUT<CTxOutStandard>();
        txOutStandard->nValue = nAmount;
        txOutStandard->scriptPubKey = scriptTmp;
        vecTxOut.push_back(txOutStandard);
    }

    return GetDenominations(vecTxOut, true);
}

std::string CDarksendPool::GetMessageByID(PoolMessage nMessageID) {
    switch (nMessageID) {
        case ERR_ALREADY_HAVE:
            return _("Already have that input.");
        case ERR_DENOM:
            return _("No matching denominations found for mixing.");
        case ERR_ENTRIES_FULL:
            return _("Entries are full.");
        case ERR_EXISTING_TX:
            return _("Not compatible with existing transactions.");
        case ERR_FEES:
            return _("Transaction fees are too high.");
        case ERR_INVALID_COLLATERAL:
            return _("Collateral not valid.");
        case ERR_INVALID_INPUT:
            return _("Input is not valid.");
        case ERR_INVALID_SCRIPT:
            return _("Invalid script detected.");
        case ERR_INVALID_TX:
            return _("Transaction not valid.");
        case ERR_MAXIMUM:
            return _("Value more than PrivateSend pool maximum allows.");
        case ERR_MN_LIST:
            return _("Not in the Ghostnode list.");
        case ERR_MODE:
            return _("Incompatible mode.");
        case ERR_NON_STANDARD_PUBKEY:
            return _("Non-standard public key detected.");
        case ERR_NOT_A_MN:
            return _("This is not a Ghostnode.");
        case ERR_QUEUE_FULL:
            return _("Ghostnode queue is full.");
        case ERR_RECENT:
            return _("Last PrivateSend was too recent.");
        case ERR_SESSION:
            return _("Session not complete!");
        case ERR_MISSING_TX:
            return _("Missing input transaction information.");
        case ERR_VERSION:
            return _("Incompatible version.");
        case MSG_NOERR:
            return _("No errors detected.");
        case MSG_SUCCESS:
            return _("Transaction created successfully.");
        case MSG_ENTRIES_ADDED:
            return _("Your entries added successfully.");
        default:
            return _("Unknown response.");
    }
}

bool CDarkSendSigner::IsVinAssociatedWithPubkey(const CTxIn &txin, const CPubKey &pubkey) {
    CScript payee;
    payee = GetScriptForDestination(pubkey.GetID());

    CTransaction tx;
    uint256 hash;
    CTransactionRef txRef(&tx);
    if (GetTransaction(txin.prevout.hash, txRef, Params().GetConsensus(), hash, true)) {
        for (unsigned int idx = 0; idx < tx.vpout.size(); idx++)
        {
            if (tx.vpout[idx]->GetValue() == GHOSTNODE_COIN_REQUIRED * COIN && *tx.vpout[idx]->GetPScriptPubKey() == payee) return true;
        }
    }

    return false;
}

bool CDarkSendSigner::GetKeysFromSecret(std::string strSecret, CKey &keyRet, CPubKey &pubkeyRet) {
    CBitcoinSecret vchSecret;

    if (!vchSecret.SetString(strSecret)) return false;

    keyRet = vchSecret.GetKey();
    pubkeyRet = keyRet.GetPubKey();

    return true;
}

bool CDarkSendSigner::SignMessage(std::string strMessage, std::vector<unsigned char> &vchSigRet, CKey key) {
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    return key.SignCompact(ss.GetHash(), vchSigRet);
}

bool CDarkSendSigner::VerifyMessage(CPubKey pubkey, const std::vector<unsigned char> &vchSig, std::string strMessage, std::string &strErrorRet) {
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkeyFromSig;
    if (!pubkeyFromSig.RecoverCompact(ss.GetHash(), vchSig)) {
        strErrorRet = "Error recovering public key.";
        return false;
    }

    if (pubkeyFromSig.GetID() != pubkey.GetID()) {
        strErrorRet = strprintf("Keys don't match: pubkey=%s, pubkeyFromSig=%s, strMessage=%s, vchSig=%s",
                                pubkey.GetID().ToString(), pubkeyFromSig.GetID().ToString(), strMessage,
                                EncodeBase64(&vchSig[0], vchSig.size()));
        return false;
    }

    return true;
}

bool CDarkSendEntry::AddScriptSig(const CTxIn &txin) {
    BOOST_FOREACH(CTxDSIn & txdsin, vecTxDSIn)
    {
        if (txdsin.prevout == txin.prevout && txdsin.nSequence == txin.nSequence) {
            if (txdsin.fHasSig) return false;

            txdsin.scriptSig = txin.scriptSig;
            txdsin.prevPubKey = txin.prevPubKey;
            txdsin.fHasSig = true;

            return true;
        }
    }

    return false;
}

bool CDarksendQueue::Sign() {
    if (!fGhostNode) return false;

    std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(nTime) + boost::lexical_cast<std::string>(fReady);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, activeGhostnode.keyGhostnode)) {
        LogPrintf("CDarksendQueue::Sign -- SignMessage() failed, %s\n", ToString());
        return false;
    }

    return CheckSignature(activeGhostnode.pubKeyGhostnode);
}

bool CDarksendQueue::CheckSignature(const CPubKey &pubKeyGhostnode) {
    std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(nTime) + boost::lexical_cast<std::string>(fReady);
    std::string strError = "";

    if (!darkSendSigner.VerifyMessage(pubKeyGhostnode, vchSig, strMessage, strError)) {
        LogPrintf("CDarksendQueue::CheckSignature -- Got bad Ghostnode queue signature: %s; error: %s\n", ToString(), strError);
        return false;
    }

    return true;
}

bool CDarksendQueue::Relay() {
    std::vector < CNode * > vNodesCopy = g_connman->CopyNodeVector();
    BOOST_FOREACH(CNode * pnode, vNodesCopy)
        if (pnode->nVersion >= MIN_PRIVATESEND_PEER_PROTO_VERSION){
            const CNetMsgMaker msgMaker(pnode->GetSendVersion());
            g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::DSQUEUE, *this));
    }

    g_connman->ReleaseNodeVector(vNodesCopy);
    return true;
}

bool CDarksendBroadcastTx::Sign() {
    if (!fGhostNode) return false;

    std::string strMessage = tx->GetHash().ToString() + boost::lexical_cast<std::string>(sigTime);

    if (!darkSendSigner.SignMessage(strMessage, vchSig, activeGhostnode.keyGhostnode)) {
        LogPrintf("CDarksendBroadcastTx::Sign -- SignMessage() failed\n");
        return false;
    }

    return CheckSignature(activeGhostnode.pubKeyGhostnode);
}

bool CDarksendBroadcastTx::CheckSignature(const CPubKey &pubKeyGhostnode) {
    std::string strMessage = tx->GetHash().ToString() + boost::lexical_cast<std::string>(sigTime);
    std::string strError = "";

    if (!darkSendSigner.VerifyMessage(pubKeyGhostnode, vchSig, strMessage, strError)) {
        LogPrintf("CDarksendBroadcastTx::CheckSignature -- Got bad dstx signature, error: %s\n", strError);
        return false;
    }

    return true;
}

void CDarksendPool::RelayFinalTransaction(const CTransaction &txFinal) {
    LOCK(g_connman->cs_vNodes);
    BOOST_FOREACH(CNode * pnode, g_connman->vNodes)
        if (pnode->nVersion >= MIN_PRIVATESEND_PEER_PROTO_VERSION){
            const CNetMsgMaker msgMaker(pnode->GetSendVersion());
            g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::DSFINALTX, nSessionID, txFinal));
        }
}

void CDarksendPool::RelayIn(const CDarkSendEntry &entry) {
    if (!pSubmittedToGhostnode) return;

    CNode *pnode = g_connman->FindNode(pSubmittedToGhostnode->addr);
    if (pnode != NULL) {
        LogPrintf("CDarksendPool::RelayIn -- found master, relaying message to %s\n", pnode->addr.ToString());
        const CNetMsgMaker msgMaker(pnode->GetSendVersion());
        g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::DSVIN, entry));
    }
}

void CDarksendPool::PushStatus(CNode *pnode, PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID) {
    if (!pnode) return;
    const CNetMsgMaker msgMaker(pnode->GetSendVersion());
    g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::DSSTATUSUPDATE, nSessionID, (int) nState, (int) vecEntries.size(), (int) nStatusUpdate, (int) nMessageID));
}

void CDarksendPool::RelayStatus(PoolStatusUpdate nStatusUpdate, PoolMessage nMessageID) {
    LOCK(g_connman->cs_vNodes);
    BOOST_FOREACH(CNode * pnode, g_connman->vNodes)
        if (pnode->nVersion >= MIN_PRIVATESEND_PEER_PROTO_VERSION)
            PushStatus(pnode, nStatusUpdate, nMessageID);
}

void CDarksendPool::RelayCompletedTransaction(PoolMessage nMessageID) {
    LOCK(g_connman->cs_vNodes);
    BOOST_FOREACH(CNode * pnode, g_connman->vNodes)
        if (pnode->nVersion >= MIN_PRIVATESEND_PEER_PROTO_VERSION){
            const CNetMsgMaker msgMaker(pnode->GetSendVersion());
            g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::DSCOMPLETE, nSessionID, (int) nMessageID));
        }
}

void CDarksendPool::SetState(PoolState nStateNew) {
    if (fGhostNode && (nStateNew == POOL_STATE_ERROR || nStateNew == POOL_STATE_SUCCESS)) {
        LogPrintf("privatesend", "CDarksendPool::SetState -- Can't set state to ERROR or SUCCESS as a Ghostnode. \n");
        return;
    }

    LogPrintf("CDarksendPool::SetState -- nState: %d, nStateNew: %d\n", nState, nStateNew);
    nState = nStateNew;
}

void CDarksendPool::UpdatedBlockTip(const CBlockIndex *pindex) {
    pCurrentBlockIndex = pindex;
    LogPrintf("privatesend", "CDarksendPool::UpdatedBlockTip -- pCurrentBlockIndex->nHeight: %d\n", pCurrentBlockIndex->nHeight);

    if (!fLiteMode && ghostnodeSync.IsGhostnodeListSynced()) {
        NewBlock();
    }
}

//TODO: Rename/move to core
void ThreadCheckDarkSendPool() {
    if (fLiteMode) return; // disable all Dash specific functionality

    static bool fOneThread;
    if (fOneThread) return;
    fOneThread = true;

    // Make this thread recognisable as the PrivateSend thread
    RenameThread("dash-privatesend");

    unsigned int nTick = 0;
    unsigned int nDoAutoNextRun = nTick + PRIVATESEND_AUTO_TIMEOUT_MIN;

    while (true) {
        MilliSleep(1000);

        // try to sync from all available nodes, one step at a time
        ghostnodeSync.ProcessTick();

        if (ghostnodeSync.IsBlockchainSynced() && !ShutdownRequested()) {

            nTick++;

            // make sure to check all ghostnodes first
            mnodeman.Check();

            // check if we should activate or ping every few minutes,
            // slightly postpone first run to give net thread a chance to connect to some peers
            if (nTick % GHOSTNODE_MIN_MNP_SECONDS == 15)
                activeGhostnode.ManageState();

            if (nTick % 60 == 0) {
                mnodeman.ProcessGhostnodeConnections();
                mnodeman.CheckAndRemove();
                mnpayments.CheckAndRemove();
                instantsend.CheckAndRemove();
            }
            if (fGhostNode && (nTick % (60 * 5) == 0)) {
                mnodeman.DoFullVerificationStep();
            }

//            if(nTick % (60 * 5) == 0) {
//                governance.DoMaintenance();
//            }

            darkSendPool.CheckTimeout();
            darkSendPool.CheckForCompleteQueue();

            if (nDoAutoNextRun == nTick) {
                darkSendPool.DoAutomaticDenominating();
                nDoAutoNextRun = nTick + PRIVATESEND_AUTO_TIMEOUT_MIN + GetRandInt(PRIVATESEND_AUTO_TIMEOUT_MAX - PRIVATESEND_AUTO_TIMEOUT_MIN);
            }
        }
    }
}
