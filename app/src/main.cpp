#include <vx/core/version.hpp>

#include <iostream>

int main() {
    std::cout << "Voxel Engine " << vx::version_string() << '\n';
    return 0;
}
