#include "utils.h"


namespace Utility {
    std::string WstringToString(const std::wstring& wstr) {
        if (wstr.empty()) {
            return "";
        }

        // Determine the required buffer size
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (size_needed == 0) {
            // Handle error, e.g., GetLastError()
            return "";
        }

        // Allocate buffer and perform the conversion
        std::vector<char> buffer(size_needed);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &buffer[0], size_needed, nullptr, nullptr);

        return std::string(buffer.begin(), buffer.end() - 1); // Exclude null terminator
    }
}