// Copyright (c) 2018 Tadhg Riordan Zcoin Developer
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include "main.h"
#include "client-api/server.h"
#include "client-api/protocol.h"
#include "rpc/server.h"
#include "znode-sync.h"
#include "core_io.h"
#include "wallet/wallet.h"
#include "client-api/wallet.h"
#include "univalue.h"
#include "darksend.h"
#include "chain.h"

using namespace std;
using namespace boost::chrono;

UniValue AvgBlockTime(){
    UniValue ret(UniValue::VOBJ);
    UniValue avgblocktime;

    double difficulty = GetDifficulty();
    //LogPrintf("difficulty: %lf\n", difficulty);

    double networkHashrateMH = GetNetworkHashPS(120, -1).get_real() / 1000000;
    //LogPrintf("networkHashrateMH: %lf\n", networkHashrateMH);

    // avg(secs) = difficulty * ((2^32) / (3600 * 10^6 * (networkHashrate(mh/s))) * 60 * 60
    // see http://www.wolframalpha.com/widgets/gallery/view.jsp?id=76444b3132fda0e2aca778051d776f1c

    avgblocktime = int(difficulty * (pow(2,32) / (3600 * pow(10,6) * networkHashrateMH)) * 60 * 60);

    return avgblocktime;
}

UniValue blockchain(Type type, const UniValue& data, const UniValue& auth, bool fHelp){

    UniValue blockinfoObj(UniValue::VOBJ);
    UniValue status(UniValue::VOBJ);
    UniValue currentBlock(UniValue::VOBJ);

    status.push_back(Pair("isBlockchainSynced", znodeSync.GetBlockchainSynced()));
    status.push_back(Pair("isZnodeListSynced", znodeSync.IsZnodeListSynced()));
    status.push_back(Pair("isWinnersListSynced", znodeSync.IsWinnersListSynced()));
    status.push_back(Pair("isSynced", znodeSync.IsSynced()));
    status.push_back(Pair("isFailed", znodeSync.IsFailed()));

    // if coming from PUB, height and time are included in data. otherwise just return chain tip
    UniValue height = find_value(data, "nHeight");
    UniValue time = find_value(data, "nTime");

    if(!(height.isNull() && time.isNull())){
        currentBlock.push_back(Pair("height", height));    
        currentBlock.push_back(Pair("timestamp", stoi(time.get_str())));
    }else{
        currentBlock.push_back(Pair("height", stoi(to_string(chainActive.Tip()->nHeight))));
        currentBlock.push_back(Pair("timestamp", stoi(to_string(chainActive.Tip()->nTime))));
    }

    blockinfoObj.push_back(Pair("testnet", Params().TestnetToBeDeprecatedFieldRPC()));
    blockinfoObj.push_back(Pair("connections", (int)vNodes.size()));
    blockinfoObj.push_back(Pair("type","full"));
    blockinfoObj.push_back(Pair("status", status));
    blockinfoObj.push_back(Pair("currentBlock", currentBlock));
    blockinfoObj.push_back(Pair("avgBlockTime", AvgBlockTime()));

    if(!znodeSync.GetBlockchainSynced()){
        unsigned long currentTimestamp = floor(
            system_clock::now().time_since_epoch() / 
            milliseconds(1)/1000);

        int blockTimestamp = chainActive.Tip()->nTime;

        int timeUntilSynced = currentTimestamp - blockTimestamp;
        blockinfoObj.push_back(Pair("timeUntilSynced", timeUntilSynced));
    }
    
    return blockinfoObj;
}

UniValue transaction(Type type, const UniValue& data, const UniValue& auth, bool fHelp){
    
    LOCK2(cs_main, pwalletMain->cs_wallet);

    //decode transaction
    UniValue ret(UniValue::VOBJ);
    CTransaction transaction;
    if (!DecodeHexTx(transaction, find_value(data, "txRaw").get_str()))
        throw JSONAPIError(API_DESERIALIZATION_ERROR, "Error parsing or validating structure in raw format");

    const CWalletTx * wtx = pwalletMain->GetWalletTx(transaction.GetHash());
    if(wtx==NULL)
        throw JSONAPIError(API_INVALID_PARAMETER, "Invalid, missing or duplicate parameter");
    
    ListAPITransactions(*wtx, ret, ISMINE_ALL);

    return ret;
}


UniValue block(Type type, const UniValue& data, const UniValue& auth, bool fHelp){

    UniValue getblockObj(UniValue::VOBJ);
    string blockhash;

    try{
        blockhash = find_value(data, "hashBlock").get_str();
    }catch (const std::exception& e){
        throw JSONAPIError(API_WRONG_TYPE_CALLED, "wrong key passed/value type for method");
    }

    StateBlock(getblockObj, blockhash);

    return getblockObj;
}

UniValue rebroadcast(Type type, const UniValue& data, const UniValue& auth, bool fHelp){

    UniValue ret(UniValue::VOBJ);
    LOCK2(cs_main, pwalletMain->cs_wallet);

    uint256 hash = uint256S(find_value(data, "txHash").get_str());
    CWalletTx *wtx = const_cast<CWalletTx*>(pwalletMain->GetWalletTx(hash));

    if (!wtx || wtx->isAbandoned() || wtx->GetDepthInMainChain() > 0){
        ret.push_back(Pair("result", false));
        ret.push_back(Pair("error", "Transaction is abandoned or already in chain"));
        return ret;
    }
    if (wtx->GetRequestCount() > 0){
        ret.push_back(Pair("result", false));
        ret.push_back(Pair("error", "Transaction has already been requested to be rebroadcast"));
        return ret;
    }

    CCoinsViewCache &view = *pcoinsTip;
    const CCoins* existingCoins = view.AccessCoins(hash);
    bool fHaveMempool = mempool.exists(hash);
    bool fHaveChain = existingCoins && existingCoins->nHeight < 1000000000;
    if (!fHaveMempool && !fHaveChain) {
        // push to local node and sync with wallets
        CValidationState state;
        bool fMissingInputs;
        if (!AcceptToMemoryPool(mempool, state, (CTransaction)*wtx, true, false, &fMissingInputs, true, false, maxTxFee)){
            ret.push_back(Pair("result", false));
            ret.push_back(Pair("error", "Transaction not accepted to mempool"));
            return ret;
        }
    } else if (fHaveChain) {
        ret.push_back(Pair("result", false));
        ret.push_back(Pair("error", "transaction already in block chain"));
        return ret;
    }

    RelayTransaction((CTransaction)*wtx);
    ret.push_back(Pair("result", true));
    return ret;
}

static const CAPICommand commands[] =
{ //  category              collection         actor (function)          authPort   authPassphrase   warmupOk
  //  --------------------- ------------       ----------------          -------- --------------   --------
    { "blockchain",         "blockchain",      &blockchain,              true,      false,           false  },
    { "blockchain",         "block",           &block,                   true,      false,           false  },
    { "blockchain",         "rebroadcast",     &rebroadcast,             true,      false,           false  },
    { "blockchain",         "transaction",     &transaction,             true,      false,           false  }
    
};
void RegisterBlockchainAPICommands(CAPITable &tableAPI)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableAPI.appendCommand(commands[vcidx].collection, &commands[vcidx]);
}