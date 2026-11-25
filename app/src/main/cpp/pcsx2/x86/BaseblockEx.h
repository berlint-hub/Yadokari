// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <cstring>
#include <map>

#include "common/Assertions.h"
#include "arm64/VixlHelpers.h"

// Every potential jump point in the PS2's addressable memory has a BASEBLOCK
// associated with it. So that means a BASEBLOCK for every 4 bytes of PS2
// addressable memory.  Yay!
struct BASEBLOCK
{
	uptr m_pFnptr;

	__inline uptr GetFnptr() const { return m_pFnptr; }
	void __inline SetFnptr(uptr ptr) { m_pFnptr = ptr; }
};

// extra block info (only valid for start of fn)
struct BASEBLOCKEX
{
	uptr fnptr;
	u32 startpc;
	u32 size;    // The size in dwords (equivalent to the number of instructions)
	u32 x86size; // The size in byte of the translated x86 instructions

#ifdef PCSX2_DEVBUILD
	// Could be useful to instrument the block
	//u32 visited; // number of times called
	//u64 ltime; // regs it assumes to have set already
#endif
};

// Blocks sorted by startpc, kept in a gap buffer: [0, mGapStart) and [mGapStart + gap, mReserved) hold the
// elements, gap = mReserved - mSize free slots in between. Inserting or erasing only moves the elements between
// the old and new gap position. Games compile and clear code a region at a time, so that is usually a few
// entries instead of the whole array: memmove'ing a sorted array of tens of thousands of blocks for every new
// block was most of the EE recompile time on the Switch.
//
// Indices are logical (sorted order). Pointers returned by insert()/operator[] stay valid until the next
// insert() or erase(), as before.
class BaseBlockArray
{
	s32 mReserved;
	s32 mSize;
	s32 mGapStart;
	BASEBLOCKEX* blocks;

	__fi s32 Physical(s32 idx) const
	{
		return (idx < mGapStart) ? idx : (idx + (mReserved - mSize));
	}

	// pos is a logical index in [0, mSize].
	void MoveGap(s32 pos)
	{
		const s32 gap = mReserved - mSize;
		if (pos < mGapStart)
			memmove(blocks + pos + gap, blocks + pos, (mGapStart - pos) * sizeof(BASEBLOCKEX));
		else if (pos > mGapStart)
			memmove(blocks + mGapStart, blocks + mGapStart + gap, (pos - mGapStart) * sizeof(BASEBLOCKEX));
		mGapStart = pos;
	}

	void reserve(s32 size)
	{
		pxAssert(size > mSize);
		auto* newMem = new BASEBLOCKEX[size];
		if (blocks)
		{
			const s32 old_gap = mReserved - mSize;
			const s32 new_gap = size - mSize;
			const s32 tail = mSize - mGapStart;
			memcpy(newMem, blocks, mGapStart * sizeof(BASEBLOCKEX));
			memcpy(newMem + mGapStart + new_gap, blocks + mGapStart + old_gap, tail * sizeof(BASEBLOCKEX));
			delete[] blocks;
		}
		blocks = newMem;
		mReserved = size;
	}

public:
	~BaseBlockArray()
	{
		delete[] blocks;
		blocks = nullptr;
	}

	explicit BaseBlockArray(s32 size)
		: mReserved(0)
		, mSize(0)
		, mGapStart(0)
		, blocks(nullptr)
	{
		reserve(size);
	}

	BaseBlockArray(const BaseBlockArray&) = delete;
	BaseBlockArray& operator=(const BaseBlockArray&) = delete;

	BASEBLOCKEX* insert(u32 startpc, uptr fnptr)
	{
		if (mSize + 1 >= mReserved)
			reserve(mReserved + std::max<s32>(0x2000, mReserved / 2)); // some games requires even more!

		// Insert the the new BASEBLOCKEX by startpc order
		int imin = 0, imax = mSize, imid;

		while (imin < imax)
		{
			imid = (imin + imax) >> 1;

			if ((*this)[imid].startpc > startpc)
				imax = imid;
			else
				imin = imid + 1;
		}

		pxAssert(imin == mSize || (*this)[imin].startpc > startpc);

		MoveGap(imin);
		BASEBLOCKEX* const block = blocks + mGapStart;
		memset(block, 0, sizeof(BASEBLOCKEX));
		block->startpc = startpc;
		block->fnptr = fnptr;

		mGapStart++;
		mSize++;
		return block;
	}

