#include "kak.hh"
#include "lsp.hh"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace klsp
{

Kak::Kak(std::string session, bool debug)
    : m_session(std::move(session)), m_debug(debug)
{
}

std::string Kak::temp_path(const char* tag)
{
    const char* dir = std::getenv("TMPDIR");
    std::string base = dir and *dir ? dir : "/tmp";
    std::string path = base + "/kakoune-lsp-cpp-" + std::to_string(::getpid()) + "-" +
                       tag + "-" + std::to_string(++m_counter);
    return path;
}

void Kak::pipe(const std::string& commands)
{
    int to_child[2];
    if (::pipe(to_child) != 0)
        throw std::runtime_error(std::string{"pipe: "} + strerror(errno));

    pid_t pid = ::fork();
    if (pid < 0)
        throw std::runtime_error(std::string{"fork: "} + strerror(errno));

    if (pid == 0)
    {
        ::dup2(to_child[0], STDIN_FILENO);
        ::close(to_child[0]);
        ::close(to_child[1]);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (not m_debug and devnull >= 0)
        {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
        }
        // Don't leak the daemon's fds (server pipes, request fifo) into the client.
        for (int fd = 3; fd < 256; ++fd)
            ::close(fd);
        const char* argv[] = {"kak", "-p", m_session.c_str(), nullptr};
        ::execvp("kak", const_cast<char**>(argv));
        ::_exit(127);
    }

    ::close(to_child[0]);
    size_t written = 0;
    while (written < commands.size())
    {
        ssize_t n = ::write(to_child[1], commands.data() + written, commands.size() - written);
        if (n <= 0)
            break;
        written += (size_t)n;
    }
    ::close(to_child[1]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (m_debug)
        std::cerr << "[klsp] kak -p exit " << WEXITSTATUS(status)
                  << " for: " << commands.substr(0, 120) << "\n";
}

void Kak::run(const std::string& commands)
{
    pipe(commands);
}

std::vector<std::string> Kak::query(const std::string& client,
                                    const std::vector<std::string>& exprs)
{
    std::string tmp = temp_path("query");
    std::string exprs_joined;
    for (auto& e : exprs)
    {
        exprs_joined += e;
        exprs_joined += ' ';
    }

    std::string cmds =
        "evaluate-commands -client " + shell_quote(client) + " %{ echo -to-file " +
        shell_quote(tmp) + " -quoting shell -- " + exprs_joined + " }\n";
    pipe(cmds);

    std::ifstream in(tmp, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    ::unlink(tmp.c_str());
    return parse_shell_words(ss.str());
}

std::string Kak::buffer_text(const std::string& client)
{
    std::string tmp = temp_path("buf");
    std::string cmds =
        "evaluate-commands -client " + shell_quote(client) +
        " %{ evaluate-commands -no-hooks %{ write -force " + shell_quote(tmp) + " } }\n";
    pipe(cmds);

    std::ifstream in(tmp, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    ::unlink(tmp.c_str());
    return ss.str();
}

void Kak::echo_markup(const std::string& client, const std::string& markup)
{
    std::string tmp = temp_path("msg");
    {
        std::ofstream out(tmp, std::ios::binary);
        out << markup;
    }
    // %sh{ cat <tmp>; rm <tmp> } avoids quoting arbitrary markup into the
    // Kakoune command line, and lets the server delete the file after it has
    // read it (kak -p returns before the expansion completes, so we must not
    // unlink it ourselves).
    std::string cmds = "evaluate-commands -client " + shell_quote(client) +
                       " %{ echo -markup %sh{ cat " + shell_quote(tmp) + "; rm -f " +
                       shell_quote(tmp) + " } }\n";
    if (m_debug)
        std::cerr << "[klsp] echo_markup: " << markup.substr(0, 100) << "\n";
    pipe(cmds);
}

} // namespace klsp
