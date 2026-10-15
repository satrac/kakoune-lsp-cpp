#include "json.hh"
#include "lsp.hh"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

#ifndef KLSP_VERSION
#define KLSP_VERSION "0.0.0"
#endif

using namespace klsp;

static const char* USAGE =
    "kakoune-lsp-cpp " KLSP_VERSION "\n"
    "\n"
    "Usage:\n"
    "  kakoune-lsp-cpp --daemon --session <name> [--server <cmd>]   run the LSP daemon\n"
    "  kakoune-lsp-cpp --probe  --server <cmd> [--file <path>] [--line N --col N]\n"
    "                                                            one-shot LSP handshake test\n"
    "  kakoune-lsp-cpp --version | --help\n";

static std::string read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (not in)
        throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string capability_json()
{
    return R"({
        "textDocument": {
            "synchronization": { "dynamicRegistration": false },
            "hover": { "contentFormat": ["markdown", "plaintext"] },
            "definition": {},
            "typeDefinition": {},
            "implementation": {},
            "references": {},
            "rename": { "prepareSupport": false },
            "documentFormatting": {},
            "documentSymbol": {},
            "publishDiagnostics": { "relatedInformation": true },
            "completion": { "completionItem": { "snippetSupport": false } }
        },
        "workspace": { "workspaceFolders": false }
    })";
}

static std::string absolute_path(const std::string& path)
{
    if (not path.empty() and path[0] == '/')
        return path;
    char cwd[4096];
    if (::getcwd(cwd, sizeof(cwd)) == nullptr)
        return path;
    return std::string{cwd} + "/" + path;
}

static int run_probe(const std::string& server_cmd, const std::string& file,
                     int line, int col)
{
    Server server{split_command(server_cmd)};

    std::string root = ".";
    if (not file.empty())
        root = absolute_path(file).substr(0, absolute_path(file).rfind('/'));

    json init_params = {
        {"processId", (long)::getpid()},
        {"clientInfo", {{"name", "kakoune-lsp-cpp"}, {"version", KLSP_VERSION}}},
        {"rootUri", to_file_uri(absolute_path(root))},
        {"capabilities", json::parse(capability_json())},
    };

    json result = server.request("initialize", init_params);
    std::cout << "initialize serverInfo: "
              << result.value("serverInfo", json::object()).dump() << "\n";

    server.notify("initialized", json::object());

    if (not file.empty())
    {
        std::string abs = absolute_path(file);
        json did_open = {
            {"textDocument", {
                {"uri", to_file_uri(abs)},
                {"languageId", "cpp"},
                {"version", 1},
                {"text", read_file(abs)},
            }},
        };
        server.notify("textDocument/didOpen", did_open);

        json hover = server.request("textDocument/hover", {
            {"textDocument", {{"uri", to_file_uri(abs)}}},
            {"position", {{"line", line}, {"character", col}}},
        });
        std::cout << "hover: " << hover.dump() << "\n";
    }

    server.request("shutdown", json());
    server.notify("exit", json());
    return 0;
}

int main(int argc, char** argv)
{
    std::string session, server_cmd, file;
    int line = 0, col = 0;
    bool probe = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
            {
                std::cerr << "missing argument for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--version") { std::cout << "kakoune-lsp-cpp " KLSP_VERSION "\n"; return 0; }
        else if (arg == "--help" or arg == "-h") { std::cout << USAGE; return 0; }
        else if (arg == "--probe") probe = true;
        else if (arg == "--daemon") probe = false;
        else if (arg == "--session") session = next("--session");
        else if (arg == "--server") server_cmd = next("--server");
        else if (arg == "--file") file = next("--file");
        else if (arg == "--line") line = std::stoi(next("--line"));
        else if (arg == "--col") col = std::stoi(next("--col"));
        else { std::cerr << "unknown argument: " << arg << "\n" << USAGE; return 2; }
    }

    try
    {
        if (probe)
        {
            if (server_cmd.empty())
            {
                std::cerr << "--probe requires --server <cmd>\n";
                return 2;
            }
            return run_probe(server_cmd, file, line, col);
        }

        // Daemon mode (Kakoune integration) is the next milestone.
        (void)session;
        std::cerr << "daemon mode is not implemented yet; use --probe\n";
        return 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
