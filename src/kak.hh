#pragma once

#include <string>
#include <vector>

namespace klsp
{

// Talks back to a running Kakoune session with `kak -p`.
class Kak
{
public:
    Kak(std::string session, bool debug = false);

    // Run raw Kakoune commands in the server context.
    void run(const std::string& commands);

    // Evaluate `exprs` (e.g. "%val{buffile}") in the given client's context
    // and return the resulting words (Kakoune `-quoting shell`).
    std::vector<std::string> query(const std::string& client,
                                   const std::vector<std::string>& exprs);

    // Current buffer contents for a client, including unsaved changes.
    std::string buffer_text(const std::string& client);

    // Show a (single-line) markup message on a client's status line.
    void echo_markup(const std::string& client, const std::string& markup);

private:
    void pipe(const std::string& commands);
    std::string temp_path(const char* tag);

    std::string m_session;
    bool m_debug;
    int m_counter = 0;
};

} // namespace klsp
