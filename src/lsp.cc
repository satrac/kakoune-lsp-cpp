#include "lsp.hh"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace klsp
{

std::vector<std::string> split_command(const std::string& command)
{
    std::vector<std::string> out;
    std::string current;
    bool in_arg = false;
    char quote = 0;

    for (size_t i = 0; i < command.size(); ++i)
    {
        char c = command[i];
        if (quote)
        {
            if (c == quote)
                quote = 0;
            else if (c == '\\' && quote == '"' and i + 1 < command.size())
                current += command[++i];
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

static void close_fd(int& fd)
{
    if (fd >= 0)
    {
        ::close(fd);
        fd = -1;
    }
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
        // Child: wire stdin/stdout to the pipes.
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

    // Parent.
    ::close(to_child[0]);
    ::close(from_child[1]);
    m_to_server = to_child[1];
    m_from_server = from_child[0];
}

Server::~Server()
{
    close_fd(m_to_server);
    close_fd(m_from_server);
    close_fd(m_err);
    if (m_pid > 0)
    {
        int status = 0;
        ::waitpid(m_pid, &status, WNOHANG);
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
    // Read headers up to the blank line.
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

void Server::notify(const std::string& method, const json& params)
{
    send_message({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

json Server::request(const std::string& method, const json& params)
{
    int id = ++m_next_id;
    send_message({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});

    for (;;)
    {
        json msg = read_message();
        if (msg.contains("method"))
        {
            // Server -> client request: answer with null so it does not block.
            if (msg.contains("id"))
                send_message({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", nullptr}});
            continue;
        }
        if (msg.value("id", -1) == id)
        {
            if (msg.contains("error"))
                throw std::runtime_error("LSP error: " + msg["error"].dump());
            return msg.value("result", json());
        }
    }
}

} // namespace klsp
