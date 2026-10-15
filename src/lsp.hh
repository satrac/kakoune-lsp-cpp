#pragma once

#include "json.hh"

#include <string>
#include <vector>

namespace klsp
{

using json = nlohmann::json;

// Split a command line into argv. Supports single/double quotes and
// backslash escapes; good enough for `clangd --background-index` and friends.
std::vector<std::string> split_command(const std::string& command);

// Convert an absolute path to a file:// URI.
std::string to_file_uri(const std::string& absolute_path);

// A language server subprocess speaking LSP over stdin/stdout.
class Server
{
public:
    Server(std::vector<std::string> argv, const std::string& cwd = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void notify(const std::string& method, const json& params = json::object());
    json request(const std::string& method, const json& params = json::object());

    void send_message(const json& msg);
    json read_message();

    long pid() const { return m_pid; }

private:
    void write_all(const char* data, size_t size);
    std::string read_exact(size_t size);

    int m_to_server = -1;
    int m_from_server = -1;
    int m_err = -1;
    long m_pid = -1;
    int m_next_id = 0;
    std::string m_buffer;
};

} // namespace klsp
