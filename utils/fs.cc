#include <filesystem>
#include <fstream>
#include <regex>

#include <utils/fs.h>

using namespace std;
using namespace utils;

string fs::readFile(const string& path) {
    if (path.empty()) {
        return {};
    }
    constexpr auto read_size = size_t{4096};
    auto stream = ifstream{path.data()};
    stream.exceptions(ios_base::badbit);

    auto out = string{};
    auto buf = string(read_size, '\0');
    while (stream.read(&buf[0], read_size)) {
        out.append(buf, 0, stream.gcount());
    }
    out.append(buf, 0, stream.gcount());
    erase(out, '\r');
    return out;
}

string fs::readFile(const string& path, const uint32_t startLine, const uint32_t endLine) {
    if (path.empty() || startLine > endLine) {
        return {};
    }
    auto stream = ifstream{path.data()};
    stream.exceptions(ios_base::badbit);

    string line, out;
    uint32_t count{};

    while (getline(stream, line)) {
        ++count;
        if (count <= startLine) {
            continue;
        }
        if (count > endLine + 1) {
            break;
        }
        out.append(line).append("\n");
    }
    erase(out, '\r');
    return out;
}
