#pragma once

#include "json.hh"

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace klsp
{

using json = nlohmann::json;

std::vector<std::string> split_command(const std::string& command);
std::vector<std::string> parse_shell_words(const std::string& text);
std::string to_file_uri(const std::string& absolute_path);

// POSIX shell single-quote a string (safe for `sh -c` and Kakoune %sh).
std::string shell_quote(const std::string& text);

// A language server subprocess speaking LSP over stdin/stdout.
// A background thread reads messages, fulfils responses and queues
// server-initiated notifications for the daemon to drain.
class Server
{
public:
    Server(std::vector<std::string> argv, const std::string& cwd = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void notify(const std::string& method, const json& params = json::object());
    json request(const std::string& method, const json& params = json::object());

    // Move any queued server notifications out. Safe to call from the
    // daemon thread only.
    std::vector<json> drain_notifications();

private:
    void send_message(const json& msg);
    void write_all(const char* data, size_t size);
    json read_message();
    std::string read_exact(size_t size);
    void reader_loop();

    int m_to_server = -1;
    int m_from_server = -1;
    long m_pid = -1;
    int m_next_id = 0;
    std::string m_buffer;

    std::thread m_reader;
    std::mutex m_write_mutex;   // guards writes to the server stdin
    std::mutex m_state_mutex;   // guards responses/notifications/closed
    std::condition_variable m_cv;
    std::map<int, json> m_responses;
    std::vector<json> m_notifications;
    bool m_closed = false;
};

} // namespace klsp
