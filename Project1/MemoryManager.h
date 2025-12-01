#pragma once
#include <vector>
#include <unordered_map>
#include <mutex>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <deque>
#include <string>

// Forward declaration
class Process;

// Global VMStat Counters
struct VMStatCounters {
    unsigned long long pages_paged_in = 0;
    unsigned long long pages_paged_out = 0;
};

struct PageTableEntry {
    int frame_num = -1;
    bool valid = false;
    bool dirty = false;
    unsigned long long last_accessed = 0; // For LRU
};

struct FrameTableEntry {
    int pid = -1;
    int page_num = -1;
    bool occupied = false;
};

class MemoryManager {
public:
    MemoryManager(size_t total_frames, size_t frame_size);

    // Returns true if access successful (or page fault handled), false if error
    bool access(int pid, int virtual_addr, bool write, int& value);

    // Allocate frames for a process (called on process creation)
    void initializePageTable(Process& p, int required_pages);

    // Helper to check if a page is resident
    bool isPageResident(int pid, int virtual_addr);

    // Debug/SMI helper
    size_t getFreeFrameCount() const;
    size_t getTotalFrames() const;
    size_t getUsedMemory() const;

    // VMStat Helpers
    VMStatCounters getVMStat();

    // Backing store simulation
    // Key: "pid:page_num", Value: vector<int> (size = frame_size)
    std::unordered_map<std::string, std::vector<int>> backing_store;

private:
    size_t total_frames;
    size_t frame_size;
    std::vector<FrameTableEntry> frame_table;
    std::vector<int> ram; // Physical memory: size = total_frames * frame_size
    std::mutex mem_mutex;

    // VMStat internal counters
    VMStatCounters stats;

    // Helper to handle page fault
    bool handlePageFault(int pid, int page_num);

    // Helper to find a free frame or evict a victim
    int allocateFrame();

    // Helper to evict a victim page (LRU)
    int evictVictim();

    // Save backing store to file
    void flushBackingStore();
};