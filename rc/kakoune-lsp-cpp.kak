# kakoune-lsp-cpp.kak — Kakoune integration for the kakoune-lsp-cpp daemon.
#
# Add to your kakrc (after installing both the binary and this file):
#
#     require-module kakoune-lsp-cpp   # if installed as a module, or
#     source /path/to/rc/kakoune-lsp-cpp.kak
#
# Then either call `:lsp-enable` per buffer, or keep the auto-enable hook below.

declare-option -docstring "Command used to run the kakoune-lsp-cpp daemon" \
    str klsp_cmd kakoune-lsp-cpp
declare-option -hidden str klsp_fifo ""
declare-option -hidden bool klsp_enabled false

# Auto-enable LSP for filetypes with a known language server.
declare-option -hidden bool klsp_auto_enable true

define-command -hidden klsp-start %{
    evaluate-commands %sh{
        fifo="${TMPDIR:-/tmp}/kakoune-lsp-cpp-${kak_session}.fifo"
        if [ ! -p "$fifo" ]; then
            rm -f "$fifo"
            if command -v setsid >/dev/null 2>&1; then
                setsid "${kak_opt_klsp_cmd}" --daemon --session "${kak_session}" \
                    --fifo "$fifo" </dev/null >/dev/null 2>&1 &
            else
                "${kak_opt_klsp_cmd}" --daemon --session "${kak_session}" \
                    --fifo "$fifo" </dev/null >/dev/null 2>&1 &
            fi
        fi
        printf "set-option global klsp_fifo '%s'\n" "$fifo"
    }
}

define-command -hidden klsp-send -params 1.. -docstring "Send a request to the LSP daemon" %{
    evaluate-commands %sh{
        fifo="${kak_opt_klsp_fifo}"
        [ -p "$fifo" ] || exit 0
        printf '%s\n' "$*" > "$fifo"
    }
}

define-command lsp-enable -docstring "Enable LSP for the current buffer" %{
    klsp-start
    set-option buffer klsp_enabled true
    klsp-send enable %val{client}
}

define-command lsp-disable -docstring "Disable LSP for the current buffer" %{
    set-option buffer klsp_enabled false
}

define-command lsp-hover -docstring "Show LSP hover information" %{
    klsp-send hover %val{client}
}

define-command lsp-definition -docstring "Jump to the LSP definition" %{
    klsp-send definition %val{client}
}

define-command lsp-references -docstring "Count LSP references" %{
    klsp-send references %val{client}
}

define-command lsp-stop -docstring "Stop the LSP daemon for this session" %{
    klsp-send quit
    set-option global klsp_fifo ""
}

# Push buffer changes to the server on idle, when enabled for this buffer.
hook global NormalIdle .* %{
    try %{ evaluate-commands %sh{
        if [ "${kak_opt_klsp_enabled}" = true ]; then
            printf '%s\n' "klsp-send did-change ${kak_client}"
        fi
    } }
}

# Auto-enable for filetypes with a built-in server, once.
hook global WinSetOption filetype=(c|cpp|objc|rust|go|python|lua) %{
    try %{ evaluate-commands %sh{
        if [ "${kak_opt_klsp_auto_enable}" = true ]; then
            printf '%s\n' "lsp-enable"
        fi
    } }
}

define-command lsp-menu -docstring "LSP actions for the current buffer" %{
    menu "%opt{klsp_menu}" \
        h 'lsp-hover'    -docstring 'hover' \
        d 'lsp-definition' -docstring 'definition' \
        r 'lsp-references' -docstring 'references' \
        e 'lsp-enable'   -docstring 'enable' \
        x 'lsp-disable'  -docstring 'disable'
} -docstring "LSP actions"

declare-option -hidden str klsp_menu "h: hover  d: definition  r: references  e: enable  x: disable"
