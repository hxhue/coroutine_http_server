#include <csignal>
#include <cstdlib>
#include <iostream>
 
class Tester
{
public:
    Tester()  { std::cout << "Tester ctor\n"; }
    ~Tester() { std::cout << "Tester dtor\n"; }
};
 
Tester static_tester; // Destructor not called
 
void signal_handler(int signal) 
{
    if (signal == SIGABRT)
        {
            std::cerr << "SIGABRT received\n";
            // raise(SIGTERM);
        }
    else
        std::cerr << "Unexpected signal " << signal << " received\n";
    // std::_Exit(128 + 11);
}
 
int main()
{
    Tester automatic_tester; // Destructor not called

    // std::abort();
 
    // Setup handler
    auto previous_handler = std::signal(SIGABRT, signal_handler);
    if (previous_handler == SIG_ERR)
    {
        std::cerr << "Setup failed\n";
        return EXIT_FAILURE;
    }
 
    std::abort(); // Raise SIGABRT
    std::cout << "This code is unreachable\n";
}