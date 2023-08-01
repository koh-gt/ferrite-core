// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>
#include <math.h>

unsigned int static KimotoGravityWell(const CBlockIndex* pindexLast, const Consensus::Params& params) {
    const CBlockIndex *BlockLastSolved = pindexLast;
    const CBlockIndex *BlockReading = pindexLast;
    uint64_t PastBlocksMass = 0;
    int64_t PastRateActualSeconds = 0;
    int64_t PastRateTargetSeconds = 0;
    double PastRateAdjustmentRatio = double(1);
    arith_uint256 PastDifficultyAverage;
    arith_uint256 PastDifficultyAveragePrev;
    double EventHorizonDeviation;
    double EventHorizonDeviationFast;
    double EventHorizonDeviationSlow;

    uint64_t pastSecondsMin = params.nPowTargetTimespan * 0.025;
    uint64_t pastSecondsMax = params.nPowTargetTimespan * 7;
    uint64_t PastBlocksMin = pastSecondsMin / params.nPowTargetSpacing;
    uint64_t PastBlocksMax = pastSecondsMax / params.nPowTargetSpacing;

    if (BlockLastSolved == nullptr || BlockLastSolved->nHeight == 0 || (uint64_t)BlockLastSolved->nHeight < PastBlocksMin) { return UintToArith256(params.powLimit).GetCompact(); }

    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
	if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
	    PastBlocksMass++;

	PastDifficultyAverage.SetCompact(BlockReading->nBits);
	if (i > 1) {
	    // handle negative arith_uint256
	    if(PastDifficultyAverage >= PastDifficultyAveragePrev)
		PastDifficultyAverage = ((PastDifficultyAverage - PastDifficultyAveragePrev) / i) + PastDifficultyAveragePrev;
	    else
		PastDifficultyAverage = PastDifficultyAveragePrev - ((PastDifficultyAveragePrev - PastDifficultyAverage) / i);
	}
	PastDifficultyAveragePrev = PastDifficultyAverage;

	PastRateActualSeconds = BlockLastSolved->GetBlockTime() - BlockReading->GetBlockTime();
	PastRateTargetSeconds = params.nPowTargetSpacing * PastBlocksMass;
	PastRateAdjustmentRatio = double(1);
	if (PastRateActualSeconds < 0) { PastRateActualSeconds = 0; }
	if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
	    PastRateAdjustmentRatio = double(PastRateTargetSeconds) / double(PastRateActualSeconds);
	}
	EventHorizonDeviation = 1 + (0.7084 * pow((double(PastBlocksMass)/double(360)), -1.228));
	EventHorizonDeviationFast = EventHorizonDeviation;
	EventHorizonDeviationSlow = 1 / EventHorizonDeviation;

	if (PastBlocksMass >= PastBlocksMin) {
		if ((PastRateAdjustmentRatio <= EventHorizonDeviationSlow) || (PastRateAdjustmentRatio >= EventHorizonDeviationFast))
		    { assert(BlockReading); break; }
	}
	if (BlockReading->pprev == nullptr) { assert(BlockReading); break; }
	    BlockReading = BlockReading->pprev;
    }

    arith_uint256 bnNew(PastDifficultyAverage);
    if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
	    bnNew *= PastRateActualSeconds;
	    bnNew /= PastRateTargetSeconds;
    }

    if (bnNew > UintToArith256(params.powLimit)) {
	    bnNew = UintToArith256(params.powLimit);
    }

    return bnNew.GetCompact();
}

unsigned int static DarkGravityWave(const CBlockIndex* pindexLast, const Consensus::Params& params) {
    /* current difficulty formula, dash - DarkGravity v3, written by Evan Duffield - evan@dash.org */
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    int64_t nPastBlocks = 24;

    // make sure we have at least (nPastBlocks + 1) blocks, otherwise just return powLimit
    if (!pindexLast || pindexLast->nHeight < nPastBlocks) {
	    return bnPowLimit.GetCompact();
    }

    const CBlockIndex *pindex = pindexLast;
    arith_uint256 bnPastTargetAvg;

    for (unsigned int nCountBlocks = 1; nCountBlocks <= nPastBlocks; nCountBlocks++) {
	    arith_uint256 bnTarget = arith_uint256().SetCompact(pindex->nBits);
	if (nCountBlocks == 1) {
	    bnPastTargetAvg = bnTarget;
	} else {
	    // NOTE: that's not an average really...
	    bnPastTargetAvg = (bnPastTargetAvg * nCountBlocks + bnTarget) / (nCountBlocks + 1);
	}

	if(nCountBlocks != nPastBlocks) {
	    assert(pindex->pprev); // should never fail
	    pindex = pindex->pprev;
	}
    }

    arith_uint256 bnNew(bnPastTargetAvg);

    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindex->GetBlockTime();
    // Accurate enough apart from ASERT algorithm.
    int64_t nTargetTimespan = nPastBlocks * params.nPowTargetSpacing;

    if (nActualTimespan < nTargetTimespan/3)
	    nActualTimespan = nTargetTimespan/3;
    if (nActualTimespan > nTargetTimespan*3)
	    nActualTimespan = nTargetTimespan*3;

    // Retarget
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > bnPowLimit) {
	    bnNew = bnPowLimit;
    }

    return bnNew.GetCompact();
}

unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    arith_uint256 bnNew;
    arith_uint256 bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
    // Ferrite: intermediate uint256 can overflow by 1 bit
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    bool fShift = bnNew.bits() > bnPowLimit.bits() - 1;
    if (fShift)
        bnNew >>= 1;
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;
    if (fShift)
        bnNew <<= 1;

    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

unsigned int GetNextWorkRequiredFerrite(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();
    // Ferrite: Difficulty adjustment used by Ferrite Core. Valid until nPowKGWHeight. (Fork height)
    // Old diffivulty algorithm.
    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Go back by what we want to be 14 days worth of blocks
    // Ferrite: This fixes an issue where a 51% attack can change difficulty at will.
    // Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
    int blockstogoback = params.DifficultyAdjustmentInterval()-1;
    if ((pindexLast->nHeight+1) != params.DifficultyAdjustmentInterval())
        blockstogoback = params.DifficultyAdjustmentInterval();

    // Go back by what we want to be 14 days worth of blocks
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < blockstogoback; i++)
        pindexFirst = pindexFirst->pprev;

    assert(pindexFirst);

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}

unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    assert(pblock != nullptr);
    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);

    if (pindexLast->nHeight + 1 < params.nPowKGWHeight) {
	return GetNextWorkRequiredFerrite(pindexLast, pblock, params);
    }

    // GetNextWorkRequiredFerrite is valid for block heights before nPowKGWHeight
    // After nPowKGWHeight, FEU and FEC will be two separate coins.

    if (params.fPowAllowMinDifficultyBlocks) {
	// recent block is more than 2 hours old
	if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + 2 * 60 * 60) {
	    return bnPowLimit.GetCompact();
	}
	// recent block is more than 10 minutes old
	if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing * 4) {
	    arith_uint256 bnNew = arith_uint256().SetCompact(pindexLast->nBits) * 10;
	    if (bnNew > bnPowLimit) {
		return bnPowLimit.GetCompact();
	    }
	    return bnNew.GetCompact();
	}
    }

    if (pindexLast->nHeight + 1 < params.nPowDGWHeight) {
	return KimotoGravityWell(pindexLast, params);
    }

    return DarkGravityWave(pindexLast, params);
}



bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}
