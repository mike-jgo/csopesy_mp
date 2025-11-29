#pragma once
#include <vector>
#include <unordered_map>
#include <mutex>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <deque>

// Forward declaration
class Process;

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
    // In demand paging, we might just initialize the page table.
    void initializePageTable(Process& p, int required_pages);

    // Debug/SMI helper
    size_t getFreeFrameCount() const;
    size_t getTotalFrames() const;
    
    // Backing store simulation: pid + page_num -> content
    // We can use a simple map for this simulation.
    // Key: "pid:page_num", Value: vector<int> (size = frame_size)
    std::unordered_map<std::string, std::vector<int>> backing_store;

private:
    size_t total_frames;
    size_t frame_size;
    std::vector<FrameTableEntry> frame_table;
    std::vector<int> ram; // Physical memory: size = total_frames * frame_size
    std::mutex mem_mutex;

    // Helper to handle page fault
    bool handlePageFault(int pid, int page_num);
    
    // Helper to find a free frame or evict a victim
    int allocateFrame();
    
    // Helper to evict a victim page (LRU)
    int evictVictim();

    // Access the global process table to update page tables
    // (We might need a way to access the process table or pass the process object)
    // For now, we assume we can access the global processTable or pass necessary info.
};
