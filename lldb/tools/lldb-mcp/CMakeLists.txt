set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(LLVM Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(MCPOptionsTableGen)

add_lldb_tool(lldb-mcp ADD_TO_FRAMEWORK
  lldb-mcp.cpp

  LINK_COMPONENTS
    Option
    Support
  LINK_LIBS
    lldbHost
    lldbProtocolMCP
    liblldb
  )

add_dependencies(lldb-mcp
  MCPOptionsTableGen
  ${tablegen_deps}
)
