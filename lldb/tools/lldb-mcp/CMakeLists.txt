set(LLVM_TARGET_DEFINITIONS Options.td)
tablegen(LLVM Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(MCPOptionsTableGen)

add_lldb_tool(lldb-mcp
  lldb-mcp.cpp

  LINK_COMPONENTS
    Support
    Option
  LINK_LIBS
    liblldb
    lldbHost
    lldbUtility
  )

add_dependencies(lldb-mcp
  MCPOptionsTableGen
  ${tablegen_deps}
)
