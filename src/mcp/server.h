#pragma once

namespace studio
{
    // MCP server over stdio (newline-delimited JSON-RPC 2.0). Every registry command is a tool.
    // Returns the process exit code when stdin closes.
    int run_mcp_server();
} // namespace studio
