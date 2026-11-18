//! A small MCP server for trying inkharness's MCP support without a real one. Speaks MCP on
//! stdin/stdout, or with --http ADDR as a streamable HTTP endpoint at http://ADDR/mcp.
//!
//!   cargo build --release --example mcp_demo
//!   [[mcp]]  name = "demo"  command = "target/release/examples/mcp_demo"
//!   [[mcp]]  name = "demo"  url = "http://127.0.0.1:8930/mcp"   (with --http 127.0.0.1:8930)

use std::sync::Arc;

use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{CallToolResult, ContentBlock};
use rmcp::transport::streamable_http_server::session::local::LocalSessionManager;
use rmcp::transport::{StreamableHttpService, stdio};
use rmcp::{ErrorData, ServerHandler, ServiceExt, schemars, tool, tool_handler, tool_router};

#[derive(serde::Deserialize, schemars::JsonSchema)]
struct Room {
    /// The room, e.g. "kitchen"
    room: String,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
struct Thermostat {
    room: String,
    /// Degrees Celsius
    celsius: f64,
}

#[derive(Clone)]
struct Demo {
    tool_router: ToolRouter<Self>,
}

#[tool_router]
impl Demo {
    fn new() -> Self {
        Demo {
            tool_router: Self::tool_router(),
        }
    }

    #[tool(description = "The temperature in a room, in degrees Celsius")]
    async fn room_temperature(
        &self,
        Parameters(r): Parameters<Room>,
    ) -> Result<CallToolResult, ErrorData> {
        let celsius = 18.0 + (r.room.len() % 5) as f64;
        eprintln!("mcp_demo: room_temperature {}", r.room);
        Ok(CallToolResult::success(vec![ContentBlock::text(format!(
            "{celsius:.1}"
        ))]))
    }

    #[tool(description = "Sets the thermostat in a room, in degrees Celsius (10 to 28)")]
    async fn set_thermostat(
        &self,
        Parameters(t): Parameters<Thermostat>,
    ) -> Result<CallToolResult, ErrorData> {
        eprintln!("mcp_demo: set_thermostat {} {}", t.room, t.celsius);
        if !(10.0..=28.0).contains(&t.celsius) {
            return Ok(CallToolResult::error(vec![ContentBlock::text(
                "out of range: 10 to 28",
            )]));
        }
        Ok(CallToolResult::success(vec![ContentBlock::text(format!(
            "{} thermostat set to {:.1}",
            t.room, t.celsius
        ))]))
    }
}

#[tool_handler(router = self.tool_router)]
impl ServerHandler for Demo {}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if let Some(addr) = args
        .iter()
        .position(|a| a == "--http")
        .and_then(|i| args.get(i + 1))
    {
        let service = StreamableHttpService::new(
            || Ok(Demo::new()),
            Arc::new(LocalSessionManager::default()),
            Default::default(),
        );
        let router = axum::Router::new().nest_service("/mcp", service);
        let listener = tokio::net::TcpListener::bind(addr).await?;
        eprintln!("mcp_demo: http://{addr}/mcp");
        axum::serve(listener, router).await?;
    } else {
        Demo::new().serve(stdio()).await?.waiting().await?;
    }
    Ok(())
}
