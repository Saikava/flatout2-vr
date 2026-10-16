#include "eyeshare.h"

#include "game.h"
#include "util.h"

#include <windows.h>

#include <cstring>
#include <unordered_map>
#include <vector>

namespace share {
namespace {

enum class Mode { Off, Keep, Reuse };
Mode g_mode;

// The left eye's leaf cards, tree by tree (the eyes can sort their trees differently, so their
// chunks need not hold the same trees). A tree is known by its model, placement and colours; its
// cards are the same in both eyes as long as the card direction (kLeafBasis) is.
struct TreeKey
{
    uint32_t model, placement, colours;
    bool operator==(const TreeKey& o) const { return model == o.model && placement == o.placement && colours == o.colours; }
};
struct TreeHash
{
    size_t operator()(const TreeKey& k) const { return (k.model * 31u + k.placement) * 31u + k.colours; }
};
std::vector<BYTE> g_kept;                              // the cards of the trees the left eye built, one after another
std::unordered_map<TreeKey, std::pair<size_t, size_t>, TreeHash> g_trees;  // offset and size in g_kept
uint32_t g_keptBasis[3];
// The left eye builds a chunk in system memory (the locked vertex buffer may be write-combined,
// slow to read back) and copies it into the buffer before it is unlocked.
size_t g_openAt, g_openSize;
BYTE* g_openDst;
Stats g_stats;

struct ChunkTrees
{
    const uint32_t* const* items;
    int first, count;
    TreeKey Key(int i) const
    {
        const uint32_t* item = items[first + i];
        return { item[0], item[1], item[3] };  // model, placement, colours
    }
    // Bytes of the tree's cards: four vertices of 24 bytes per card (the model's vertex count at +0x18).
    size_t Size(int i) const { return (size_t)*(const uint16_t*)((const BYTE*)(uintptr_t)items[first + i][0] + 0x18) * 24; }
};

// The leaf pass locked a chunk's vertex buffer (p, size bytes) for `count` trees. Returns where the
// game should build the cards, or null when they are already there (the right eye's copy).
BYTE* __cdecl LeafLock(BYTE* p, int size, int count, const BYTE* frame)
{
    if (g_mode == Mode::Off || size <= 0 || count <= 0) return p;
    ChunkTrees t = { *(const uint32_t* const* const*)(frame + fo2::kLeafFrameItems), *(const int*)(frame + fo2::kLeafFrameFirst), count };
    if (g_mode == Mode::Keep)
    {
        g_openAt = g_kept.size();
        g_openSize = (size_t)size;
        g_openDst = p;
        g_kept.resize(g_openAt + g_openSize);
        memcpy(g_keptBasis, (const void*)fo2::kLeafBasis, sizeof(g_keptBasis));
        size_t at = g_openAt;
        for (int i = 0; i < count; i++)
        {
            size_t n = t.Size(i);
            g_trees[t.Key(i)] = { at, n };
            at += n;
        }
        return g_kept.data() + g_openAt;
    }
    if (memcmp(g_keptBasis, (const void*)fo2::kLeafBasis, sizeof(g_keptBasis)) == 0)
    {
        size_t at = 0;
        int i = 0;
        for (; i < count; i++)
        {
            auto it = g_trees.find(t.Key(i));
            if (it == g_trees.end() || it->second.second != t.Size(i) || at + it->second.second > (size_t)size) break;
            at += it->second.second;
        }
        if (i == count && at == (size_t)size)
        {
            BYTE* dst = p;
            for (i = 0; i < count; i++)
            {
                const auto& place = g_trees.find(t.Key(i))->second;
                memcpy(dst, g_kept.data() + place.first, place.second);
                dst += place.second;
            }
            g_stats.reused++;
            return nullptr;
        }
    }
    g_stats.rebuilt++;
    return p;
}

void __cdecl LeafUnlock()
{
    if (!g_openDst) return;
    memcpy(g_openDst, g_kept.data() + g_openAt, g_openSize);
    g_openDst = nullptr;
}

uintptr_t g_leafLoop = fo2::kLeafLoop, g_leafDone = fo2::kLeafDone, g_leafLockFailed = fo2::kLeafLockFailed,
          g_leafUnlockNext = fo2::kLeafUnlock + 7;

// Replaces the lock at kLeafLock (eax: the vertex buffer, ecx: the size, ebx: the chunk's trees).
__declspec(naked) void LeafLockStub()
{
    __asm {
        push ecx
        mov edx, [eax]
        push ecx
        mov ecx, eax
        call dword ptr [edx + 8]
        pop ecx
        test eax, eax
        jz failed
        push ebp
        push ebx
        push ecx
        push eax
        call LeafLock
        add esp, 16
        test eax, eax
        jz filled
        mov [ebp + 0x54], eax
        jmp dword ptr [g_leafLoop]
    filled:
        mov dword ptr [ebp + 0x54], 0
        jmp dword ptr [g_leafDone]
    failed:
        mov [ebp + 0x54], eax
        jmp dword ptr [g_leafLockFailed]
    }
}

// Runs before the unlock at kLeafUnlock, then the two instructions it replaces.
__declspec(naked) void LeafUnlockStub()
{
    __asm {
        push eax
        push ecx
        push edx
        call LeafUnlock
        pop edx
        pop ecx
        pop eax
        mov ecx, [ebp + 0x4c]
        mov dx, [ecx + 0xc]
        jmp dword ptr [g_leafUnlockNext]
    }
}

// ---- grass -----------------------------------------------------------------------------------------
// The left eye's grass cards, batch by batch: a batch's cards fade with the distance along the
// view direction, the same for both eyes. The left eye writes its buffers in system memory and
// copies them over before each draw, as for the leaves.
std::vector<BYTE> g_grassKept;                                              // its buffers, one after another
std::unordered_map<const void*, std::pair<size_t, size_t>> g_grassBatches;  // batch: offset and size in g_grassKept
size_t g_grassChunk;  // the open buffer's offset in g_grassKept
BYTE* g_grassDst;     // and the vertex buffer it goes to (null: none open)
const void* g_grassBatch;
size_t g_grassBatchAt;

BYTE* __cdecl GrassLock(BYTE* p)
{
    if (g_mode != Mode::Keep || !p) return p;
    g_grassChunk = g_grassKept.size();
    g_grassKept.resize(g_grassChunk + fo2::kGrassChunkBytes);
    g_grassDst = p;
    return g_grassKept.data() + g_grassChunk;
}

// Before a buffer is drawn; dst: the game's write pointer in it.
void __cdecl GrassFlush(const BYTE* dst)
{
    if (!g_grassDst) return;
    const BYTE* base = g_grassKept.data() + g_grassChunk;
    size_t used = dst >= base && dst <= base + fo2::kGrassChunkBytes ? (size_t)(dst - base) : fo2::kGrassChunkBytes;
    memcpy(g_grassDst, base, used);
    g_grassDst = nullptr;
}

// A batch's cards start. True when the right eye copied them (the game then skips building them).
int __cdecl GrassBatch(const void* batch, int count, BYTE** dst, int* freeCards)
{
    g_grassBatch = nullptr;
    if (g_mode == Mode::Off || count <= 0) return 0;
    if (g_mode == Mode::Keep)
    {
        if (!g_grassDst) return 0;
        g_grassBatch = batch;
        g_grassBatchAt = (size_t)(*dst - g_grassKept.data());
        return 0;
    }
    auto it = g_grassBatches.find(batch);
    if (it == g_grassBatches.end())
    {
        g_stats.grassBuilt++;
        return 0;
    }
    size_t n = it->second.second;
    memcpy(*dst, g_grassKept.data() + it->second.first, n);
    *dst += n;
    *freeCards -= (int)(n / fo2::kGrassCardBytes);
    g_stats.grassCopied++;
    return 1;
}

void __cdecl GrassBatchEnd(const BYTE* dst)
{
    if (!g_grassBatch) return;
    size_t end = (size_t)(dst - g_grassKept.data());
    if (end >= g_grassBatchAt && end <= g_grassChunk + fo2::kGrassChunkBytes)
        g_grassBatches[g_grassBatch] = { g_grassBatchAt, end - g_grassBatchAt };
    g_grassBatch = nullptr;
}

uintptr_t g_grassLockNext = fo2::kGrassLock + 20, g_grassFlushNext = fo2::kGrassFlush + 11,
          g_grassFinalFlushNext = fo2::kGrassFinalFlush + 9, g_grassBatchNext = fo2::kGrassBatch + 13,
          g_grassBatchEndAt = fo2::kGrassBatchEnd, g_grassBatchEndNext = fo2::kGrassBatchEnd + 8;

// Replaces the lock (mov edx,[edi]; push 30000h; mov ecx,edi; call [edx+8]; mov edx,[esp+14h];
// mov [esp+3Ch],eax).
__declspec(naked) void GrassLockStub()
{
    __asm {
        mov edx, [edi]
        push 0x30000
        mov ecx, edi
        call dword ptr [edx + 8]
        push eax
        call GrassLock
        add esp, 4
        mov edx, [esp + 0x14]
        mov [esp + 0x3c], eax
        jmp dword ptr [g_grassLockNext]
    }
}

// Before the draw of a full buffer, then what it replaces (sub ecx,eax; mov eax,ecx; lea ecx,[eax*4]).
__declspec(naked) void GrassFlushStub()
{
    __asm {
        push eax
        push ecx
        push edx
        push dword ptr [esp + 12 + 0x3c]
        call GrassFlush
        add esp, 4
        pop edx
        pop ecx
        pop eax
        sub ecx, eax
        mov eax, ecx
        lea ecx, [eax * 4]
        jmp dword ptr [g_grassFlushNext]
    }
}

// Before the draw of the last buffer, then what it replaces (sub ecx,eax; lea eax,[ecx*4]).
__declspec(naked) void GrassFinalFlushStub()
{
    __asm {
        push eax
        push ecx
        push edx
        push dword ptr [esp + 12 + 0x3c]
        call GrassFlush
        add esp, 4
        pop edx
        pop ecx
        pop eax
        sub ecx, eax
        lea eax, [ecx * 4]
        jmp dword ptr [g_grassFinalFlushNext]
    }
}

// A batch starts (edx: its card count); replaces test edx,edx; mov eax,[esp+38h];
// mov eax,[esp+eax*4+110h].
__declspec(naked) void GrassBatchStub()
{
    __asm {
        push edx
        push ecx
        lea ecx, [esp + 8 + 0x0c]
        push ecx
        lea ecx, [esp + 12 + 0x3c]
        push ecx
        push edx
        mov eax, [esp + 20 + 0x38]
        mov eax, [esp + 20 + eax * 4 + 0x110]
        push eax
        call GrassBatch
        add esp, 16
        pop ecx
        pop edx
        test eax, eax
        jnz copied
        test edx, edx
        mov eax, [esp + 0x38]
        mov eax, [esp + eax * 4 + 0x110]
        jmp dword ptr [g_grassBatchNext]
    copied:
        jmp dword ptr [g_grassBatchEndAt]
    }
}

// A batch's cards end; then what it replaces (mov edx,[esp+38h]; mov eax,[esp+40h]).
__declspec(naked) void GrassBatchEndStub()
{
    __asm {
        push eax
        push ecx
        push edx
        push dword ptr [esp + 12 + 0x3c]
        call GrassBatchEnd
        add esp, 4
        pop edx
        pop ecx
        pop eax
        mov edx, [esp + 0x38]
        mov eax, [esp + 0x40]
        jmp dword ptr [g_grassBatchEndNext]
    }
}

bool PatchJump(uintptr_t at, const BYTE* expected, size_t n, const void* to)
{
    BYTE code[32];
    if (n < 5 || n > sizeof(code)) return false;
    code[0] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    memcpy(code + 1, &rel, 4);
    memset(code + 5, 0x90, n - 5);
    return PatchCode(at, expected, code, n);
}

}  // namespace

bool Install()
{
    // mov edx,[eax]; push ecx; mov ecx,eax; call [edx+8]; test eax,eax; mov [ebp+54h],eax
    static const BYTE lock[] = { 0x8B, 0x10, 0x51, 0x8B, 0xC8, 0xFF, 0x52, 0x08, 0x85, 0xC0, 0x89, 0x45, 0x54 };
    // mov ecx,[ebp+4Ch]; mov dx,[ecx+0Ch]
    static const BYTE unlock[] = { 0x8B, 0x4D, 0x4C, 0x66, 0x8B, 0x51, 0x0C };
    if (memcmp((const void*)fo2::kLeafLock, lock, sizeof(lock)) != 0 || memcmp((const void*)fo2::kLeafUnlock, unlock, sizeof(unlock)) != 0)
        return false;
    if (!PatchJump(fo2::kLeafUnlock, unlock, sizeof(unlock), (const void*)&LeafUnlockStub) ||
        !PatchJump(fo2::kLeafLock, lock, sizeof(lock), (const void*)&LeafLockStub))
        return false;
    // The grass pass: all five places or none.
    static const BYTE grassLock[] = { 0x8B, 0x17, 0x68, 0x00, 0x00, 0x03, 0x00, 0x8B, 0xCF, 0xFF,
                                      0x52, 0x08, 0x8B, 0x54, 0x24, 0x14, 0x89, 0x44, 0x24, 0x3C };
    static const BYTE grassFlush[] = { 0x2B, 0xC8, 0x8B, 0xC1, 0x8D, 0x0C, 0x85, 0x00, 0x00, 0x00, 0x00 };
    static const BYTE grassFinalFlush[] = { 0x2B, 0xC8, 0x8D, 0x04, 0x8D, 0x00, 0x00, 0x00, 0x00 };
    static const BYTE grassBatch[] = { 0x85, 0xD2, 0x8B, 0x44, 0x24, 0x38, 0x8B, 0x84, 0x84, 0x10, 0x01, 0x00, 0x00 };
    static const BYTE grassBatchEnd[] = { 0x8B, 0x54, 0x24, 0x38, 0x8B, 0x44, 0x24, 0x40 };
    if (memcmp((const void*)fo2::kGrassLock, grassLock, sizeof(grassLock)) != 0 ||
        memcmp((const void*)fo2::kGrassFlush, grassFlush, sizeof(grassFlush)) != 0 ||
        memcmp((const void*)fo2::kGrassFinalFlush, grassFinalFlush, sizeof(grassFinalFlush)) != 0 ||
        memcmp((const void*)fo2::kGrassBatch, grassBatch, sizeof(grassBatch)) != 0 ||
        memcmp((const void*)fo2::kGrassBatchEnd, grassBatchEnd, sizeof(grassBatchEnd)) != 0)
    {
        Log("eyes: grass pass differs - each eye builds its own grass");
        return true;
    }
    PatchJump(fo2::kGrassFlush, grassFlush, sizeof(grassFlush), (const void*)&GrassFlushStub);
    PatchJump(fo2::kGrassFinalFlush, grassFinalFlush, sizeof(grassFinalFlush), (const void*)&GrassFinalFlushStub);
    PatchJump(fo2::kGrassBatchEnd, grassBatchEnd, sizeof(grassBatchEnd), (const void*)&GrassBatchEndStub);
    PatchJump(fo2::kGrassBatch, grassBatch, sizeof(grassBatch), (const void*)&GrassBatchStub);
    PatchJump(fo2::kGrassLock, grassLock, sizeof(grassLock), (const void*)&GrassLockStub);
    return true;
}

void BeginEye(int eye)
{
    LeafUnlock();
    g_mode = g_cfg.testShareEyes ? (eye == 0 ? Mode::Keep : Mode::Reuse) : Mode::Off;
    if (eye == 0)
    {
        g_kept.clear();
        g_trees.clear();
        g_grassKept.clear();
        g_grassBatches.clear();
    }
    g_grassDst = nullptr;
    g_grassBatch = nullptr;
}

void EndEyes()
{
    LeafUnlock();
    g_mode = Mode::Off;
    g_grassDst = nullptr;
    g_grassBatch = nullptr;
}

Stats TakeStats()
{
    Stats s = g_stats;
    g_stats = Stats();
    return s;
}

}  // namespace share