	__fi BASEBLOCKEX& operator[](int idx) const
	{
		return blocks[Physical(idx)];
	}

	void clear()
	{
		mSize = 0;
		mGapStart = 0;
	}

	[[nodiscard]] __fi u32 size() const
	{
		return mSize;
	}

	// Removes the logical range [first, last).
	void erase(s32 first, s32 last)
	{
		pxAssert(first >= 0 && first <= last && last <= mSize);

		// Bring the gap next to the range; whatever part of the range is on either side of it then
		// simply joins the gap.
		if (mGapStart < first)
			MoveGap(first);
		else if (mGapStart > last)
			MoveGap(last);

		mGapStart = first;
		mSize -= last - first;
	}
};

class BaseBlocks
{
protected:
	typedef std::multimap<u32, uptr>::iterator linkiter_t;

	// switch to a hash map later?
	std::multimap<u32, uptr> links;
	uptr recompiler;
	BaseBlockArray blocks;

public:
	BaseBlocks()
		: recompiler(0)
		, blocks(0x4000)
	{
	}

	void SetJITCompile(const void *recompiler_)
	{
		recompiler = reinterpret_cast<uptr>(recompiler_);
	}

	BASEBLOCKEX* New(u32 startpc, uptr fnptr);
	[[nodiscard]] int LastIndex(u32 startpc) const;
	//BASEBLOCKEX* GetByX86(uptr ip);

	[[nodiscard]] __fi int Index(u32 startpc) const
	{
		const int idx = LastIndex(startpc);
		if (idx == -1)
			return -1;

		const u32 block_startpc = blocks[idx].startpc;
		const u32 block_size = blocks[idx].size;
		if ((startpc < block_startpc) || (block_size && (startpc >= block_startpc + (block_size << 2)))) // size * 4
			return -1;
		else
			return idx;
	}

	__fi BASEBLOCKEX* operator[](int idx)
	{
		if (idx < 0 || idx >= (int)blocks.size())
			return nullptr;

		return &blocks[idx];
	}

	__fi BASEBLOCKEX* Get(u32 startpc)
	{
		return (*this)[Index(startpc)];
	}

	__fi void Remove(int first, int last)
	{
		pxAssert(first <= last);
		int idx = first;
		do
		{
			pxAssert(idx <= last);

			//u32 startpc = blocks[idx].startpc;
			auto range = links.equal_range(blocks[idx].startpc);
			for (auto i = range.first; i != range.second; ++i) {
//                *(u32 *) i->second = recompiler - (i->second + 4);
                armEmitJmpPtr((void*)i->second, (void*)recompiler, true);
            }

			if (IsDevBuild)
			{
				// Clear the first instruction to 0xcc (breakpoint), as a way to assert if some
				// static jumps get left behind to this block.  Note: Do not clear more than the
				// first byte, since this code is called during exception handlers and event handlers
				// both of which expect to be able to return to the recompiled code.

				BASEBLOCKEX effu(blocks[idx]);
				memset((void*)effu.fnptr, 0xcc, 1);
			}
		} while (idx++ < last);

		// TODO: remove links from this block?
		blocks.erase(first, last + 1);
	}

	void Link(u32 pc, s32* jumpptr);

	__fi void Reset()
	{
		blocks.clear();
		links.clear();
	}
};

#define PC_GETBLOCK_(x, reclut) ((BASEBLOCK*)(reclut[((u32)(x)) >> 16] + (x) * (sizeof(BASEBLOCK) >> 2))) // (sizeof(BASEBLOCK) / 4)

/**
 * Add a page to the recompiler lookup table
 *
 * Will associate `reclut[pagebase + pageidx]` with `mapbase[mappage << 14]`
 * Will associate `hwlut[pagebase + pageidx]` with `pageidx << 16`
 */
static inline void recLUT_SetPage(uptr reclut[0x10000], u32 hwlut[0x10000],
                                  BASEBLOCK* mapbase, uint pagebase, uint pageidx, uint mappage)
{
	// this value is in 64k pages!
	uint page = pagebase + pageidx;

	pxAssert(page < 0x10000);
	reclut[page] = (uptr)&mapbase[((s32)mappage - (s32)page) << 14];
	if (hwlut)
		hwlut[page] = 0u - (pagebase << 16);
}

static_assert(sizeof(BASEBLOCK) == 8, "BASEBLOCK is not 8 bytes");
