#include <vx/core/version.hpp>

#include <iostream>

int main() {
    std::cout << "Perdix " << vx::version_string() << '\n';
    return 0;
}
