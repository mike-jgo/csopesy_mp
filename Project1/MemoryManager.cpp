#include "globals.h"
#include "MemoryManager.h"

// Define the global unique_ptr
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

    // Clear backing store file on startup
    std::ofstream ofs("csopesy-backing-store.txt", std::ofstream::out | std::ofstream::trunc);
    ofs.close();
}

bool MemoryManager::access(int pid, int virtual_addr, bool write, int& value) {
    std::lock_guard<std::mutex> lock(mem_mutex);

    int page_num = virtual_addr / frame_size;
    int offset = virtual_addr % frame_size;

    // Find process securely
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
    pte.last_accessed = global_tick; // Update LRU timestamp

    if (!pte.valid) {
        // Page Fault!
        // Release lock momentarily to prevent deadlock if needed, 
        // but here we hold it because handlePageFault is internal.
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
    }
    else {
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

bool MemoryManager::isPageResident(int pid, int virtual_addr) {
    std::lock_guard<std::mutex> lock(mem_mutex);
    
    // Find process securely
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

    int page_num = virtual_addr / frame_size;
    
    // Check if page entry exists and is valid (resident)
    if (proc->page_table.find(page_num) != proc->page_table.end()) {
        return proc->page_table[page_num].valid;
    }
    return false;
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

    // Update Stats
    stats.pages_paged_in++;

    // Update Frame Table
    frame_table[frame_idx].pid = pid;
    frame_table[frame_idx].page_num = page_num;
    frame_table[frame_idx].occupied = true;

    // Update Page Table
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
    int victim_frame = -1;
    unsigned long long min_tick = -1; // Max value

    // 1. Scan Frame Table to find LRU victim
    // We need to look up the Process's PageTable to get the 'last_accessed' time
    // because that's where we store it.

    std::lock_guard<std::mutex> pLock(processTableMutex);

    for (size_t i = 0; i < total_frames; ++i) {
        int pid = frame_table[i].pid;
        int page = frame_table[i].page_num;

        // Find process
        for (auto& p : processTable) {
            if (p.pid == pid) {
                if (p.page_table.count(page)) {
                    unsigned long long last = p.page_table[page].last_accessed;
                    if (last < min_tick) {
                        min_tick = last;
                        victim_frame = static_cast<int>(i);
                    }
                }
                break;
            }
        }
    }

    // Fallback if somehow not found (shouldn't happen if frames occupied)
    if (victim_frame == -1) victim_frame = 0;

    // 2. Evict the victim
    int v_pid = frame_table[victim_frame].pid;
    int v_page = frame_table[victim_frame].page_num;

    for (auto& p : processTable) {
        if (p.pid == v_pid) {
            PageTableEntry& pte = p.page_table[v_page];

            // Write back if dirty
            if (pte.dirty) {
                std::string key = std::to_string(v_pid) + ":" + std::to_string(v_page);
                std::vector<int> page_data(frame_size);
                int phys_start = victim_frame * frame_size;
                for (size_t k = 0; k < frame_size; ++k) {
                    page_data[k] = ram[phys_start + k];
                }
                backing_store[key] = page_data;

                stats.pages_paged_out++;
                flushBackingStore(); // Persist to disk
            }

            pte.valid = false;
            pte.frame_num = -1;
            pte.dirty = false;
            break;
        }
    }

    frame_table[victim_frame].occupied = false;
    return victim_frame;
}

void MemoryManager::flushBackingStore() {
    std::ofstream outFile("csopesy-backing-store.txt");
    for (const auto& [key, data] : backing_store) {
        outFile << "Page: " << key << " Data: ";
        for (int val : data) outFile << val << " ";
        outFile << "\n";
    }
    outFile.close();
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

size_t MemoryManager::getUsedMemory() const {
    return (total_frames - getFreeFrameCount()) * frame_size;
}

VMStatCounters MemoryManager::getVMStat() {
    return stats;
}