//------------------------------------------------------------------------------
/*
This file is part of stellard: https://github.com/stellar/stellard
Copyright (c) 2014 Stellar Development Foundation.

Permission to use, copy, modify, and/or distribute this software for any
purpose  with  or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================
#include "InflationTransactor.h"

#define INFLATION_FREQUENCY			(60*60*24*30)  // every 30 days
//inflation is 0.000817609695 per 30 days, or 1% a year
#define INFLATION_RATE_TRILLIONTHS      817609695L
#define TRILLION                    1000000000000L
#define INFLATION_NUM_WINNERS		5
#define INFLATION_WIN_MIN_PERCENT	.1
#define INFLATION_START_TIME		(1397088000-946684800) // seconds since 1/1/2000
#ifndef MIN
#define MIN(x,y)  ((x)<(y) ? (x) : (y))
#endif

/*
What about when an account that wins a dole is now gone?
*/


namespace ripple {

	bool voteSorter(const std::pair<uint160, uint64>  &p1, const std::pair<uint160, uint64> &p2)
	{
		return p1.second > p2.second;
	}

	uint64 bigIntegerToUint64(beast::BigInteger& bigInt){
		uint64 res = bigInt.getBitRangeAsInt(0, 32) + ( ((uint64)bigInt.getBitRangeAsInt(32, 32)) << 32 );

		return res;
	}

	SETUP_LOG(InflationTransactor)



	TER InflationTransactor::doApply()
	{
		TER terResult = tesSUCCESS;
		// make sure it is time to apply inflation
		// make sure the seq number of this inflation transaction is correct

		uint32 seq = mTxn.getFieldU32(sfInflateSeq);
		

		if (seq != mEngine->getLedger()->getInflationSeq())
		{
			WriteLog(lsINFO, InflationTransactor) << "doInflation: Invalid Seq number.";

			return telNOT_TIME;
		}

		uint32 closeTime=mEngine->getLedger()->getParentCloseTimeNC();
		uint32 nextTime = (INFLATION_START_TIME + seq*INFLATION_FREQUENCY);
		if (closeTime < nextTime)
		{
			WriteLog(lsINFO, InflationTransactor) << "doInflation: Too early.";

			return telNOT_TIME;
		}

		// check previous ledger if this should be applied now
		// loop through all accounts and tally votes
		// dole out the inflation amount to the winners
		uint256 parentHash=mEngine->getLedger()->getParentHash();
		Ledger::pointer votingLedger=getApp().getLedgerMaster().getLedgerByHash(parentHash);
		if (votingLedger)
		{
			std::map< uint160, uint64 > voteTally;
			
			// TODO: is there a better way to do this than walk every element in the ledger?
			const SHAMap::pointer votingLedgerItems = votingLedger->peekAccountStateMap();
			SHAMapItem::pointer item = votingLedgerItems->peekFirstItem();
			while (item)
			{
				
				SLE::pointer s=boost::make_shared<SLE>(item->peekSerializer(), item->getTag());

				if (s->getType() == ltACCOUNT_ROOT)
				{
					if (s->isFieldPresent(sfInflationDest))
					{
						uint160 addr=s->getFieldAccount160(sfInflationDest);
						STAmount balance = s->getFieldAmount(sfBalance);
						if (voteTally.find(addr) == voteTally.end()) voteTally[addr] = balance.getNValue();
						else voteTally[addr] = voteTally[addr] + balance.getNValue();
					}
					
				}

				item = votingLedgerItems->peekNextItem(item->getTag());
			}
			// sort the votes
			std::vector< std::pair<uint160, uint64> > sortedVotes;
			copy(voteTally.begin(), voteTally.end(), back_inserter(sortedVotes));

			sort(sortedVotes.begin(), sortedVotes.end(), voteSorter);

			uint64 minBalance = mEngine->getLedger()->getTotalCoins()*INFLATION_WIN_MIN_PERCENT;
			uint64 totalVoted = 0;
			int maxIndex = MIN(INFLATION_NUM_WINNERS, sortedVotes.size());
			for (int n = 0; n < maxIndex; n++)
			{
				if (sortedVotes[n].second > minBalance)
				{
					totalVoted += sortedVotes[n].second;
				}else 
				{
					if (totalVoted)
						maxIndex = n;
					break;
				}
			}

			if (!totalVoted)
			{  // no one crossed the threshold so just take top N
				for (int n = 0; n < maxIndex; n++)
				{
					totalVoted += sortedVotes[n].second;
				}
			}


			// TODO: Is there better way to cast uint64 to signed int64? (There is no constructor using uint64)
			beast::BigInteger biCoinsToDole      { (int64) mEngine->getLedger()->getTotalCoins() }; 
			beast::BigInteger inflRateMultiplier { (int64) INFLATION_RATE_TRILLIONTHS };
			beast::BigInteger inflRateDivider    { (int64) TRILLION };
			beast::BigInteger poolFee            { (int64) mEngine->getLedger()->getFeePool() };

			/// coinsToDole = totalCoins * INFLATION_RATE + feePool
			biCoinsToDole *= inflRateMultiplier;
			biCoinsToDole /= inflRateDivider;
			biCoinsToDole += poolFee;


			beast::BigInteger biTotalVoted { (int64)totalVoted };

			
			for (int n = 0; n < maxIndex; n++)
			{
				/// coinsDoled = coinToDole * ( votes / totalVoted )
				beast::BigInteger biCoinsDoled { (int64)sortedVotes[n].second }; 
				biCoinsDoled *= biCoinsToDole;
				biCoinsDoled /= biTotalVoted;

				uint64 coinsDoled = bigIntegerToUint64(biCoinsDoled);


				SLE::pointer account = mEngine->entryCache(ltACCOUNT_ROOT, Ledger::getAccountRootIndex(sortedVotes[n].first));
				
				if (account)
				{
					mEngine->entryModify(account);
					account->setFieldAmount(sfBalance, account->getFieldAmount(sfBalance) + coinsDoled);
					mEngine->getLedger()->inflateCoins(coinsDoled);
				}
			}

			mEngine->getLedger()->incrementInflationSeq();
			
		}
		else
		{
			WriteLog(lsINFO, InflationTransactor) << "doInflation: Ledger not found?";

			return temUNKNOWN;
		}



		return terResult;
	}

}
