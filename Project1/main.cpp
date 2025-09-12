#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>

std::atomic<bool> marquee_running{ false };
std::thread marquee_thread;

void displayWelcomeMessage() {
	std::cout << "Welcome to CSOPESY" << std::endl; 
	std::cout << std::endl;
	std::cout << "Group developer:" << std::endl;
	std::cout << "Go, Michael Joseph" << std::endl;
	std::cout << "Go, Michael Anthony" << std::endl;
	std::cout << std::endl;
	std::cout << "Version date: 9/12/25" << std::endl;
}

void marquee() {
    const std::string text = "Hello World!";
    std::string display = text + " ";

    while (marquee_running.load(std::memory_order_relaxed)) {
        std::cout << "\r" << display << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        display = display.substr(1) + display[0];
    }
    std::cout << "\r" << std::string(display.size(), ' ') << "\r" << std::flush; 
}

void inputLoop() {
	std::string input;
	while (true) {
		std::cout << "CSOPESY> ";
		std::getline(std::cin, input);
		if (input == "exit") {
			if (marquee_running.load(std::memory_order_relaxed)) {
				marquee_running.store(false, std::memory_order_relaxed);
				if (marquee_thread.joinable()) {
					marquee_thread.join();
				}
			}
			break;
		}

		else if (input == "start_marquee") {
			if (marquee_running.load(std::memory_order_relaxed)) {
				std::cout << "Marquee already running." << std::endl;
			}
			else {
				marquee_running.store(true, std::memory_order_relaxed);
				marquee_thread = std::thread(marquee);
				std::cout << "Started marquee. Type 'stop_marquee' to stop." << std::endl;
			}
		}
		else if (input == "stop_marquee") {
			if (!marquee_running.load(std::memory_order_relaxed)) {
				std::cout << "Marquee is not running." << std::endl;
			}
			else {
				marquee_running.store(false, std::memory_order_relaxed);
				if (marquee_thread.joinable()) {
					marquee_thread.join();                
				}
				std::cout << "\nStopped marquee." << std::endl;
			}
		}

		else if(input == "help") {
			std::cout << "Available commands: help, start_marquee, exit" << std::endl;
		}
		else
			std::cout << "You entered: " << input << std::endl;
	}
}

int main() {
	displayWelcomeMessage();
	inputLoop();
}