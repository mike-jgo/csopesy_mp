#include "globals.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <regex>
#include <thread>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <deque>
#include <algorithm>
#include <cctype>


// === Global variables ===
std::mutex io_mutex;
std::mutex processTableMutex;
bool initialized = false;
Config systemConfig;
ConsoleMode mode = ConsoleMode::MAIN;
std::deque<Process> processTable; 
std::vector<CPUCore> cpuCores;
int nextPID = 1;
std::string current_process = "";
std::atomic<bool> autoCreateRunning{ false };
std::atomic<bool> schedulerRunning{ false };
std::thread schedulerThread;
unsigned long long global_tick = 0;
size_t rrCursor = 0;


// === Forward declarations ===
void scheduler_loop_tick();

// === Utility functions ===
std::vector<std::string> tokenize(const std::string& input) {
    std::istringstream stream(input);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) tokens.push_back(token);
    return tokens;
}

std::string trim(const std::string& str) {
    const auto first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    const auto last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
}


std::vector<std::string> expandForLoops(const std::vector<std::string>& instructions) {
    std::vector<std::string> result;
    std::regex forRegex(R"(FOR\(\[([^\]]+)\],\s*(\d+)\))");

    for (const auto& rawInstr : instructions) {
        std::string instr = trim(rawInstr);
        std::smatch match;
        if (std::regex_match(instr, match, forRegex)) {
            std::string body = match[1];
            int repeats = std::stoi(match[2]);

            std::vector<std::string> innerInstrs;
            std::stringstream ss(body);
            std::string temp;
            while (std::getline(ss, temp, ';')) {
                std::string innerTrimmed = trim(temp);
                if (!innerTrimmed.empty()) innerInstrs.push_back(innerTrimmed);
            }

            std::vector<std::string> expandedInner = expandForLoops(innerInstrs);
            for (int i = 0; i < repeats; ++i)
                result.insert(result.end(), expandedInner.begin(), expandedInner.end());
        }
        else if (!instr.empty()) {
            result.push_back(instr);
        }
    }

    return result;
}

// Split by '+' but ignore '+' inside single quotes
std::vector<std::string> splitPrintExpr(const std::string& expr) {
    std::vector<std::string> parts;
    std::string cur;
    bool inStr = false; // single-quoted string
    for (size_t i = 0; i < expr.size(); ++i) {
        char c = expr[i];
        if (c == '\'') {
            inStr = !inStr;
            cur.push_back(c);
        }
        else if (c == '+' && !inStr) {
            parts.push_back(trim(cur));
            cur.clear();
        }
        else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(trim(cur));
    return parts;
}

// Remove surrounding single quotes if present
bool isSingleQuoted(const std::string& s) {
    return s.size() >= 2 && s.front() == '\'' && s.back() == '\'';
}
std::string unquoteSingle(const std::string& s) {
    if (isSingleQuoted(s)) return s.substr(1, s.size() - 2);
    return s;
}


// === Config handling ===
bool generateDefaultConfig(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cout << "Error: Could not create " << filename << std::endl;
        return false;
    }

    file << "num-cpu 4\n";
    file << "scheduler rr\n";
    file << "quantum-cycles 2\n";
    file << "batch-process-freq 3\n";
    file << "min-ins 5\n";
    file << "max-ins 10\n";
    file << "delays-per-exec 1\n";
    file.close();

    std::cout << "Default config.txt generated with safe defaults.\n";
    return true;
}

