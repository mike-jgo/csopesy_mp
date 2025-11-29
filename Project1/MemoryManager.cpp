#include "globals.h"
#include "MemoryManager.h"

std::unique_ptr<MemoryManager> memoryManager;

MemoryManager::MemoryManager(size_t total_frames, size_t frame_size)
    : total_frames(total_frames), frame_size(frame_size) {
    frame_table.resize(total_frames);
    ram.resize(total_frames * frame_size, 0); // Initialize RAM with 0
    
    for (size_t i = 0; i < total_frames; ++i) {
        frame_table[i].pid = -1;
        frame_table[i].page_num = -1;
        frame_table[i].occupied = false;
    }
}

bool MemoryManager::access(int pid, int virtual_addr, bool write, int& value) {
    std::lock_guard<std::mutex> lock(mem_mutex);

    int page_num = virtual_addr / frame_size;
    int offset = virtual_addr % frame_size;

    // Find process
    Process* proc = nullptr;
    {
        std::lock_guard<std::mutex> pLock(processTableMutex);
        for (auto& p : processTable) {
            if (p.pid == pid) {
                proc = &p;
                break;
            }
        }
    }

    if (!proc) return false;

    // Check bounds
    if (virtual_addr >= proc->memory_required) {
        std::cout << "Error: Segmentation Fault (PID " << pid << " Addr " << virtual_addr << ")\n";
        return false;
    }

    // Ensure page table entry exists
    if (proc->page_table.find(page_num) == proc->page_table.end()) {
        proc->page_table[page_num] = PageTableEntry();
    }

    PageTableEntry& pte = proc->page_table[page_num];
    pte.last_accessed = global_tick;

    if (!pte.valid) {
        // Page Fault
        // std::cout << "[MemoryManager] Page Fault: PID " << pid << " Page " << page_num << "\n";
        if (!handlePageFault(pid, page_num)) {
            std::cout << "Error: Failed to handle page fault for PID " << pid << "\n";
            return false;
        }
    }

    int frame_num = pte.frame_num;
    int phys_addr = frame_num * frame_size + offset;

    if (write) {
        ram[phys_addr] = value;
        pte.dirty = true;
        // Also update backing store immediately? No, only on eviction for "Write-Back".
        // But for simplicity/safety in simulation, we can also update backing store if we want "Write-Through".
        // Requirement says: "Write back the evicted page to the backing store if it is dirty".
        // So we stick to Write-Back.
    } else {
        value = ram[phys_addr];
    }

    return true;
}

void MemoryManager::initializePageTable(Process& p, int required_pages) {
    std::lock_guard<std::mutex> lock(mem_mutex);
    p.page_table.clear();
    for (int i = 0; i < required_pages; ++i) {
        p.page_table[i] = PageTableEntry();
    }
}

bool MemoryManager::handlePageFault(int pid, int page_num) {
    int frame_idx = allocateFrame();
    if (frame_idx == -1) return false;

    // Load from backing store
    std::string key = std::to_string(pid) + ":" + std::to_string(page_num);
    
    // If not in backing store, it's a new page (zeros)
    if (backing_store.find(key) == backing_store.end()) {
        backing_store[key] = std::vector<int>(frame_size, 0);
    }
    
    const auto& page_data = backing_store[key];
    
    // Copy to RAM
    int phys_start = frame_idx * frame_size;
    for (size_t i = 0; i < frame_size; ++i) {
        if (i < page_data.size())
            ram[phys_start + i] = page_data[i];
        else
            ram[phys_start + i] = 0;
    }

    // Update Frame Table
    frame_table[frame_idx].pid = pid;
    frame_table[frame_idx].page_num = page_num;
    frame_table[frame_idx].occupied = true;

    // Update Page Table
    // Need to find process again or pass it?
    // We have pid.
    {
        std::lock_guard<std::mutex> pLock(processTableMutex);
        for (auto& p : processTable) {
            if (p.pid == pid) {
                p.page_table[page_num].frame_num = frame_idx;
                p.page_table[page_num].valid = true;
                p.page_table[page_num].dirty = false;
                p.page_table[page_num].last_accessed = global_tick;
                break;
            }
        }
    }
    
    return true;
}

int MemoryManager::allocateFrame() {
    // 1. Find free frame
    for (size_t i = 0; i < total_frames; ++i) {
        if (!frame_table[i].occupied) {
            return static_cast<int>(i);
        }
    }

    // 2. No free frame -> Evict
    return evictVictim();
}

int MemoryManager::evictVictim() {
    // LRU Policy
    int victim_frame = -1;
    unsigned long long min_tick = -1; // Max value

    // Iterate all frames to find the one with oldest last_accessed
    for (size_t i = 0; i < total_frames; ++i) {
        int pid = frame_table[i].pid;
        int page = frame_table[i].page_num;
        
        // Access process to get last_accessed
        // This is slow (O(N*P)) but fine for simulation
        std::lock_guard<std::mutex> pLock(processTableMutex);
        Process* proc = nullptr;
        for (auto& p : processTable) {
            if (p.pid == pid) {
                proc = &p;
                break;
            }
        }
        
        if (proc) {
            if (proc->page_table.count(page)) {
                unsigned long long last = proc->page_table[page].last_accessed;
                if (last < min_tick) {
                    min_tick = last;
                    victim_frame = static_cast<int>(i);
                }
            }
        } else {
            // Process might have finished? If so, frame should be free.
            // If we find a frame pointing to a non-existent process, reclaim it immediately.
            frame_table[i].occupied = false;
            return static_cast<int>(i);
        }
    }

    if (victim_frame == -1) {
        // Should not happen if frames are occupied
        return 0; // Fallback
    }

    // Evict victim
    int v_pid = frame_table[victim_frame].pid;
    int v_page = frame_table[victim_frame].page_num;

    // std::cout << "[MemoryManager] Evicting Frame " << victim_frame 
    //           << " (PID " << v_pid << " Page " << v_page << ")\n";

    {
        std::lock_guard<std::mutex> pLock(processTableMutex);
        Process* proc = nullptr;
        for (auto& p : processTable) {
            if (p.pid == v_pid) {
                proc = &p;
                break;
            }
        }

        if (proc) {
            PageTableEntry& pte = proc->page_table[v_page];
            
            // Write back if dirty
            if (pte.dirty) {
                std::string key = std::to_string(v_pid) + ":" + std::to_string(v_page);
                std::vector<int> page_data(frame_size);
                int phys_start = victim_frame * frame_size;
                for (size_t i = 0; i < frame_size; ++i) {
                    page_data[i] = ram[phys_start + i];
                }
                backing_store[key] = page_data;
                // std::cout << "[MemoryManager] Swapped out dirty page to backing store.\n";
            }

            pte.valid = false;
            pte.frame_num = -1;
            pte.dirty = false;
        }
    }

    frame_table[victim_frame].occupied = false;
    return victim_frame;
}

size_t MemoryManager::getFreeFrameCount() const {
    size_t count = 0;
    for (const auto& f : frame_table) {
        if (!f.occupied) count++;
    }
    return count;
}

size_t MemoryManager::getTotalFrames() const {
    return total_frames;
}
