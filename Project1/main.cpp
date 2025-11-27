#include "globals.h"
#include <iostream>
#include <ctime>
#include <cstdlib>

int main() {
    srand(static_cast<unsigned int>(time(0)));
    std::cout << "Welcome to CSOPESY Emulator CLI\n";
    std::cout << "Developers: Go, Michael Joseph | Go, Michael Anthony | Magaling, Zoe | Uy, Matthew\n";
    std::cout << "Version date: 11/5/25\n\n";

    inputLoop();
    std::cout << "Exiting CSOPESY Emulator...\n";
}
