#include <iostream>
#include <string.h>
#include <math.h>
#include <unordered_set>
#include <algorithm>

#include "cache.h"

#define int int32_t
#define uint uint32_t
#define long int64_t
#define ulong uint64_t

//reverse the bits of a 7 bit integer, used as part of the pc tag hash
constexpr uint REVERSE[] = {0, 64, 32, 96, 16, 80, 48, 112, 8, 72, 40, 104, 24, 88, 56, 120, 4, 68, 36, 100, 20, 84, 52, 116, 12, 76, 44, 108, 28, 92, 60, 124, 2, 66, 34, 98, 18, 82, 50, 114, 10, 74, 42, 106, 26, 90, 58, 122, 6, 70, 38, 102, 22, 86, 54, 118, 14, 78, 46, 110, 30, 94, 62, 126, 1, 65, 33, 97, 17, 81, 49, 113, 9, 73, 41, 105, 25, 89, 57, 121, 5, 69, 37, 101, 21, 85, 53, 117, 13, 77, 45, 109, 29, 93, 61, 125, 3, 67, 35, 99, 19, 83, 51, 115, 11, 75, 43, 107, 27, 91, 59, 123, 7, 71, 39, 103, 23, 87, 55, 119, 15, 79, 47, 111, 31, 95, 63, 127};

constexpr uint CORES = NUM_CPUS;

constexpr uint BLOC = 6; //block size(log2)
constexpr uint PAGE = 12; //page size(log2)
constexpr uint OFFSETS = PAGE - BLOC; //possible block offsets in a page(log2)
constexpr uint WAYS = 3; //#ways(log2)
constexpr uint SETS = 10; //#sets(log2)

constexpr uint PCS = 5; //number of rows in PC delta table (log2)
constexpr uint PCTAGS = 13 - PCS; //tag bits in PC Delta table
constexpr uint DEFPCTAG = (1 << PCTAGS) - 1;
constexpr uint USEFULS = 3; //number of bits in the useful counters in the PC + Delta table
constexpr uint MAXUSE = (1 << USEFULS) - 1; //max value of useful counter
constexpr uint USEGRAN = 3; //garnularity of useful counter (log2)
constexpr uint ACCS = 7; //number of bits in accuracy
constexpr uint PREFACC = (uint)(0.4 * (1 << ACCS)); //accuracy threshold at which to prefetch
constexpr uint DEFACC = PREFACC / 2;
constexpr uint MINACC = 0;
constexpr uint MAXACC = (1 << ACCS) - 1;
constexpr uint TRAINFACT = 5; //factor by which to train accuracies (log2)

constexpr uint HIST = 10; //length of GHB(log2)
constexpr uint SAMPLECHANCE = 1; //chance that a demand load gets sampled (-log2)

constexpr uint ASSOC = 0; //associativity of page table(log2)
constexpr uint BUCKETS = 8; //number of page table sets(log2)
constexpr uint COUNTS = 1; //length of page + offset counters
constexpr uint PAGETAGS = 10; //size of tags in page offset table
constexpr uint DEFPAGETAG = (1 << PAGETAGS) - 1;
constexpr uint DEFOFFSET = (1 << OFFSETS) - 1;

constexpr uint DEFPC = (1 << PCS) - 1;
constexpr uint DEFPAGE = (1 << BUCKETS) - 1;

constexpr double MAXMSHR = 0.5; //MSHR occupancy at which to stop prefetch filling

constexpr uint detrainAcc(uint acc) { //train the accuracy down
    acc -= (acc + (1 << (TRAINFACT - 1))) >> TRAINFACT;
    return acc > MINACC ? acc : MINACC;
}

constexpr uint trainAcc(uint acc) { //train the accuracy up
    acc = detrainAcc(acc);
    acc += 1 << (ACCS - TRAINFACT);
    return acc < MAXACC ? acc : MAXACC;
}

constexpr ulong pcCompress(ulong pc) { //delete trailing 0s and the trailing 1 from the PC
    return pc ? pc >> (__builtin_ctz(pc) + 1) : 0;
}

constexpr uint pcHash(ulong pc) { //get the row in the PC + delta table
    return pcCompress(pc) & ((1 << PCS) - 1);
}

constexpr uint pcTag(ulong pc) { //get the PC tag (part of the PC not used in the row index)
    pc = pcCompress(pc) >> PCS;
    pc = (pc & -(2 << OFFSETS)) ^ REVERSE[pc & ((2 << OFFSETS) - 1)]; //reverse the first 7 bits of the tag for hashing purposes
    return pc & ((1 << PCTAGS) - 1);
}

constexpr ulong getPage(ulong addr) {
    return addr >> OFFSETS;
}

constexpr uint getOffset(ulong addr) {
    return addr & ((1 << OFFSETS) - 1);
}

constexpr ulong getAddr(ulong page, uint offset) {
    return (page << OFFSETS) + offset;
}

constexpr ulong getAddr(ulong byteAddr) {
    return byteAddr >> BLOC;
}

