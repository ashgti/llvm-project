include "llvm/Option/OptParser.td"

class F<string name> : Flag<["--", "-"], name>;

def help : F<"help">,
           HelpText<"Prints out the usage information for the lldb-mcp tool.">;
def : Flag<["-"], "h">, Alias<help>, HelpText<"Alias for --help">;

def version : F<"version">, HelpText<"Prints out the lldb-mcp version.">;
def : Flag<["-"], "v">, Alias<version>, HelpText<"Alias for --version">;

def server : Flag<["--"], "server">,
             HelpText<"Starts a new lldb-mcp multiplexer server.">;
