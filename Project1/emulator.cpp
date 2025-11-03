#include "globals.h"
#include "instruction.h"
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

// == Forward Declaration ==
void logInstructionTrace(Process& p, const std::string& instr);

// === Global variables ===
std::mutex io_mutex;
bool initialized = false;
Config systemConfig;
ConsoleMode mode = ConsoleMode::MAIN;
std::vector<Process> processTable;
int nextPID = 1;
std::string current_process = "";
std::atomic<bool> schedulerRunning{ false };
std::thread schedulerThread;
unsigned long long global_tick = 0;


// === Utility functions ===
std::vector<std::string> tokenize(const std::string& input) {
    std::istringstream stream(input);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) tokens.push_back(token);
    return tokens;
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
        else if (key == "scheduler") systemConfig.scheduler = value;
        else if (key == "quantum-cycles") systemConfig.quantum_cycles = std::stoi(value);
        else if (key == "batch-process-freq") systemConfig.batch_process_freq = std::stoi(value);
        else if (key == "min-ins") systemConfig.min_ins = std::stoi(value);
        else if (key == "max-ins") systemConfig.max_ins = std::stoi(value);
        else if (key == "delays-per-exec") systemConfig.delays_per_exec = std::stoi(value);
    }

    file.close();

    if (systemConfig.num_cpu <= 0 || systemConfig.scheduler.empty()) {
        std::cout << "Invalid config. Regenerating defaults.\n";
        generateDefaultConfig(filename);
        return loadConfigFile(filename);
    }

    systemConfig.loaded = true;
    return true;
}

// === Process helpers ===
Process* findProcess(const std::string& name) {
    for (auto& p : processTable)
        if (p.name == name)
            return &p;
    return nullptr;
}

// Execute a single instruction for a process
void executeInstruction(Process& p) {
    if (p.state == ProcessState::FINISHED || p.pc >= p.instructions.size()) {
        p.state = ProcessState::FINISHED;
        return;
    }

    auto& instr = p.instructions[p.pc];
    instr->execute(p);
    logInstructionTrace(p, instr->toString());

    p.pc++;
    if (p.pc >= p.instructions.size())
        p.state = ProcessState::FINISHED;
}