constexpr ulong getByteAddr(ulong addr, uint byte) {
    return (addr << BLOC) + byte;
}

constexpr uint getSet(ulong addr) {
    return addr & ((1 << SETS) - 1);
}

constexpr uint getIndex(uint index, uint tag) { //get the index in the PC delta table given the delta and pctag
    return (index ^ tag) & ((2 << OFFSETS) - 1);
}

struct PTEntry { //entry in the PT (PC delta table)
    uint acc; //accuracy
    uint tag; //pctag (part not used to index the row)
    uint use; //useful counter, incremented whenever the entry is positively trained, decremented whenever another signature that missed should've been trained positively
};

PTEntry PT[CORES][1 << PCS][2 << OFFSETS];

struct HTEntry { //entry in the GHB
    uint pc; //row index in PT
    uint pctag; //tag for PT
    uint page; //bucket index for set-associative page offset table
    uint pagetag; //tag for page table
    uint offset; //offset of the access
    bool counted; //whether this access incremented the counter in the page offset table or not
    bool valid; //is this entry valid
};

constexpr HTEntry DEFHTentry = {DEFPC, DEFPCTAG, DEFPAGE, DEFPAGETAG, DEFOFFSET, false, false}; //invalid entry

HTEntry HT[CORES][1 << HIST]; //GHB
uint cur[CORES]; //head/tail of the GHB

struct PageEntry { //entry in the page offset table
    uint tag; //tag of the page(part not used to index the set/bucket)
    uint count; //number of offsets in this page with non zero counters, used for evicted entries
    uint counts[1 << OFFSETS]; //counter for each offset in the page, each with COUNTS many bits
};

PageEntry Buckets[CORES][1 << BUCKETS][1 << ASSOC]; // set-associative page offset counter table

PageEntry* getPageEntry(uint page, uint tag, uint core) { //given page and pagetag, return pointer to pageEntry
    PageEntry* bucket = Buckets[core][page];
    for (uint x = 0; x < 1 << ASSOC; x++) {
        if (bucket[x].tag == tag) { //if an entry is found with the same tag return that
            return bucket + x;
        }
    }
    for (uint x = 0; x < 1 << ASSOC; x++) {
        if (bucket[x].count == 0) { //if an empty entry is found, return that
            return bucket + x;
        }
    }
    return nullptr; //otherwise return null
}

void CACHE::prefetcher_initialize() {
    std::cout << NAME << "\tS" << std::endl;
    for (uint z = 0; z < CORES; z++) {
        cur[z] = 0;
        for (uint x = 0; x < 1 << PCS; x++) {
            for (uint y = 0; y < 2 << OFFSETS; y++) {
                PT[z][x][y] = {DEFACC, DEFPCTAG, 0};
            }
        }
        for (uint x = 0; x < 1 << HIST; x++) {
            HT[z][x] = DEFHTentry;
        }
        for (uint x = 0; x < 1 << BUCKETS; x++) {
            for (uint y = 0; y < 1 << ASSOC; y++) {
                Buckets[z][x][y].tag = DEFPAGETAG;
                Buckets[z][x][y].count = 0;
                for (uint w = 0; w < 1 << OFFSETS; w++) {
                    Buckets[z][x][y].counts[w] = 0;
                }
            }
        }
    }
}

