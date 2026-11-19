#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: EmbedBinary <input> <output.cpp> <symbol>\n";
        return 1;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 2;
    std::filesystem::create_directories(std::filesystem::path(argv[2]).parent_path());
    std::ofstream output(argv[2], std::ios::trunc);
    if (!output) return 3;
    output << "#include <cstddef>\nextern const unsigned char " << argv[3] << "[] = {\n";
    unsigned char byte = 0;
    size_t count = 0;
    while (input.read(reinterpret_cast<char*>(&byte), 1)) {
        if (count % 16 == 0) output << "    ";
        output << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte) << ',';
        if (++count % 16 == 0) output << '\n';
    }
    if (count % 16 != 0) output << '\n';
    output << "};\nextern const size_t " << argv[3] << "_size = " << std::dec << count << ";\n";
    return input.eof() ? 0 : 4;
}
