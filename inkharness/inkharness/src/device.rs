//! The tools a connected device runs itself (docs/voice-protocol.md, "Device tools"). The board
//! announces them in its hello; the model calls them like any other tool, and a call goes down
//! the device's connection as `tool_call` and waits for its `tool_result`.

use std::collections::{HashMap, HashSet};
use std::sync::Mutex;
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::Duration;

use serde_json::Value;
use tokio::sync::{mpsc, oneshot};
use voice_proto::{DeviceTool, Frame, ServerMsg};

use crate::tools::ToolDef;

/// How long a device has to answer a call.
pub const TIMEOUT: Duration = Duration::from_secs(15);
/// Tools taken from one device; the rest are left out.
const MAX_TOOLS: usize = 32;

type Answer = Result<String, String>;

pub struct Device {
    defs: Vec<ToolDef>,
    tx: mpsc::Sender<Frame>,
    next: AtomicU32,
    waiting: Mutex<HashMap<u32, oneshot::Sender<Answer>>>,
}

impl Device {
    /// The device's tools, minus any whose name is taken (by `taken`, or by one before it) or
    /// isn't one a model API accepts. `tx` is the connection's way out.
    pub fn new(announced: Vec<DeviceTool>, taken: &[&str], tx: mpsc::Sender<Frame>) -> Device {
        let mut names: HashSet<String> = taken.iter().map(|n| n.to_string()).collect();
        let mut defs = Vec::new();
        for tool in announced {
            if defs.len() == MAX_TOOLS {
                tracing::warn!("the device has more than {MAX_TOOLS} tools: the rest are left out");
                break;
            }
            let valid = (1..=64).contains(&tool.name.len())
                && tool
                    .name
                    .chars()
                    .all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-');
            if !valid {
                tracing::warn!(tool = %tool.name, "leaving out a device tool with that name");
                continue;
            }
            if !names.insert(tool.name.clone()) {
                tracing::warn!(tool = %tool.name, "the device's tool has a name already in use: left out");
                continue;
            }
            defs.push(ToolDef {
                name: tool.name,
                description: tool.description,
                schema: tool.input_schema,
            });
        }
        Device {
            defs,
            tx,
            next: AtomicU32::new(1),
            waiting: Mutex::default(),
        }
    }

    pub fn defs(&self) -> &[ToolDef] {
        &self.defs
    }

    pub fn has(&self, name: &str) -> bool {
        self.defs.iter().any(|d| d.name == name)
    }

    /// Runs a tool on the device: `Err` is a message for the model.
    pub async fn call(&self, name: &str, input: Value) -> Answer {
        let id = self.next.fetch_add(1, Ordering::Relaxed);
        let (done, answer) = oneshot::channel();
        self.waiting.lock().unwrap().insert(id, done);
        // A turn cut short drops this future: the call is forgotten, and its late answer with it.
        let _forget = Forget(self, id);
        let msg = ServerMsg::ToolCall {
            id,
            name: name.to_string(),
            input,
        };
        let frame = Frame::control(&msg).map_err(|e| e.to_string())?;
        if self.tx.send(frame).await.is_err() {
            return Err("the board has gone".into());
        }
        match tokio::time::timeout(TIMEOUT, answer).await {
            Ok(Ok(answer)) => answer,
            Ok(Err(_)) => Err("the board has gone".into()),
            Err(_) => Err(format!(
                "the board didn't answer in {} s",
                TIMEOUT.as_secs()
            )),
        }
    }

    /// The device's `tool_result`. One for no call still waiting (a turn cut short) is dropped.
    pub fn answer(&self, id: u32, text: String, error: bool) {
        match self.waiting.lock().unwrap().remove(&id) {
            Some(done) => {
                let _ = done.send(if error { Err(text) } else { Ok(text) });
            }
            None => tracing::debug!(id, "an answer for no call waiting"),
        }
    }
}

struct Forget<'a>(&'a Device, u32);

impl Drop for Forget<'_> {
    fn drop(&mut self) {
        self.0.waiting.lock().unwrap().remove(&self.1);
    }
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    fn tool(name: &str) -> DeviceTool {
        DeviceTool {
            name: name.into(),
            description: "d".into(),
            input_schema: json!({"type": "object"}),
        }
    }

    #[tokio::test]
    async fn calls_go_down_and_answers_come_back() {
        let (tx, mut rx) = mpsc::channel(4);
        let device = Device::new(
            vec![
                tool("open_book"),
                tool("run_js"),
                tool("bad name"),
                tool("open_book"),
            ],
            &["run_js"],
            tx,
        );
        let names: Vec<&str> = device.defs().iter().map(|d| d.name.as_str()).collect();
        assert_eq!(names, ["open_book"]);

        let board = async {
            let Some(Frame::Control(json)) = rx.recv().await else {
                panic!("no call")
            };
            let ServerMsg::ToolCall { id, name, input } = serde_json::from_str(&json).unwrap()
            else {
                panic!("not a call: {json}")
            };
            assert_eq!(
                (name.as_str(), &input),
                ("open_book", &json!({"path": "a"}))
            );
            device.answer(id + 100, "not this one".into(), false);
            device.answer(id, "Opening it.".into(), false);
        };
        let (answer, ()) = tokio::join!(device.call("open_book", json!({"path": "a"})), board);
        assert_eq!(answer, Ok("Opening it.".into()));
        assert!(device.waiting.lock().unwrap().is_empty());
    }
}