bool loadConfigFile(const std::string& filename) {
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cout << "Warning: " << filename << " not found.\n";
        std::cout << "Creating default configuration file...\n";

        if (!generateDefaultConfig(filename)) return false;

        file.open(filename);
        if (!file.is_open()) return false;
    }

    std::string key, value;
    while (file >> key >> value) {
        if (key == "num-cpu") systemConfig.num_cpu = std::stoi(value);
        else if (key == "scheduler") {
            systemConfig.scheduler = value;
            std::transform(systemConfig.scheduler.begin(), systemConfig.scheduler.end(),
                systemConfig.scheduler.begin(),
                [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        }
        else if (key == "quantum-cycles") systemConfig.quantum_cycles = std::stoi(value);
        else if (key == "batch-process-freq") systemConfig.batch_process_freq = std::stoi(value);
        else if (key == "min-ins") systemConfig.min_ins = std::stoi(value);
        else if (key == "max-ins") systemConfig.max_ins = std::stoi(value);
        else if (key == "delays-per-exec") systemConfig.delays_per_exec = std::stoi(value);
    }

    file.close();

    if (systemConfig.scheduler != "rr" && systemConfig.scheduler != "fcfs") {
        std::cout << "Warning: Unsupported scheduler '" << systemConfig.scheduler
            << "'. Defaulting to round-robin.\n";
        systemConfig.scheduler = "rr";
    }

    // Validate basic config
    if (systemConfig.num_cpu <= 0 || systemConfig.scheduler.empty()) {
        std::cout << "Invalid config. Regenerating defaults.\n";
        generateDefaultConfig(filename);
        return loadConfigFile(filename);
    }

    // Initialize CPU cores based on config
    cpuCores.clear();
    cpuCores.resize(systemConfig.num_cpu);
    for (int i = 0; i < systemConfig.num_cpu; ++i) {
        cpuCores[i].id = i;
        cpuCores[i].running = nullptr;
        cpuCores[i].quantum_left = 0;
    }

    systemConfig.loaded = true;
    std::cout << "Loaded " << systemConfig.num_cpu << " CPU cores.\n";

    return true;
}


// === Process helpers ===
Process* findProcess(const std::string& name) {
    for (auto& p : processTable)
        if (p.name == name)
            return &p;
    return nullptr;
}

// Resolve a token to either an integer or a variable value
int resolveValue(Process& p, const std::string& token) {
    try {
        return std::stoi(token);
    }
    catch (...) {
       
    }

    if (!p.variables.count(token)) {
        p.variables[token] = 0;
    }
    return p.variables[token];
}

// Execute a single instruction for a process
void executeInstruction(Process& p) {
    if (p.state == ProcessState::FINISHED) return;
    if (p.pc >= p.instructions.size()) {
        p.state = ProcessState::FINISHED;
        return;
    }

    std::string instr = p.instructions[p.pc];
    bool advancePc = true;

    // === DECLARE(var, value) ===
    if (instr.rfind("DECLARE", 0) == 0) {
        std::regex re(R"(DECLARE\((\w+),\s*(-?\d+)\))");
        std::smatch match;
        if (std::regex_match(instr, match, re)) {
            std::string var = match[1];
            int value = std::stoi(match[2]);
            p.variables[var] = value;
        }
    }

    // === ADD(var1, var2/value, var3/value) ===
    else if (instr.rfind("ADD", 0) == 0) {
        std::regex re(R"(ADD\((\w+),\s*([\w\-]+),\s*([\w\-]+)\))");
        std::smatch match;
        if (std::regex_match(instr, match, re)) {
            std::string target = match[1];
            std::string op1 = match[2];
            std::string op2 = match[3];

            int val1 = resolveValue(p, op1);
            int val2 = resolveValue(p, op2);
            p.variables[target] = val1 + val2;
        }
    }

    // === SUBTRACT(var1, var2/value, var3/value) ===
    else if (instr.rfind("SUBTRACT", 0) == 0) {
        std::regex re(R"(SUBTRACT\((\w+),\s*([\w\-]+),\s*([\w\-]+)\))");
        std::smatch match;
        if (std::regex_match(instr, match, re)) {
            std::string target = match[1];
            std::string op1 = match[2];
            std::string op2 = match[3];

            int val1 = resolveValue(p, op1);
            int val2 = resolveValue(p, op2);
            p.variables[target] = val1 - val2;
        }
    }

    // === PRINT('message') ===
    else if (instr.rfind("PRINT", 0) == 0) {
        // Match anything inside PRINT(...)
        std::regex re(R"(PRINT\((.*)\))");
        std::smatch match;
        if (std::regex_match(instr, match, re)) {
            std::string expr = trim(match[1]);

            // Break the expression by '+' while respecting quoted segments
            std::vector<std::string> parts = splitPrintExpr(expr);
            std::ostringstream out;

            for (auto& part : parts) {
                // If it's a quoted string, append literal content
                if (isSingleQuoted(part)) {
                    out << unquoteSingle(part);
                }
                else {
                    try {
                        // Try integer literal first
                        int v = std::stoi(part);
                        out << v;
                    }
                    catch (...) {
                        // Fallback to variable
                        int v = resolveValue(p, part);
                        out << v;
                    }
                }
            }

            p.logs.push_back(out.str());
        }
    }


    // === SLEEP(n) ===
    else if (instr.rfind("SLEEP", 0) == 0) {
        std::regex re(R"(SLEEP\((\d+)\))");
        std::smatch match;
        if (std::regex_match(instr, match, re)) {
            int n = std::stoi(match[1]);
            p.sleep_counter = n;
            p.state = ProcessState::SLEEPING;
        }
    }

    // === FOR([...], repeats) ===
    else if (instr.rfind("FOR", 0) == 0) {
        std::vector<std::string> expanded = expandForLoops(std::vector<std::string>{ instr });
        std::string trimmedInstr = trim(instr);
        if (expanded.size() != 1 || expanded[0] != trimmedInstr) {
            p.instructions.erase(p.instructions.begin() + p.pc);
            p.instructions.insert(p.instructions.begin() + p.pc, expanded.begin(), expanded.end());
            advancePc = false; // stay on the first expanded instruction for the next tick
        }
    }



    // Advance PC
    if (advancePc) {
        p.pc++;
    }

    if (systemConfig.delays_per_exec > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(systemConfig.delays_per_exec));

    if (p.pc >= p.instructions.size()) {
        p.state = ProcessState::FINISHED;
    }
}

// Generate dummy instructions for a process 
std::vector<std::string> generateDummyInstructions(int count) {
    std::vector<std::string> ins;
    static std::vector<std::string> pool = {
        "DECLARE(x, 5)",
        "DECLARE(y, 10)",
        "ADD(sum, x, y)",
        "SUBTRACT(diff, y, x)",
        "PRINT('Hello world!')",
        "PRINT('Value of sum: ' + sum)",
        "SLEEP(2)",
        "FOR([DECLARE(i,1); ADD(sum, sum, 1); PRINT('Loop iteration ' + sum)], 3)"
    };
    for (int i = 0; i < count; ++i)
        ins.push_back(pool[rand() % pool.size()]);
    return ins;
}

// Generates alternating PRINT and ADD instructions
std::vector<std::string> generateAlternatingInstructions(int count) {
    std::vector<std::string> instructions;
    if (count <= 0) return instructions;

    for (int i = 0; i < count; ++i) {
        if (i % 2 == 0) {
            // PRINT instruction
            instructions.push_back("PRINT('Value from: ' + x)");
        }
        else {
            // ADD instruction with randomized operand between 1 and 10
            int randomValue = (rand() % 10) + 1;
            instructions.push_back("ADD(x, x, " + std::to_string(randomValue) + ")");
        }
    }

    return instructions;
}

// Ensure the scheduler thread is running
void ensureSchedulerActive() {
    if (!schedulerRunning.load() && initialized) {
        schedulerRunning.store(true);
        schedulerThread = std::thread([]() {
            while (schedulerRunning.load()) {

                bool shouldTick = false;
                {
                    std::lock_guard<std::mutex> lock(processTableMutex);
                    shouldTick = std::any_of(processTable.begin(), processTable.end(),
                        [](const Process& p) {
                            return p.state == ProcessState::READY ||
                                p.state == ProcessState::RUNNING ||
                                p.state == ProcessState::SLEEPING;
                        });
                }

                if (shouldTick || autoCreateRunning.load()) {
                    scheduler_loop_tick(); // always tick sleeping processes
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                bool shouldStop = false;
                {
                    std::lock_guard<std::mutex> lock(processTableMutex);
                    bool allFinished = !processTable.empty() &&
                        std::all_of(processTable.begin(), processTable.end(),
                            [](const Process& p) { return p.state == ProcessState::FINISHED; });

                    if (allFinished && !autoCreateRunning.load()) {
                        schedulerRunning.store(false);
                        shouldStop = true;
                    }
                }

                if (shouldStop) {
                    std::cout << "[Tick " << global_tick
                        << "] Scheduler halted (all processes finished).\n";
                    break;
                }
            }
            });
        schedulerThread.detach();
        std::cout << "Scheduler thread started.\n";
    }
}

// === COMMANDS ===
// Some commands are just couts simulating behavior
// initialize command
void initializeCommand() {
    if (systemConfig.loaded) {
        std::cout << "System already initialized.\n";
        return;
    }

    std::cout << "Initializing system from config.txt...\n";

    if (!loadConfigFile("config.txt")) {
        std::cout << "Initialization failed. Please check config.txt.\n";
        return;
    }

    initialized = true;
    std::cout << "Configuration loaded successfully:\n";
    std::cout << "  num-cpu: " << systemConfig.num_cpu << "\n";
    std::cout << "  scheduler: " << systemConfig.scheduler << "\n";
    std::cout << "  quantum-cycles: " << systemConfig.quantum_cycles << "\n";
    std::cout << "  batch-process-freq: " << systemConfig.batch_process_freq << "\n";
    std::cout << "  instruction range: " << systemConfig.min_ins << "-" << systemConfig.max_ins << "\n";
    std::cout << "  delays-per-exec: " << systemConfig.delays_per_exec << "\n";
    std::cout << "System initialization complete.\n\n";
}


// screen
void handleScreenCommand(const std::vector<std::string>& args) {
    if (!initialized) {
        std::cout << "Error: System not initialized. Type 'initialize' first.\n";
        return;
    }
    if (args.size() == 1) {
        std::cout << "Usage:\n"
            << "  screen -s <process_name>\n"
            << "  screen -r <process_name>\n"
            << "  screen -ls\n";
        return;
    }
    std::string flag = args[1];

    // --- Create new process ---
    if (flag == "-s" && args.size() >= 3) {
        std::string name = args[2];
        Process newProc;
        newProc.name = name;
        newProc.state = ProcessState::READY;
        int insCount = rand() % (systemConfig.max_ins - systemConfig.min_ins + 1) + systemConfig.min_ins;
        newProc.instructions = generateDummyInstructions(insCount);

        {
            std::lock_guard<std::mutex> lock(processTableMutex);
            if (findProcess(name)) {
                std::cout << "Process " << name << " already exists.\n";
                return;
            }
            newProc.pid = nextPID++;
            processTable.push_back(newProc);
        }

        std::cout << "Created new process: " << name << " (PID " << newProc.pid << ")\n";
        std::cout << "Attached to process screen.\n";
        ensureSchedulerActive();

        mode = ConsoleMode::PROCESS;
        current_process = name;
    }
    // --- Reattach to an existing process ---
    else if (flag == "-r" && args.size() >= 3) {
        std::string name = args[2];
        bool found = false;
        bool finished = false;
        int pid = 0;
        {
            std::lock_guard<std::mutex> lock(processTableMutex);
            Process* p = findProcess(name);
            if (p) {
                found = true;
                pid = p->pid;
                finished = (p->state == ProcessState::FINISHED);
            }
        }

        if (!found) {
            std::cout << "Process " << name << " not found.\n";
            return;
        }

        if (finished) {
            std::cout << "Process " << name << " already finished.\n";
            return;
        }

        std::cout << "Reattached to process " << name << " (PID " << pid << ")\n";
        mode = ConsoleMode::PROCESS;
        current_process = name;
    }

    // --- List all processes ---
    else if (flag == "-ls") {
        std::deque<Process> snapshot;
        {
            std::lock_guard<std::mutex> lock(processTableMutex);
            if (processTable.empty()) {
                std::cout << "No processes created.\n";
                return;
            }
            snapshot.assign(processTable.begin(), processTable.end());
        }

        int totalCores = systemConfig.num_cpu;
        int runningCount = 0, finishedCount = 0, readyCount = 0, sleepingCount = 0;

        for (const auto& p : snapshot) {
            switch (p.state) {
            case ProcessState::RUNNING:   runningCount++; break;
            case ProcessState::READY:     readyCount++; break;
            case ProcessState::SLEEPING:  sleepingCount++; break;
            case ProcessState::FINISHED:  finishedCount++; break;
            }
        }

        float utilization = (totalCores > 0)
            ? (float)runningCount / totalCores * 100.0f
            : 0.0f;

        std::cout << "\n=== CPU SUMMARY ===\n";
        std::cout << "CPU Utilization: " << utilization << "%\n";
        std::cout << "Cores Used: " << runningCount << "/" << totalCores << "\n";
        std::cout << "Cores Available: " << (totalCores - runningCount) << "\n";
        std::cout << "Ready: " << readyCount
            << " | Sleeping: " << sleepingCount
            << " | Finished: " << finishedCount << "\n";

        std::cout << "\n=== PROCESS TABLE ===\n";

        // Print all RUNNING and SLEEPING processes first
        for (const auto& p : snapshot) {
            if (p.state == ProcessState::RUNNING || p.state == ProcessState::SLEEPING) {
                std::string stateStr = (p.state == ProcessState::RUNNING ? "RUNNING" : "SLEEPING");
                std::cout << "  " << p.name << " [PID " << p.pid << "] - "
                    << stateStr << " (" << p.pc << "/" << p.instructions.size() << ")\n";
            }
        }

        // Show next 4 READY processes based on actual scheduler order
        std::vector<Process*> readyList;

        if (systemConfig.scheduler == "rr" && !snapshot.empty()) {
            size_t total = snapshot.size();
            size_t added = 0;
            for (size_t offset = 0; offset < total && added < 4; ++offset) {
                size_t idx = (rrCursor + offset) % total;
                if (snapshot[idx].state == ProcessState::READY) {
                    readyList.push_back(&snapshot[idx]);
                    added++;
                }
            }
        }
        else if (systemConfig.scheduler == "fcfs") {
            for (auto& p : snapshot) {
                if (p.state == ProcessState::READY) {
                    readyList.push_back(&p);
                    if (readyList.size() >= 4) break;
                }
            }
        }

        // Display the READY list
        for (auto* p : readyList) {
            std::cout << "  " << p->name << " [PID " << p->pid << "] - READY ("
                << p->pc << "/" << p->instructions.size() << ")\n";
        }

        if (runningCount == 0 && sleepingCount == 0 && readyList.empty())
            std::cout << "  (No active or upcoming processes)\n";

        std::cout << "=====================\n\n";
    }
}

// Trace function
void logInstructionTrace(Process& p, const std::string& instr) {
    std::ofstream trace("csopesy-trace.txt", std::ios::app);
    if (!trace.is_open()) return;

    // Real timestamp
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    #ifdef _WIN32
        localtime_s(&tm_buf, &now_time);
    #else
        localtime_r(&now_time, &tm_buf);
    #endif

    std::ostringstream timestamp;
    timestamp << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");

    // Process state as string
    std::string stateStr;
    switch (p.state) {
    case ProcessState::READY: stateStr = "READY"; break;
    case ProcessState::RUNNING: stateStr = "RUNNING"; break;
    case ProcessState::SLEEPING: stateStr = "SLEEPING"; break;
    case ProcessState::FINISHED: stateStr = "FINISHED"; break;
    }

    // Combined log entry
    trace << "[" << timestamp.str() << "] ";

    trace << "[Tick " << global_tick;
    if (systemConfig.scheduler == "rr" && systemConfig.quantum_cycles > 0) {
        int quantumPos = (p.pc % systemConfig.quantum_cycles) + 1;
        trace << " | Q" << quantumPos << "/" << systemConfig.quantum_cycles;
    }
    else if (systemConfig.scheduler == "fcfs") {
        trace << " | FCFS";
    }
    trace << "] "
        << p.name << " [PID " << p.pid << "] pc="
        << p.pc << "/" << p.instructions.size()
        << " -> " << instr
        << " | State=" << stateStr << "\n";
    trace.close();
}

// Scheduler-start/stop command handler
void handleSchedulerCommand(const std::vector<std::string>& args) {
    if (!initialized) {
        std::cout << "Error: System not initialized. Type 'initialize' first.\n";
        return;
    }

    if (args.size() == 1) {
        std::cout << "Usage:\n"
            << "  scheduler start\n"
            << "  scheduler stop\n";
        return;
    }

    const std::string& subcmd = args[1];

    if (subcmd == "start") {
        if (autoCreateRunning.load()) {
            std::cout << "Auto-creation is already running (every "
                << systemConfig.batch_process_freq << " tick"
                << (systemConfig.batch_process_freq == 1 ? "" : "s") << ").\n";
            // Still ensure the scheduler thread is alive.
            ensureSchedulerActive();
            return;
        }
        autoCreateRunning.store(true);

        ensureSchedulerActive();

        std::cout << "Auto-creation started new process every "
            << systemConfig.batch_process_freq << " tick"
            << (systemConfig.batch_process_freq == 1 ? "" : "s") << ".\n";
    }
    else if (subcmd == "stop") {
        if (!autoCreateRunning.load()) {
            std::cout << "Auto-creation is not running.\n";
            return;
        }
        autoCreateRunning.store(false);
        std::cout << "Auto-creation stopped.\n";

        // Do NOT kill the scheduler thread here it should keep ticking sleepers
        // and will auto-halt when all processes reach FINISHED.
        bool allFinished = false;
        {
            std::lock_guard<std::mutex> lock(processTableMutex);
            allFinished = !processTable.empty() &&
                std::all_of(processTable.begin(), processTable.end(),
                    [](const Process& p) { return p.state == ProcessState::FINISHED; });
        }

        if (allFinished) {
            schedulerRunning.store(false);
            std::cout << "All processes finished scheduler halted.\n";
        }
    }
    else {
        std::cout << "Invalid command. Use 'scheduler start' or 'scheduler stop'.\n";
    }
}


// scheduler-start
// === Multi-core scheduler (RR / FCFS) ===
void scheduler_loop_tick() {
    global_tick++;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::lock_guard<std::mutex> lock(processTableMutex);
    // === 1. Wake up sleeping processes ===
    for (auto& p : processTable) {
        if (p.state == ProcessState::SLEEPING && p.sleep_counter > 0) {
            p.sleep_counter--;
            if (p.sleep_counter == 0) {
                p.state = ProcessState::READY;
            }
        }
    }

    // === 2. Assign ready processes to idle cores ===
    for (auto& core : cpuCores) {
        if (!core.running || core.running->state == ProcessState::FINISHED) {
            core.running = nullptr;
        }
    }

    size_t tableSize = processTable.size();
    if (tableSize == 0) {
        rrCursor = 0;
    }

    for (auto& core : cpuCores) {
        if (core.running) {
            continue;
        }

        tableSize = processTable.size();
        if (tableSize == 0) {
            rrCursor = 0;
            core.running = nullptr;
            continue;
        }

        if (systemConfig.scheduler == "rr" && tableSize > 0) {
            if (rrCursor >= tableSize) {
                rrCursor %= tableSize;
            }
        }

        size_t chosenIndex = tableSize;
        if (systemConfig.scheduler == "rr") {
            for (size_t offset = 0; offset < tableSize; ++offset) {
                size_t idx = (rrCursor + offset) % tableSize;
                if (processTable[idx].state == ProcessState::READY) {
                    chosenIndex = idx;
                    rrCursor = (idx + 1) % tableSize;
                    break;
                }
            }
        }
        else {
            for (size_t idx = 0; idx < tableSize; ++idx) {
                if (processTable[idx].state == ProcessState::READY) {
                    chosenIndex = idx;
                    break;
                }
            }
        }
        if (chosenIndex != tableSize) {
            core.running = &processTable[chosenIndex];
            processTable[chosenIndex].state = ProcessState::RUNNING;
            core.quantum_left = (systemConfig.scheduler == "rr")
                ? systemConfig.quantum_cycles
                : 0;
        }
    }

    // === 3. Execute processes on each core ===
    for (auto& core : cpuCores) {
        if (core.running && core.running->state == ProcessState::RUNNING) {
            Process* p = core.running;

            // Execute one instruction = one tick
            if (p->pc < p->instructions.size()) {
                logInstructionTrace(*p, p->instructions[p->pc]);
                executeInstruction(*p);
                if (systemConfig.scheduler == "rr") {
                    core.quantum_left--;
                }

                // Handle post-execution logic
                if (p->state == ProcessState::FINISHED) {
                    core.running = nullptr;
                }
                else if (p->state == ProcessState::SLEEPING) {
                    core.running = nullptr;
                }
                else if (systemConfig.scheduler == "rr" &&
                    core.quantum_left <= 0) {
                    // Quantum expired — check if another READY process exists
                    bool hasOtherReady = std::any_of(processTable.begin(), processTable.end(),
                        [&](const Process& other) {
                            return other.state == ProcessState::READY &&
                                &other != p;
                        });

                    if (hasOtherReady) {
                        p->state = ProcessState::READY;
                        core.running = nullptr; // Preempt
                        core.quantum_left = systemConfig.quantum_cycles;

                        auto it = std::find_if(processTable.begin(), processTable.end(),
                            [&](Process& candidate) { return &candidate == p; });
                        if (it != processTable.end()) {
                            size_t idx = static_cast<size_t>(std::distance(processTable.begin(), it));
                            size_t tableSize = processTable.size();
                            if (tableSize > 0) {
                                rrCursor = (idx + 1) % tableSize;
                            }
                            else {
                                rrCursor = 0;
                            }
                        }
                    }
                    else {
                        // No other ready — keep executing
                        core.quantum_left = systemConfig.quantum_cycles;
                    }
                }
            }
        }
    }

    // === 4. Auto-create processes if enabled ===
    if (autoCreateRunning.load() &&
        systemConfig.batch_process_freq > 0 &&
        global_tick % systemConfig.batch_process_freq == 0) {
    
        // Only create one process per batch frequency, not potentially multiple
        static unsigned long long lastCreationTick = 0;
    
        if (global_tick != lastCreationTick) {
            Process newProc;
            newProc.name = "auto_p" + std::to_string(nextPID++);
            newProc.pid = nextPID - 1;
            newProc.state = ProcessState::READY;
            int insCount = rand() % (systemConfig.max_ins - systemConfig.min_ins + 1) + systemConfig.min_ins;
            newProc.instructions = generateDummyInstructions(insCount);
            processTable.push_back(newProc);
        
            lastCreationTick = global_tick;
        }
    }
}

// report-util 
void reportUtilCommand()
{
    if (!initialized)
    {
        std::cout << "Error: System not initialized. Type 'initialize' first.\n";
        return;
    }

    int running = 0, ready = 0, sleeping = 0, finished = 0;
    {
        std::lock_guard<std::mutex> lock(processTableMutex);
        for (auto& p : processTable)
        {
            switch (p.state)
            {
            case ProcessState::RUNNING:   running++; break;
            case ProcessState::READY:     ready++; break;
            case ProcessState::SLEEPING:  sleeping++; break;
            case ProcessState::FINISHED:  finished++; break;
            }
        }
    }

    float utilization = (systemConfig.num_cpu > 0)
        ? (float)running / systemConfig.num_cpu * 100.0f
        : 0.0f;

    std::cout << "\n=== CPU UTILIZATION REPORT ===\n";
    std::cout << "CPU Utilization: " << utilization << "%\n";
    std::cout << "Cores Used: " << running << "/" << systemConfig.num_cpu << "\n";
    std::cout << "Ready: " << ready
        << " | Sleeping: " << sleeping
        << " | Finished: " << finished << "\n";
    std::cout << "Report saved to csopesy-log.txt\n";
    std::cout << "===============================\n\n";

    std::ofstream log("csopesy-log.txt");
    if (!log.is_open())
    {
        std::cout << "Error: Unable to create csopesy-log.txt\n";
        return;
    }

    log << "=== CSOPESY CPU UTILIZATION REPORT ===\n";
    log << "CPU Utilization: " << utilization << "%\n";
    log << "Cores Used: " << running << "/" << systemConfig.num_cpu << "\n";
    log << "Ready: " << ready
        << " | Sleeping: " << sleeping
        << " | Finished: " << finished << "\n";
    log << "======================================\n";

    log.close();
}

// process-smi inside process screen
void processSmiCommand() {
    Process procSnapshot;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(processTableMutex);
        Process* proc = findProcess(current_process);
        if (proc) {
            found = true;
            procSnapshot = *proc;
        }
    }

    if (!found) {
        std::cout << "Error: Process " << current_process << " not found.\n";
        return;
    }

    std::cout << "\n=== Process SMI ===\n";
    std::cout << "Name: " << procSnapshot.name << "\n";
    std::cout << "PID: " << procSnapshot.pid << "\n";

    // Translate enum to string
    std::string stateStr;
    switch (procSnapshot.state) {
    case ProcessState::READY: stateStr = "READY"; break;
    case ProcessState::RUNNING: stateStr = "RUNNING"; break;
    case ProcessState::SLEEPING: stateStr = "SLEEPING"; break;
    case ProcessState::FINISHED: stateStr = "FINISHED"; break;
    }
    std::cout << "State: " << stateStr << "\n";

    // Instruction progress
    std::cout << "Instruction progress: " << procSnapshot.pc << " / " << procSnapshot.instructions.size() << "\n";

    // Display variables
    if (!procSnapshot.variables.empty()) {
        std::cout << "Variables:\n";
        for (const auto& [var, val] : procSnapshot.variables)
            std::cout << "  " << var << " = " << val << "\n";
    }
    else {
        std::cout << "Variables: (none)\n";
    }

    // Display logs
    if (!procSnapshot.logs.empty()) {
        std::cout << "Logs:\n";
        for (const auto& log : procSnapshot.logs)
            std::cout << "  " << log << "\n";
    }
    else {
        std::cout << "Logs: (none)\n";
    }

    // === Display Instructions ===
    //if (!procSnapshot.instructions.empty()) {
    //    std::cout << "\nInstructions:\n";
    //    for (size_t i = 0; i < procSnapshot.instructions.size(); ++i) {
    //        std::cout << "  [" << std::setw(2) << i << "] ";

    //        // Highlight the current instruction
    //        if (i == procSnapshot.pc && procSnapshot.state != ProcessState::FINISHED)
    //            std::cout << ">> " << procSnapshot.instructions[i] << "  <-- current\n";
    //        else
    //            std::cout << procSnapshot.instructions[i] << "\n";
    //    }
    //}
    //else {
    //    std::cout << "\nInstructions: (none)\n";
    //}

    // Finished message
    if (procSnapshot.state == ProcessState::FINISHED)
        std::cout << "Process has finished execution.\n";

    std::cout << "=====================\n\n";
}

// === INPUT LOOP ===
void inputLoop() {
    std::string input;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(io_mutex);
            std::cout << (mode == ConsoleMode::MAIN ? "CSOPESY> " : current_process + "> ") << std::flush;
        }

        std::getline(std::cin, input);
        if (input.empty()) continue;

        std::vector<std::string> tokens = tokenize(input);
        if (tokens.empty()) continue;

        std::string cmd = tokens[0];

        // === MAIN CONSOLE MODE ===
        if (mode == ConsoleMode::MAIN) {
            if (cmd == "help") {
                std::cout << "Available commands:\n"
                    << "  initialize          - Load configuration and start scheduler\n"
                    << "  screen              - Create or manage processes\n"
                    << "  scheduler start     - Begin automatic process creation\n"
                    << "  scheduler stop      - Stop automatic process creation\n"
                    << "  report-util         - Generate CPU report\n"
                    << "  report-trace        - Show execution trace log\n"
                    << "  exit                - Quit program\n";
            }
            else if (cmd == "initialize") initializeCommand();
            else if (cmd == "screen") handleScreenCommand(tokens);
            else if (cmd == "scheduler") handleSchedulerCommand(tokens);
            else if (cmd == "report-util") reportUtilCommand();
            else if (cmd == "report-trace") {
                std::ifstream trace("csopesy-trace.txt");
                if (!trace.is_open()) {
                    std::cout << "No trace log found.\n";
                    continue;
                }
                std::cout << "\n=== EXECUTION TRACE ===\n";
                std::string line;
                while (std::getline(trace, line)) std::cout << line << "\n";
                std::cout << "=======================\n";
            }
            else if (cmd == "exit") break;
            else std::cout << "Unknown command. Type 'help'.\n";
        }

        // === PROCESS MODE ===
        else if (mode == ConsoleMode::PROCESS) {
            if (cmd == "process-smi") processSmiCommand();
            else if (cmd == "step") {
                int pcAfter = 0;
                std::string procName;
                bool found = false;
                {
                    std::lock_guard<std::mutex> lock(processTableMutex);
                    Process* p = findProcess(current_process);
                    if (p) {
                        found = true;
                        procName = p->name;
                        executeInstruction(*p);
                        pcAfter = p->pc;
                    }
                }
                if (!found) {
                    std::cout << "No active process.\n";
                    continue;
                }
                std::cout << "Executed instruction " << pcAfter
                    << " for process " << procName << ".\n";
            }
            else if (cmd == "exit") {
                std::cout << "Exiting process screen...\n";
                mode = ConsoleMode::MAIN;
                current_process.clear();
            }
            else std::cout << "Invalid command in process screen.\n";
        }
    }
}
