#include "lsp.hh"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include <sys/wait.h>
#include <unistd.h>

namespace klsp
{

std::vector<std::string> split_command(const std::string& command)
{
    return parse_shell_words(command);
}

// Parse a shell-ish word list: single/double quotes, backslash escapes,
// and adjacent-quote concatenation ("a'b c'd" -> "ab cd").
std::vector<std::string> parse_shell_words(const std::string& text)
{
    std::vector<std::string> out;
    std::string current;
    bool in_arg = false;
    char quote = 0;

    for (size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (quote)
        {
            if (c == quote)
                quote = 0;
            else if (c == '\\' and quote == '"' and i + 1 < text.size())
                current += text[++i];
            else
                current += c;
        }
        else if (c == '\'' or c == '"')
        {
            quote = c;
            in_arg = true;
        }
        else if (std::isspace((unsigned char)c))
        {
            if (in_arg)
            {
                out.push_back(current);
                current.clear();
                in_arg = false;
            }
        }
        else
        {
            current += c;
            in_arg = true;
        }
    }
    if (in_arg)
        out.push_back(current);
    return out;
}

std::string shell_quote(const std::string& text)
{
    std::string out = "'";
    for (char c : text)
    {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

std::string to_file_uri(const std::string& path)
{
    static const char* hex = "0123456789abcdef";
    std::string uri = "file://";
    for (unsigned char c : path)
    {
        if (std::isalnum(c) or strchr("/-._~", c))
            uri += (char)c;
        else
        {
            uri += '%';
            uri += hex[c >> 4];
            uri += hex[c & 0xf];
        }
    }
    return uri;
}

Server::Server(std::vector<std::string> argv, const std::string& cwd)
{
    if (argv.empty())
        throw std::runtime_error("empty server command");

    int to_child[2];
    int from_child[2];
    if (::pipe(to_child) != 0 or ::pipe(from_child) != 0)
        throw std::runtime_error(std::string{"pipe: "} + strerror(errno));

    m_pid = ::fork();
    if (m_pid < 0)
        throw std::runtime_error(std::string{"fork: "} + strerror(errno));

    if (m_pid == 0)
    {
        ::dup2(to_child[0], STDIN_FILENO);
        ::dup2(from_child[1], STDOUT_FILENO);
        ::close(to_child[0]);
        ::close(to_child[1]);
        ::close(from_child[0]);
        ::close(from_child[1]);
        if (not cwd.empty())
            ::chdir(cwd.c_str());

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (auto& a : argv)
            args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }

    ::close(to_child[0]);
    ::close(from_child[1]);
    m_to_server = to_child[1];
    m_from_server = from_child[0];

    m_reader = std::thread([this] { reader_loop(); });
}

Server::~Server()
{
    if (m_to_server >= 0)
        ::close(m_to_server);
    m_to_server = -1;
    if (m_from_server >= 0)
        ::close(m_from_server);
    m_from_server = -1;
    if (m_reader.joinable())
        m_reader.join();
    if (m_pid > 0)
    {
        int status = 0;
        ::waitpid(m_pid, &status, 0);
    }
}

void Server::write_all(const char* data, size_t size)
{
    size_t written = 0;
    while (written < size)
    {
        ssize_t n = ::write(m_to_server, data + written, size - written);
        if (n <= 0)
            throw std::runtime_error("failed to write to language server");
        written += (size_t)n;
    }
}

void Server::send_message(const json& msg)
{
    std::string body = msg.dump();
    std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    std::lock_guard<std::mutex> lock{m_write_mutex};
    if (m_to_server < 0)
        return;
    write_all(header.data(), header.size());
    write_all(body.data(), body.size());
}

std::string Server::read_exact(size_t size)
{
    while (m_buffer.size() < size)
    {
        char tmp[8192];
        ssize_t n = ::read(m_from_server, tmp, sizeof(tmp));
        if (n <= 0)
            throw std::runtime_error("language server closed the connection");
        m_buffer.append(tmp, (size_t)n);
    }
    std::string out = m_buffer.substr(0, size);
    m_buffer.erase(0, size);
    return out;
}

json Server::read_message()
{
    std::string headers;
    for (;;)
    {
        size_t nl = m_buffer.find("\r\n");
        while (nl == std::string::npos)
        {
            char tmp[8192];
            ssize_t n = ::read(m_from_server, tmp, sizeof(tmp));
            if (n <= 0)
                throw std::runtime_error("language server closed the connection");
            m_buffer.append(tmp, (size_t)n);
            nl = m_buffer.find("\r\n");
        }
        std::string line = m_buffer.substr(0, nl);
        m_buffer.erase(0, nl + 2);
        if (line.empty())
            break;
        headers += line;
        headers += "\n";
    }

    auto pos = headers.find("Content-Length:");
    if (pos == std::string::npos)
        throw std::runtime_error("missing Content-Length header");
    pos += strlen("Content-Length:");
    size_t end = headers.find('\n', pos);
    size_t length = std::stoul(headers.substr(pos, end - pos));
    return json::parse(read_exact(length));
}

void Server::reader_loop()
{
    try
    {
        for (;;)
        {
            json msg = read_message();
            std::lock_guard<std::mutex> lock{m_state_mutex};
            if (msg.contains("method"))
            {
                if (msg.contains("id") and not msg["id"].is_null())
                {
                    // Server -> client request; answer null so it does not block.
                    json reply = {{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", nullptr}};
                    send_message(reply);
                }
                else
                    m_notifications.push_back(std::move(msg));
            }
            else if (msg.contains("id") and msg["id"].is_number_integer())
            {
                int id = msg["id"].get<int>();
                m_responses[id] = std::move(msg);
                m_cv.notify_all();
            }
        }
    }
    catch (const std::exception& e)
    {
        std::lock_guard<std::mutex> lock{m_state_mutex};
        m_closed = true;
        m_cv.notify_all();
        std::cerr << "[klsp] server reader stopped: " << e.what() << "\n";
    }
}

void Server::notify(const std::string& method, const json& params)
{
    send_message({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

json Server::request(const std::string& method, const json& params)
{
    int id;
    {
        std::lock_guard<std::mutex> lock{m_state_mutex};
        id = ++m_next_id;
    }
    send_message({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});

    std::unique_lock<std::mutex> lock{m_state_mutex};
    m_cv.wait(lock, [&] { return m_responses.count(id) > 0 or m_closed; });
    if (m_responses.count(id) == 0)
        throw std::runtime_error("language server closed during request");
    json msg = std::move(m_responses[id]);
    m_responses.erase(id);
    lock.unlock();

    if (msg.contains("error"))
        throw std::runtime_error("LSP error: " + msg["error"].dump());
    return msg.value("result", json());
}

std::vector<json> Server::drain_notifications()
{
    std::lock_guard<std::mutex> lock{m_state_mutex};
    std::vector<json> out;
    out.swap(m_notifications);
    return out;
}

} // namespace klsp
