#include "daemon.hh"

#include "kak.hh"
#include "lsp.hh"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace klsp
{

namespace
{

struct Language
{
    std::string command;
    std::string language_id;
};

// Built-in defaults: filetype -> language server.
const std::map<std::string, Language>& languages()
{
    static const std::map<std::string, Language> table = {
        {"c",      {"clangd", "c"}},
        {"cpp",    {"clangd", "cpp"}},
        {"objc",   {"clangd", "objective-c"}},
        {"rust",   {"rust-analyzer", "rust"}},
        {"go",     {"gopls", "go"}},
        {"python", {"pylsp", "python"}},
        {"lua",    {"lua-language-server", "lua"}},
    };
    return table;
}

std::string dirname(const std::string& path)
{
    auto pos = path.rfind('/');
    return pos == std::string::npos ? "." : (pos == 0 ? "/" : path.substr(0, pos));
}

std::string uri_to_path(const std::string& uri)
{
    if (uri.rfind("file://", 0) != 0)
        return {};
    std::string path = uri.substr(7);
    std::string out;
    for (size_t i = 0; i < path.size(); ++i)
    {
        if (path[i] == '%' and i + 2 < path.size())
        {
            out += (char)std::stoi(path.substr(i + 1, 2), nullptr, 16);
            i += 2;
        }
        else
            out += path[i];
    }
    return out;
}

std::string single_line(std::string text)
{
    // Collapse to one line and strip markdown code fences, so `echo -markup`
    // is happy and the status line shows something readable.
    std::string out;
    for (char c : text)
    {
        if (c == '\n' or c == '\r')
            out += "  ";
        else
            out += c;
    }
    return out;
}

struct Doc
{
    std::string path;
    std::string uri;
    std::string language_id;
    std::string server_key;
    int version = 1;
};

struct ServerEntry
{
    std::shared_ptr<Server> server;
    bool usable = true;
    std::string root;
};

std::string server_key(const std::string& command, const std::string& root)
{
    return command + "\n" + root;
}

class Daemon
{
public:
    Daemon(std::string session, std::string fifo, bool debug)
        : m_session(std::move(session)), m_fifo(std::move(fifo)),
          m_debug(debug), m_kak(m_session, debug) {}

    int run();

private:
    void handle_line(const std::string& line);
    void handle_enable(const std::string& client);
    void handle_did_change(const std::string& client);
    void handle_hover(const std::string& client);
    void handle_definition(const std::string& client);
    void handle_references(const std::string& client);
    void handle_diagnostics(const json& msg);
    void drain_servers();
    Server* server_for_doc(const Doc& doc);
    json position_params(const std::string& client, const Doc& doc);

    std::string m_session;
    std::string m_fifo;
    bool m_debug;
    Kak m_kak;
    std::map<std::string, ServerEntry> m_servers;
    std::map<std::string, Doc> m_docs;   // client -> doc
    bool m_quit = false;
    std::string m_input;
};

void Daemon::handle_enable(const std::string& client)
{
    auto ctx = m_kak.query(client, {"%val{buffile}", "%opt{filetype}"});
    if (ctx.empty())
        return;
    std::string path = ctx[0];
    std::string filetype = ctx.size() > 1 ? ctx[1] : "";
    if (path.empty())
        return;

    auto lang = languages().find(filetype);
    if (lang == languages().end())
    {
        if (m_debug)
            std::cerr << "[klsp] no language server for filetype '" << filetype << "'\n";
        return;
    }

    std::string root = dirname(path);
    std::string key = server_key(lang->second.command, root);
    auto& entry = m_servers[key];
    if (not entry.server)
    {
        try
        {
            entry.root = root;
            entry.server = std::make_shared<Server>(split_command(lang->second.command), root);
            json init = {
                {"processId", (long)::getpid()},
                {"clientInfo", {{"name", "kakoune-lsp-cpp"}, {"version", "0.1.0"}}},
                {"rootUri", to_file_uri(root)},
                {"capabilities", json::parse(R"({
                    "textDocument": {
                        "synchronization": {"dynamicRegistration": false},
                        "hover": {"contentFormat": ["markdown", "plaintext"]},
                        "definition": {},
                        "references": {},
                        "rename": {"prepareSupport": false},
                        "documentFormatting": {},
                        "publishDiagnostics": {"relatedInformation": true}
                    }
                })")},
            };
            entry.server->request("initialize", init);
            entry.server->notify("initialized", json::object());
        }
        catch (const std::exception& e)
        {
            entry.usable = false;
            if (m_debug)
                std::cerr << "[klsp] failed to start " << lang->second.command
                          << ": " << e.what() << "\n";
            return;
        }
    }
    if (not entry.usable)
        return;

    std::string uri = to_file_uri(path);
    auto existing = m_docs.find(client);
    if (existing != m_docs.end() and existing->second.uri == uri)
        return;   // already open in this client
    std::string text = m_kak.buffer_text(client);
    entry.server->notify("textDocument/didOpen", {
        {"textDocument", {
            {"uri", uri},
            {"languageId", lang->second.language_id},
            {"version", 1},
            {"text", text},
        }},
    });
    m_docs[client] = Doc{path, uri, lang->second.language_id, key, 1};
    if (m_debug)
        std::cerr << "[klsp] enabled " << filetype << " (" << lang->second.command
                  << ") for " << client << "\n";
}

Server* Daemon::server_for_doc(const Doc& doc)
{
    auto it = m_servers.find(doc.server_key);
    if (it == m_servers.end() or not it->second.server or not it->second.usable)
        return nullptr;
    return it->second.server.get();
}

void Daemon::handle_did_change(const std::string& client)
{
    auto it = m_docs.find(client);
    if (it == m_docs.end())
        return;
    Doc& doc = it->second;
    Server* server = server_for_doc(doc);
    if (not server)
        return;
    std::string text = m_kak.buffer_text(client);
    ++doc.version;
    try
    {
        server->notify("textDocument/didChange", {
            {"textDocument", {{"uri", doc.uri}, {"version", doc.version}}},
            {"contentChanges", json::array({json{{"text", text}}})},
        });
    }
    catch (const std::exception& e)
    {
        if (m_debug)
            std::cerr << "[klsp] didChange failed: " << e.what() << "\n";
    }
}

json Daemon::position_params(const std::string& client, const Doc& doc)
{
    auto pos = m_kak.query(client, {"%val{cursor_line}", "%val{cursor_char_column}"});
    int line = 0, col = 0;
    if (pos.size() >= 2)
    {
        try
        {
            line = std::stoi(pos[0]) - 1;   // Kakoune lines are 1-based
            col = std::stoi(pos[1]);        // columns are 0-based
        }
        catch (...) {}
    }
    return {
        {"textDocument", {{"uri", doc.uri}}},
        {"position", {{"line", std::max(0, line)}, {"character", std::max(0, col)}}},
    };
}

void Daemon::handle_hover(const std::string& client)
{
    auto it = m_docs.find(client);
    if (it == m_docs.end())
    {
        m_kak.echo_markup(client, "{\\}LSP not enabled for this buffer");
        return;
    }
    Server* server = server_for_doc(it->second);
    if (not server)
        return;

    json result;
    try { result = server->request("textDocument/hover", position_params(client, it->second)); }
    catch (const std::exception& e) { m_kak.echo_markup(client, std::string{"{\\}LSP hover failed: "} + e.what()); return; }

    if (m_debug)
        std::cerr << "[klsp] hover result: " << result.dump().substr(0, 120) << "\n";

    if (result.is_null() or not result.contains("contents"))
    {
        m_kak.echo_markup(client, "{\\}No hover information");
        return;
    }
    const json& contents = result["contents"];
    std::string text;
    if (contents.is_string())
        text = contents.get<std::string>();
    else if (contents.is_object())
        text = contents.value("value", std::string{});
    else if (contents.is_array())
        for (auto& part : contents)
            text += part.is_string() ? part.get<std::string>() : part.value("value", std::string{}) + "\n";
    m_kak.echo_markup(client, "{Information}" + single_line(text));
}

void Daemon::handle_definition(const std::string& client)
{
    auto it = m_docs.find(client);
    if (it == m_docs.end())
    {
        m_kak.echo_markup(client, "{\\}LSP not enabled for this buffer");
        return;
    }
    Server* server = server_for_doc(it->second);
    if (not server)
        return;

    json result;
    try { result = server->request("textDocument/definition", position_params(client, it->second)); }
    catch (const std::exception& e) { m_kak.echo_markup(client, std::string{"{\\}LSP definition failed: "} + e.what()); return; }

    if (result.is_null() or (result.is_array() and result.empty()))
    {
        m_kak.echo_markup(client, "{\\}No definition found");
        return;
    }
    json loc = result.is_array() ? result[0] : result;
    if (not loc.contains("uri"))
    {
        m_kak.echo_markup(client, "{\\}Definition has no location");
        return;
    }
    std::string path = uri_to_path(loc["uri"].get<std::string>());
    int line = loc["range"]["start"].value("line", 0) + 1;
    int col = loc["range"]["start"].value("character", 0) + 1;
    if (path.empty())
        return;
    // `kak -p` reads line-oriented input, so send two complete commands
    // rather than a newline inside a single %{ ... } block.
    std::string pos = std::to_string(line) + "." + std::to_string(col) + "," +
                      std::to_string(line) + "." + std::to_string(col);
    std::string cmd =
        "evaluate-commands -client " + shell_quote(client) + " %{ edit " + shell_quote(path) + " }\n"
        "evaluate-commands -client " + shell_quote(client) + " %{ select " + pos + " }\n";
    if (m_debug)
        std::cerr << "[klsp] definition -> " << path << " " << line << ":" << col << "\n";
    m_kak.run(cmd);
}

void Daemon::handle_references(const std::string& client)
{
    auto it = m_docs.find(client);
    if (it == m_docs.end())
        return;
    Server* server = server_for_doc(it->second);
    if (not server)
        return;
    json params = position_params(client, it->second);
    params["context"] = {{"includeDeclaration", true}};
    json result;
    try { result = server->request("textDocument/references", params); }
    catch (const std::exception& e) { m_kak.echo_markup(client, std::string{"{\\}LSP references failed: "} + e.what()); return; }
    size_t count = result.is_array() ? result.size() : 0;
    m_kak.echo_markup(client, "{\\}LSP: " + std::to_string(count) + " reference(s)");
}

void Daemon::handle_diagnostics(const json& msg)
{
    const json& params = msg.value("params", json::object());
    std::string uri = params.value("uri", std::string{});
    const json& diags = params.value("diagnostics", json::array());

    int errors = 0, warnings = 0;
    std::string summary;
    for (auto& d : diags)
    {
        int sev = d.value("severity", 1);
        if (sev == 1) ++errors;
        else if (sev == 2) ++warnings;
    }
    if (not summary.empty())
        summary += "  ";

    // Find a client currently viewing this document.
    std::string client;
    for (auto& [c, doc] : m_docs)
        if (doc.uri == uri) { client = c; break; }
    if (client.empty())
        return;

    std::string text = "{\\}LSP: " + std::to_string(errors) + " error(s), " +
                       std::to_string(warnings) + " warning(s)";
    m_kak.echo_markup(client, text);
    if (m_debug)
        std::cerr << "[klsp] diagnostics for " << uri << ": " << diags.size() << "\n";
}

void Daemon::drain_servers()
{
    for (auto& [key, entry] : m_servers)
    {
        if (not entry.server)
            continue;
        for (auto& msg : entry.server->drain_notifications())
        {
            if (msg.value("method", "") == "textDocument/publishDiagnostics")
                handle_diagnostics(msg);
        }
    }
}

void Daemon::handle_line(const std::string& line)
{
    auto words = split_command(line);
    if (words.empty())
        return;
    const std::string& verb = words[0];
    std::string client = words.size() > 1 ? words[1] : "";
    if (m_debug)
        std::cerr << "[klsp] request: " << line << "\n";

    if (verb == "enable") handle_enable(client);
    else if (verb == "did-change") handle_did_change(client);
    else if (verb == "hover") handle_hover(client);
    else if (verb == "definition") handle_definition(client);
    else if (verb == "references") handle_references(client);
    else if (verb == "quit") m_quit = true;
}

int Daemon::run()
{
    ::unlink(m_fifo.c_str());
    if (::mkfifo(m_fifo.c_str(), 0600) != 0 and errno != EEXIST)
    {
        std::cerr << "kakoune-lsp-cpp: cannot create fifo " << m_fifo << "\n";
        return 1;
    }

    // O_RDWR keeps a writer alive so reads return EAGAIN instead of EOF.
    int fd = ::open(m_fifo.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        std::cerr << "kakoune-lsp-cpp: cannot open fifo " << m_fifo << "\n";
        return 1;
    }

    if (m_debug)
        std::cerr << "[klsp] daemon listening on " << m_fifo << " (session " << m_session << ")\n";

    while (not m_quit)
    {
        pollfd pfd{fd, POLLIN, 0};
        int n = ::poll(&pfd, 1, 200);
        if (n > 0 and (pfd.revents & POLLIN))
        {
            char buf[4096];
            ssize_t r = ::read(fd, buf, sizeof(buf));
            if (r > 0)
            {
                m_input.append(buf, (size_t)r);
                size_t pos;
                while ((pos = m_input.find('\n')) != std::string::npos)
                {
                    std::string line = m_input.substr(0, pos);
                    m_input.erase(0, pos + 1);
                    if (not line.empty())
                        handle_line(line);
                }
            }
        }
        drain_servers();
    }

    ::close(fd);
    ::unlink(m_fifo.c_str());
    return 0;
}

} // namespace

int run_daemon(const std::string& session, const std::string& fifo_path, bool debug)
{
    Daemon daemon{session, fifo_path, debug};
    return daemon.run();
}

} // namespace klsp