uint32_t CACHE::prefetcher_cache_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, uint8_t type, uint32_t metadata_in, ulong _a, ulong _b) {
    addr = getAddr(addr); //convert the address to block-addressable (divide by 64 to remove the trailing 0s)
    if (type == WRITEBACK || type == PREFETCH || type == TRANSLATION) { //don't do anything on writebacks or prefetches or translations
        return metadata_in;
    }
    ulong page = getPage(addr);
    uint offset = getOffset(addr);
    uint pc = pcHash(ip); //index in the PT
    uint pctag = pcTag(ip);

    //prefetch all offsets in the same page that hit in the PT and have good accuracies
    PTEntry* PTentries = PT[cpu][pc]; //row in the PT corresponding to this PC
    for (uint x = 0; x < 1 << OFFSETS; x++) {
        uint stride = x - offset + (1 << OFFSETS);
        uint index = getIndex(stride, pctag); //index of this stride in the PT
        PTEntry PTentry = PTentries[index];
        if (PTentry.tag != pctag || PTentry.acc < PREFACC || stride == 0) { //if miss, or accuracy too low, or trying to prefetch itself, don't
            continue;
        }
        prefetch_line(getByteAddr(getAddr(page, x), 0), get_occupancy(0, 0) <= get_size(0, 0) * MAXMSHR, metadata_in); //prefetch the offset in the page, fill or not depending on the MSHR occupancy
    }

    //determine if the access should be sampled or not, 2^-SAMPLECHANCE chance
    if (page & ((1 << SAMPLECHANCE) - 1)) {
        return metadata_in; //don't sample this access
    }
    page >>= SAMPLECHANCE; //remove trailing 0s
    cur[cpu]++; //increment head/tail of GHB
    cur[cpu] &= (1 << HIST) - 1; //wrap around
    bool usegran = !(cur[cpu] & ((1 << USEGRAN) - 1)); //should useful counters be updated, 2^-USEGRAN chance

    HTEntry HTentry = HT[cpu][cur[cpu]]; //entry in the GHB that will be evicted
    PageEntry* pageEntry = getPageEntry(HTentry.page, HTentry.pagetag, cpu); //pageEntry of the victim

    //decrement the counter in the page offset table of the victim
    if (HTentry.valid && HTentry.counted && pageEntry != nullptr && pageEntry->tag == HTentry.pagetag) { //only dec if vicitm is valid and was counted and the pageEntry search hit
        uint* count = pageEntry->counts + HTentry.offset;
        if (*count > 0) { //avoid underflow
            (*count)--;
            if (*count == 0) { //if after decrementing, the counter went to 0, decrement the counter for number of non-0 counters for that pageEntry
                pageEntry->count--;
            }
        }
    }

    //train the accuracies in the PT for the victim
    PTentries = PT[cpu][HTentry.pc]; //row of the victim
    for (uint x = 0; x < 1 << OFFSETS; x++) { //loop through all offsets to check count in the page offset table and train the stride
        uint stride = x - HTentry.offset + (1 << OFFSETS);
        if (!HTentry.valid || stride == 0) { //if the victim is invalid, or trying to train delta 0, don't
            continue;
        }
        PTEntry* PTentry = PTentries + getIndex(stride, HTentry.pctag); //entry in the PT correspoding to the stride from the victim to x
        if (pageEntry == nullptr || pageEntry->tag != HTentry.pagetag || pageEntry->counts[x] == 0) { //if the page wasn't found or the counter for offset x is 0, lower accuracy
            if (PTentry->tag != HTentry.pctag) { //entry in PT table wasn't found either, don't do anything
                continue;
            }
            PTentry->acc = detrainAcc(PTentry->acc); //lower accuracy
            continue;
        }
        //positively train accuracy in PT
        if (PTentry->tag != HTentry.pctag) { //missed in PT table
            if (PTentry->use > 0) { //if the entry allocated, was useful, make it less useful
                if (usegran) {
                    PTentry->use--;
                }
                continue;
            }
            PTentry->tag = HTentry.pctag; //overtake the useless entry in the PT
            if (PTentry->acc > DEFACC) { //limit the accuracy to DEFACC
                PTentry->acc = DEFACC;
            }
        }
        if (!(cur[cpu] & (1 << (USEGRAN - 1))) && PTentry->use < MAXUSE) {
            PTentry->use++; //the entry was useful, because it was trained up
        }    
        PTentry->acc = trainAcc(PTentry->acc); //positively train the accuracy
    }

    //add current load to the GHB and update the counter
    uint pagetag = (page >> BUCKETS) & ((1 << PAGETAGS) - 1); //tag of the page of this load(part not used to index the bucket)
    page &= (1 << BUCKETS) - 1; //bucket index of the page
    pageEntry = getPageEntry(page, pagetag, cpu);
    if (pageEntry == nullptr) { //if there is no space in the Page offset table for this new entry, can't allocate a new entry in GHB either
        HT[cpu][cur[cpu]] = DEFHTentry; //invalid entry
        return metadata_in;
    }
    if (pageEntry->tag != pagetag) { //if page offset table missed, but there was an entry wiht a count of 0(useless), overtake that entry
        pageEntry->tag = pagetag;
    }
    uint* count = pageEntry->counts + offset; //counter for the offset
    if (*count == 0) { //if the count was at 0 before, increment the number of non 0 counters for the pageEntry
        pageEntry->count++;
    }
    (*count)++;
    bool counted = *count < 1 << COUNTS; //if the counter overflowed, mark the GHB entry as not having incremented the counter
    if (!counted) {
        *count = (1 << COUNTS) - 1; //saturate the counter
    }
    HT[cpu][cur[cpu]] = {pc, pctag, (uint)page, pagetag, offset, counted, true}; //alocate new GHB entry
    return metadata_in;
}

uint32_t CACHE::prefetcher_cache_fill(uint64_t addr, uint32_t set, uint32_t way, uint8_t prefetch, uint64_t evicted_addr, uint32_t metadata_in, ulong _a) {
    addr = getAddr(addr);
    evicted_addr = getAddr(evicted_addr);
    return metadata_in;
}

void CACHE::prefetcher_cycle_operate() {}

void CACHE::prefetcher_final_stats() {}

#undef int
#undef uint
#undef long
#undef ulong

void CACHE::pref_pprefetcherDg_external_cache_fill(CACHE* a, unsigned long b, unsigned long c) {}

void CACHE::pref_pprefetcherDg_broadcast_accuracy(unsigned int a) {}

void CACHE::pref_pprefetcherDg_broadcast_bw(unsigned char a) {}

void CACHE::pref_pprefetcherDg_broadcast_ipc(unsigned char a) {}
