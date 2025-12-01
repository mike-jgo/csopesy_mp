#include "Instruction.h"
#include "globals.h"
#include <iostream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <cctype>

// === Utility functions ===
std::string trim(const std::string& str) {
    const auto first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    const auto last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
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

// Helper to parse string that might be decimal or hex (0x...)
int parseAddressOrValue(const std::string& token) {
    try {
        return std::stoi(token, nullptr, 0); // 0 base auto-detects 0x
    } catch (...) {
        return -1; // Indicator for failure
    }
}

// Helper to resolve a token to either an integer or a variable value
bool getValueFromMemory(Process& p, const std::string& token, int& outVal) {
    // 1. Try parsing as literal integer
    try {
        outVal = std::stoi(token);
        return true; 
    } catch (...) {}

    // 2. Look up variable in Symbol Table
    if (p.symbol_table.find(token) == p.symbol_table.end()) {
        // Variable not found (runtime error in strict mode, or 0 in loose mode)
        outVal = 0;
        return true;
    }

    int addr = p.symbol_table[token];

    // 3. Access Memory (Read)
    // We assume 2-byte integers. We read the address. 
    // (Simplification: we only read 1 word/int at that address)
    if (!memoryManager->access(p.pid, addr, false, outVal)) {
        return false; // Page Fault triggered
    }

    return true; // Success
}

// Helper to set a variable value in memory
bool setValueToMemory(Process& p, const std::string& varName, int value) {
    int addr;

    // 1. Check if variable exists
    if (p.symbol_table.find(varName) == p.symbol_table.end()) {
        // Allocate new address in Page 0
        addr = p.symbol_cursor;
        p.symbol_table[varName] = addr;

        // === FIX: Increment by 2 bytes (16-bit integer simulation) ===
        // This ensures variables are stored at 0, 2, 4, etc.
        p.symbol_cursor += 2;
    }
    else {
        addr = p.symbol_table[varName];
    }

    // 2. Access Memory (Write)
    // We attempt to write to the specific address.
    // Note: logic implies we write to 'addr', and 'addr+1' is effectively "occupied" by this int.
    int temp = value;
    if (!memoryManager->access(p.pid, addr, true, temp)) {
        return false; // Page Fault triggered
    }

    return true;
}

// Helper to clamp uint16
int clampUint16(int val) {
    if (val < 0) return 0;
    if (val > 65535) return 65535;
    return val;
}

// === Implementations ===

DeclareInstruction::DeclareInstruction(const std::string& v, int value) : var(v), val(value) {}
void DeclareInstruction::execute(Process& p) {
    if (setValueToMemory(p, var, clampUint16(val))) {
        p.pc++;
    }
}
std::string DeclareInstruction::toString() const {
    return "DECLARE(" + var + ", " + std::to_string(val) + ")";
}

AddInstruction::AddInstruction(const std::string& t, const std::string& o1, const std::string& o2)
    : target(t), op1(o1), op2(o2) {}
void AddInstruction::execute(Process& p) {
    int v1, v2;
    if (!getValueFromMemory(p, op1, v1)) return;
    if (!getValueFromMemory(p, op2, v2)) return; 
    int result = clampUint16(v1 + v2);
    if (setValueToMemory(p, target, result)) {
        p.pc++;
    }
}
std::string AddInstruction::toString() const {
    return "ADD(" + target + ", " + op1 + ", " + op2 + ")";
}

SubtractInstruction::SubtractInstruction(const std::string& t, const std::string& o1, const std::string& o2)
    : target(t), op1(o1), op2(o2) {}
void SubtractInstruction::execute(Process& p) {
    int v1, v2;
    if (!getValueFromMemory(p, op1, v1)) return;
    if (!getValueFromMemory(p, op2, v2)) return;
    int result = clampUint16(v1 - v2);
    if (setValueToMemory(p, target, result)) {
        p.pc++;
    }
}
std::string SubtractInstruction::toString() const {
    return "SUBTRACT(" + target + ", " + op1 + ", " + op2 + ")";
}

PrintInstruction::PrintInstruction(const std::string& expr) : expression(expr) {}
void PrintInstruction::execute(Process& p) {
    std::vector<std::string> parts = splitPrintExpr(expression);
    std::ostringstream out;
    bool stall = false;

    for (auto& part : parts) {
        if (isSingleQuoted(part)) {
            out << unquoteSingle(part);
        }
        else {
            int val;
            if (!getValueFromMemory(p, part, val)) {
                stall = true;
                break; 
            }
            out << val;
        }
    }

    if (stall) return; 

    p.logs.push_back(out.str());
    p.pc++;
}
std::string PrintInstruction::toString() const {
    return "PRINT(" + expression + ")";
}

SleepInstruction::SleepInstruction(int d) : duration(d) {}
void SleepInstruction::execute(Process& p) {
    p.sleep_counter = duration;
    p.state = ProcessState::SLEEPING;
    p.pc++;
}
std::string SleepInstruction::toString() const {
    return "SLEEP(" + std::to_string(duration) + ")";
}

ForInstruction::ForInstruction(const std::string& b, int r) : body(b), repeats(r) {}
void ForInstruction::execute(Process& p) {
    std::vector<std::shared_ptr<Instruction>> expanded;
    std::stringstream ss(body);
    std::string temp;
    while (std::getline(ss, temp, ';')) {
        auto inst = parseInstruction(temp);
        if (inst) expanded.push_back(inst);
    }

    std::vector<std::shared_ptr<Instruction>> fullExpansion;
    for (int i = 0; i < repeats; ++i) {
        for (const auto& inst : expanded) {
            fullExpansion.push_back(inst);
        }
    }

    if (p.pc < p.instructions.size()) {
        auto it = p.instructions.begin() + p.pc;
        it = p.instructions.erase(it);
        p.instructions.insert(it, fullExpansion.begin(), fullExpansion.end());
    }
}
std::string ForInstruction::toString() const {
    return "FOR([" + body + "], " + std::to_string(repeats) + ")";
}

WriteInstruction::WriteInstruction(const std::string& a, const std::string& v) : addrStr(a), valStr(v) {}
void WriteInstruction::execute(Process& p) {
    int addr = parseAddressOrValue(addrStr);
    
    int valToWrite;
    if (!getValueFromMemory(p, valStr, valToWrite)) return; 

    valToWrite = clampUint16(valToWrite);

    if (addr < 0 || addr >= p.memory_required) {
        p.state = ProcessState::MEMORY_VIOLATED;
        return;
    }

    int dummy = valToWrite;
    if (memoryManager->access(p.pid, addr, true, dummy)) {
        p.pc++;
    }
}
std::string WriteInstruction::toString() const {
    return "WRITE(" + addrStr + ", " + valStr + ")";
}

ReadInstruction::ReadInstruction(const std::string& a, const std::string& v) : addrStr(a), var(v) {}
void ReadInstruction::execute(Process& p) {
    int addr = parseAddressOrValue(addrStr);
    
    if (addr < 0 || addr >= p.memory_required) {
        p.state = ProcessState::MEMORY_VIOLATED;
        return;
    }

    int memVal;
    if (!memoryManager->access(p.pid, addr, false, memVal)) return; 

    if (setValueToMemory(p, var, clampUint16(memVal))) {
        p.pc++;
    }
}
std::string ReadInstruction::toString() const {
    return "READ(" + var + ", " + addrStr + ")";
}

// === Parsing ===

std::shared_ptr<Instruction> parseInstruction(const std::string& line) {
    std::string instr = trim(line);
    if (instr.empty()) return nullptr;

    std::smatch match;

    // DECLARE
    static std::regex declareRegex(R"(DECLARE\((\w+),\s*(-?\d+)\))");
    if (std::regex_match(instr, match, declareRegex)) {
        return std::make_shared<DeclareInstruction>(match[1], std::stoi(match[2]));
    }

    // ADD
    static std::regex addRegex(R"(ADD\((\w+),\s*([\w\-]+),\s*([\w\-]+)\))");
    if (std::regex_match(instr, match, addRegex)) {
        return std::make_shared<AddInstruction>(match[1], match[2], match[3]);
    }

    // SUBTRACT
    static std::regex subRegex(R"(SUBTRACT\((\w+),\s*([\w\-]+),\s*([\w\-]+)\))");
    if (std::regex_match(instr, match, subRegex)) {
        return std::make_shared<SubtractInstruction>(match[1], match[2], match[3]);
    }

    // PRINT
    static std::regex printRegex(R"(PRINT\((.*)\))");
    if (std::regex_match(instr, match, printRegex)) {
        return std::make_shared<PrintInstruction>(trim(match[1]));
    }

    // SLEEP
    static std::regex sleepRegex(R"(SLEEP\((\d+)\))");
    if (std::regex_match(instr, match, sleepRegex)) {
        return std::make_shared<SleepInstruction>(std::stoi(match[1]));
    }

    // FOR
    static std::regex forRegex(R"(FOR\(\[([^\]]+)\],\s*(\d+)\))");
    if (std::regex_match(instr, match, forRegex)) {
        return std::make_shared<ForInstruction>(match[1], std::stoi(match[2]));
    }

    // READ
    static std::regex readRegex(R"(READ\((\w+),\s*((?:0x[0-9a-fA-F]+|\d+))\))");
    if (std::regex_match(instr, match, readRegex)) {
        return std::make_shared<ReadInstruction>(match[2], match[1]); // Addr, Var
    }

    // WRITE
    static std::regex writeRegex(R"(WRITE\(((?:0x[0-9a-fA-F]+|\d+)),\s*([a-zA-Z0-9_]+)\))");
    if (std::regex_match(instr, match, writeRegex)) {
        return std::make_shared<WriteInstruction>(match[1], match[2]);
    }

    // === Space-Separated Syntax Support ===

    // DECLARE <var> <val>
    static std::regex declareSpaceRegex(R"(DECLARE\s+(\w+)\s+(-?\d+))");
    if (std::regex_match(instr, match, declareSpaceRegex)) {
        return std::make_shared<DeclareInstruction>(match[1], std::stoi(match[2]));
    }

    // ADD <target> <op1> <op2>
    static std::regex addSpaceRegex(R"(ADD\s+(\w+)\s+([\w\-]+)\s+([\w\-]+))");
    if (std::regex_match(instr, match, addSpaceRegex)) {
        return std::make_shared<AddInstruction>(match[1], match[2], match[3]);
    }

    // SUBTRACT <target> <op1> <op2>
    static std::regex subSpaceRegex(R"(SUBTRACT\s+(\w+)\s+([\w\-]+)\s+([\w\-]+))");
    if (std::regex_match(instr, match, subSpaceRegex)) {
        return std::make_shared<SubtractInstruction>(match[1], match[2], match[3]);
    }

    // READ <var> <addr>
    static std::regex readSpaceRegex(R"(READ\s+(\w+)\s+((?:0x[0-9a-fA-F]+|\d+)))");
    if (std::regex_match(instr, match, readSpaceRegex)) {
        return std::make_shared<ReadInstruction>(match[2], match[1]); // Addr, Var
    }

    // WRITE <addr> <val>
    static std::regex writeSpaceRegex(R"(WRITE\s+((?:0x[0-9a-fA-F]+|\d+))\s+([a-zA-Z0-9_]+))");
    if (std::regex_match(instr, match, writeSpaceRegex)) {
        return std::make_shared<WriteInstruction>(match[1], match[2]);
    }

    // SLEEP <duration>
    static std::regex sleepSpaceRegex(R"(SLEEP\s+(\d+))");
    if (std::regex_match(instr, match, sleepSpaceRegex)) {
        return std::make_shared<SleepInstruction>(std::stoi(match[1]));
    }

    return nullptr;
}
