#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <sstream>

std::mutex io_mutex;

// CLI mode tracker
enum class ConsoleMode { MAIN, PROCESS };
ConsoleMode mode = ConsoleMode::MAIN;

// Initialization flag
bool initialized = false;

// Simulated process screen state
std::string current_process = "";

// Simple helper: split string into tokens
std::vector<std::string> tokenize(const std::string& input) {
    std::istringstream stream(input);
    std::vector<std::string> tokens;
    std::string token;
    while (stream >> token) tokens.push_back(token);
    return tokens;
}

// === COMMANDS ===
// All commands are just couts simulating behavior
// initialize command
void initializeCommand() {
    if (initialized) {
        std::cout << "System already initialized.\n";
        return;
    }
    std::cout << "Reading config.txt ... (simulated)\n";
    std::cout << "Scheduler: RR | num-cpu: 4 | quantum-cycles: 2\n";
    initialized = true;
    std::cout << "System initialized successfully.\n";
}

// screen
void handleScreenCommand(const std::vector<std::string>& args) {
    if (!initialized) {
        std::cout << "Error: System not initialized.\n";
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

    if (flag == "-s" && args.size() >= 3) {
        current_process = args[2];
        std::cout << "Created new process: " << current_process << "\n";
        std::cout << "Attaching to " << current_process << "...\n";
        mode = ConsoleMode::PROCESS;
    }
    else if (flag == "-r" && args.size() >= 3) {
        current_process = args[2];
        std::cout << "Reattaching to process: " << current_process << "\n";
        mode = ConsoleMode::PROCESS;
    }
    else if (flag == "-ls") {
        std::cout << "Listing all processes (simulated):\n";
        std::cout << "  p01 [RUNNING]\n  p02 [SLEEPING]\n  p03 [FINISHED]\n";
    }
    else {
        std::cout << "Invalid screen command.\n";
    }
}

// scheduler-start
void schedulerStartCommand() {
    if (!initialized) {
        std::cout << "Error: System not initialized.\n";
        return;
    }
    std::cout << "Scheduler started (simulated). Generating dummy processes...\n";
}

// scheduler-stop
void schedulerStopCommand() {
    if (!initialized) {
        std::cout << "Error: System not initialized.\n";
        return;
    }
    std::cout << "Scheduler stopped.\n";
}

// report-util
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
    std::cout << "Process: " << current_process << "\n";
    std::cout << "State: RUNNING\n";
    std::cout << "Instructions executed: 5/12\n";
    std::cout << "Variables in memory: x=10, y=20\n";
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
            else if (cmd == "exit") break;
            else std::cout << "Unknown command. Type 'help'.\n";
        }
        else if (mode == ConsoleMode::PROCESS) {
            if (cmd == "process-smi") processSmiCommand();
            else if (cmd == "exit") {
                std::cout << "Exiting process screen...\n";
                mode = ConsoleMode::MAIN;
                current_process.clear();
            }
            else std::cout << "Invalid command in process screen.\n";
        }
    }
}

// === MAIN ===
int main() {
    std::cout << "Welcome to CSOPESY Emulator CLI\n";
    std::cout << "Developers: Go, Michael Joseph | Go, Michael Anthony | Magaling, Zoe | Uy, Matthew\n";
    std::cout << "Version date: 11/1/25\n\n";
    inputLoop();
    std::cout << "Exiting CSOPESY Emulator...\n";
}