// Generate dummy instructions for a process 
std::vector<std::string> generateDummyInstructions(int count) {
    std::vector<std::string> ins;
    static std::vector<std::string> pool = {
        "DECLARE(x, 5)", "ADD(y, x, 2)", "SUBTRACT(x, y, 1)",
        "PRINT('Hello world!')", "SLEEP(2)"
    };
    for (int i = 0; i < count; ++i)
        ins.push_back(pool[rand() % pool.size()]);
    return ins;
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
        if (findProcess(name)) {
            std::cout << "Process " << name << " already exists.\n";
            return;
        }
        Process newProc;
        newProc.name = name;
        newProc.pid = nextPID++;
        newProc.state = ProcessState::READY;
        int insCount = rand() % (systemConfig.max_ins - systemConfig.min_ins + 1) + systemConfig.min_ins;
        std::vector<std::string> dummy = generateDummyInstructions(insCount);
        for (const auto& line : dummy) {
            auto instr = parseInstruction(line);
            if (instr) newProc.instructions.push_back(std::move(instr));
        }


        processTable.push_back(std::move(newProc));

        std::cout << "Created new process: " << name << " (PID " << newProc.pid << ")\n";
        std::cout << "Attached to process screen.\n";

        mode = ConsoleMode::PROCESS;
        current_process = name;
    }
    // --- Reattach to an existing process ---
    else if (flag == "-r" && args.size() >= 3) {
        std::string name = args[2];
        Process* p = findProcess(name);
        if (!p) {
            std::cout << "Process " << name << " not found.\n";
            return;
        }

        if (p->state == ProcessState::FINISHED) {
            std::cout << "Process " << name << " already finished.\n";
            return;
        }

        std::cout << "Reattached to process " << name << " (PID " << p->pid << ")\n";
        mode = ConsoleMode::PROCESS;
        current_process = name;
    }

    // --- List all processes ---
    else if (flag == "-ls") {
        if (processTable.empty()) {
            std::cout << "No processes created.\n";
            return;
        }

        int totalCores = systemConfig.num_cpu;
        int runningCount = 0, finishedCount = 0, readyCount = 0, sleepingCount = 0;

        for (const auto& p : processTable) {
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
        for (const auto& p : processTable) {
            std::string stateStr;
            switch (p.state) {
            case ProcessState::READY: stateStr = "READY"; break;
            case ProcessState::RUNNING: stateStr = "RUNNING"; break;
            case ProcessState::SLEEPING: stateStr = "SLEEPING"; break;
            case ProcessState::FINISHED: stateStr = "FINISHED"; break;
            }
            std::cout << "  " << p.name << " [PID " << p.pid << "] - "
                << stateStr << " (" << p.pc << "/" << p.instructions.size() << ")\n";
        }
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
    trace << "[" << timestamp.str() << "] "
        << "[Tick " << global_tick << "] "
        << p.name << " [PID " << p.pid << "] pc="
        << p.pc << "/" << p.instructions.size()
        << " -> " << instr
        << " | State=" << stateStr << "\n";

    trace.close();
}

// scheduler-start
// === Single-core round robin scheduler ===
void schedulerStartCommand() {
    if (!initialized) {
        std::cout << "Error: System not initialized. Type 'initialize' first.\n";
        return;
    }

    if (schedulerRunning.load()) {
        std::cout << "Scheduler already running.\n";
        return;
    }

    std::cout << "Starting single-core scheduler...\n";
    schedulerRunning.store(true);

    schedulerThread = std::thread([]() {
        std::deque<Process*> readyQueue;

        // Initialize ready queue
        for (auto& p : processTable)
            if (p.state == ProcessState::READY)
                readyQueue.push_back(&p);

        unsigned long long tick = 0;

        while (schedulerRunning.load()) {
            tick++;
            global_tick = tick;

            // --- Wake sleeping processes ---
            for (auto& p : processTable) {
                if (p.state == ProcessState::SLEEPING) {
                    if (p.sleep_counter > 0) p.sleep_counter--;
                    if (p.sleep_counter == 0) {
                        p.state = ProcessState::READY;
                        readyQueue.push_back(&p);
                    }
                }
            }

            // --- If no ready process, idle ---
            if (readyQueue.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // --- Pick next ready process (RR) ---
            Process* p = readyQueue.front();
            readyQueue.pop_front();

            // Assign a full quantum
            int remainingQuantum = systemConfig.quantum_cycles;
            p->state = ProcessState::RUNNING;

            // --- Run up to N ticks or until sleep/finish ---
            while (schedulerRunning.load() && remainingQuantum > 0) {
                tick++;
                global_tick = tick;

                // Execute one instruction per tick
                size_t before_pc = p->pc;
                executeInstruction(*p);

                if (p->pc > before_pc)
                    logInstructionTrace(*p, p->instructions[p->pc - 1]->toString());

                // Handle process end or sleep
                if (p->state == ProcessState::FINISHED || p->pc >= p->instructions.size()) {
                    p->state = ProcessState::FINISHED;
                    break;
                }
                if (p->state == ProcessState::SLEEPING) {
                    // Yield CPU immediately if process sleeps
                    break;
                }

                remainingQuantum--;

                // Apply busy-wait delay (logical ticks)
                for (int i = 0; i < systemConfig.delays_per_exec; ++i) {
                    tick++;
                    global_tick = tick;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            // --- Return to ready queue if not finished/sleeping ---
            if (p->state == ProcessState::RUNNING) {
                p->state = ProcessState::READY;
                readyQueue.push_back(p);
            }
        }

        schedulerRunning.store(false);
        std::cout << "\nScheduler stopped.\n";
        });

    schedulerThread.detach();
}


// scheduler-stop
void schedulerStopCommand() {
    if (!schedulerRunning.load()) {
        std::cout << "Scheduler is not running.\n";
        return;
    }

    schedulerRunning.store(false);
    std::cout << "Stopping scheduler...\n";
}

// report-util (simulated)
void reportUtilCommand() {
    if (!initialized) {
        std::cout << "Error: System not initialized.\n";
        return;
    }
    std::cout << "Generating CPU utilization report (simulated)...\n";
    std::cout << "CPU Utilization: 83%\nCores Used: 3/4\nFinished Processes: 5\n";
    std::cout << "Saved to csopesy-log.txt\n";
}

// process-smi inside process screen
void processSmiCommand() {
    Process* proc = findProcess(current_process);
    if (!proc) {
        std::cout << "Error: Process " << current_process << " not found.\n";
        return;
    }

    std::cout << "\n=== Process SMI ===\n";
    std::cout << "Name: " << proc->name << "\n";
    std::cout << "PID: " << proc->pid << "\n";

    // Translate enum to string
    std::string stateStr;
    switch (proc->state) {
    case ProcessState::READY: stateStr = "READY"; break;
    case ProcessState::RUNNING: stateStr = "RUNNING"; break;
    case ProcessState::SLEEPING: stateStr = "SLEEPING"; break;
    case ProcessState::FINISHED: stateStr = "FINISHED"; break;
    }
    std::cout << "State: " << stateStr << "\n";

    // Instruction progress
    std::cout << "Instruction progress: " << proc->pc << " / " << proc->instructions.size() << "\n";

    // Display variables
    if (!proc->variables.empty()) {
        std::cout << "Variables:\n";
        for (const auto& [var, val] : proc->variables)
            std::cout << "  " << var << " = " << val << "\n";
    }
    else {
        std::cout << "Variables: (none)\n";
    }

    // Display logs
    if (!proc->logs.empty()) {
        std::cout << "Logs:\n";
        for (const auto& log : proc->logs)
            std::cout << "  " << log << "\n";
    }
    else {
        std::cout << "Logs: (none)\n";
    }

    // Display instructions
    if (!proc->instructions.empty()) {
        std::cout << "Instructions:\n";
        for (size_t i = 0; i < proc->instructions.size(); ++i) {
            std::cout << "  [" << i << "] ";
            if (i == proc->pc)
                std::cout << "-> ";  // indicates the current instruction
            std::cout << proc->instructions[i]->toString() << "\n";
        }
    }
    else {
        std::cout << "Instructions: (none)\n";
    }

    // Finished message
    if (proc->state == ProcessState::FINISHED)
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
        //Skip if the user entered only whitespace
        if (tokens.empty()) {
            continue;
        }
        std::string cmd = tokens[0];

        if (mode == ConsoleMode::MAIN) {
            if (cmd == "help") {
                std::cout << "Available commands:\n"
                    << "  initialize        - Load configuration\n"
                    << "  screen            - Create or manage processes\n"
                    << "  scheduler-start   - Begin dummy process generation\n"
                    << "  scheduler-stop    - Stop scheduler\n"
                    << "  report-util       - Generate CPU report\n"
                    << "  exit              - Quit program\n";
            }
            else if (cmd == "initialize") initializeCommand();
            else if (cmd == "screen") handleScreenCommand(tokens);
            else if (cmd == "scheduler-start") schedulerStartCommand();
            else if (cmd == "scheduler-stop") schedulerStopCommand();
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
        else if (mode == ConsoleMode::PROCESS) {
            if (cmd == "process-smi") processSmiCommand();
            else if (cmd == "step") {
                Process* p = findProcess(current_process);
                if (!p) {
                    std::cout << "No active process.\n";
                    continue;
                }
                executeInstruction(*p);
                std::cout << "Executed instruction " << p->pc << " for process " << p->name << ".\n";
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