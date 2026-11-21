//! The session model with `backend = "mock"`: plays a session through without a model, so a
//! board or a test can see every state. Words in the task steer it:
//!
//! - always: a plan of three steps, a run_js call and a line of log, then a result in Markdown;
//! - "ask" or "choose": asks which of two plumbers to book, and waits for the answer;
//! - "slow": takes 3 s over each step, long enough to watch it work and to talk into it;
//! - "note": the result opens with a heading, which makes the session a note;
//! - "fail": the run fails;
//! - "deadline": the question waits a minute, then takes the recommendation ("again": asks
//!   again; "give up": gives up);
//! - "expect": the plan says it will be done in 30 minutes;
//! - "schedule": makes the session a schedule, weekdays at 07:00, waking the board; its runs
//!   end with a result (or fail, with "fail");
//! - "watch": makes it a watch, every 5 minutes; each look reads 712, until a look whose
//!   condition says "DROPPED", which holds;
//! - "panel": draws a fare panel with every kind of block, a choice at its foot.
//!
//! Talked into afterwards, it answers "You said: …".

use std::time::Duration;

use anyhow::{Result, bail};
use async_trait::async_trait;
use serde_json::json;
use tokio::sync::mpsc;

use super::tools::{ASK_TOOL, PANEL_TOOL, PLAN_TOOL, SCHEDULE_TOOL, WATCH_TOOL};
use crate::llm::{Llm, Outcome, Piece, Request};
use crate::tools::JS_TOOL;

pub struct MockAgent;

#[async_trait]
impl Llm for MockAgent {
    async fn respond(&self, request: Request<'_>, out: mpsc::Sender<Piece>) -> Result<Outcome> {
        let say = |text: String| {
            let out = out.clone();
            async move {
                out.send(Piece::Text(text))
                    .await
                    .map_err(anyhow::Error::from)
            }
        };
        let call = |name: &'static str, input: serde_json::Value| {
            let out = out.clone();
            let tools = request.tools;
            async move {
                out.send(Piece::Tool(name.into(), input.clone())).await?;
                anyhow::Ok(tools.call(name, input).await)
            }
        };
        let follow_up = !request.history.is_empty()
            || request.user.contains("inkharness restarted")
            || request.user.contains("paused this task");
        if follow_up {
            say(format!("You said: {}", request.user)).await?;
            return Ok(Outcome::Done);
        }

        let task = request.user.trim();
        let words = task.to_lowercase();
        if task.starts_with("(A scheduled run") {
            if words.contains("fail") {
                bail!("the mock model failed, as the schedule asked");
            }
            say(format!(
                "**Brief:** {}",
                task.lines().last().unwrap_or_default()
            ))
            .await?;
            return Ok(Outcome::Done);
        }
        if task.starts_with("(A look for your watch") {
            let reading = if task.contains("DROPPED") {
                json!({"met": true})
            } else {
                json!({"value": 712})
            };
            call(WATCH_TOOL, reading).await?.ok();
            say("Looked: $712.".into()).await?;
            return Ok(Outcome::Done);
        }
        if words.contains("schedule") {
            let made = call(
                SCHEDULE_TOOL,
                json!({"at": "07:00", "days": "weekdays", "prompt": task, "wake_board": true}),
            )
            .await?;
            say(made.unwrap_or_else(|e| e)).await?;
            return Ok(Outcome::Done);
        }
        if words.contains("watch") {
            let made = call(WATCH_TOOL, json!({"every_minutes": 5, "condition": task})).await?;
            say(made.unwrap_or_else(|e| e)).await?;
            return Ok(Outcome::Done);
        }
        let pause = Duration::from_secs(if words.contains("slow") { 3 } else { 0 });
        let steps = ["Read the task", "Work it out", "Write the result"];
        let title: String = format!("Mock · {task}").chars().take(40).collect();
        let expect = words.contains("expect").then_some(30);
        let plan = |current: usize| json!({"steps": steps, "current": current, "title": title, "expect_minutes": expect});

        call(PLAN_TOOL, plan(1)).await?.ok();
        say("Reading the task.".into()).await?;
        call(JS_TOOL, json!({"code": "1 + 1"})).await?.ok();
        tokio::time::sleep(pause).await;
        call(PLAN_TOOL, plan(2)).await?.ok();
        if words.contains("panel") {
            call(PANEL_TOOL, fare_panel()).await?.ok();
        }
        let mut chose = String::new();
        if words.contains("ask") || words.contains("choose") || words.contains("deadline") {
            let mut question = json!({
                "question": "Which plumber should I book?",
                "detail": "Two can come this week.",
                "options": [
                    {"title": "Riverside · Thu 08:00", "right": "$145", "detail": "4.8 ★ (212 reviews), licensed, 2 km away."},
                    {"title": "Northgate · Wed 13:00", "right": "$120"}
                ],
                "recommend": 0
            });
            if words.contains("deadline") {
                question["deadline_minutes"] = json!(1);
                if words.contains("again") {
                    question["then"] = json!("ask_again");
                } else if words.contains("give up") {
                    question["then"] = json!("give_up");
                }
            }
            let answer = call(ASK_TOOL, question).await?;
            chose = format!("\n- You chose: {}", answer.unwrap_or_else(|e| e));
        }
        tokio::time::sleep(pause).await;
        if words.contains("fail") {
            bail!("the mock model failed, as the task asked");
        }
        call(PLAN_TOOL, plan(3)).await?.ok();
        tokio::time::sleep(pause).await;
        call(PLAN_TOOL, plan(4)).await?.ok();
        let result = if words.contains("note") {
            format!("# Note: {task}\n\n- The first thing to keep\n- The second{chose}\n")
        } else {
            format!("**Done:** {task}\n\n- Read it\n- Worked it out{chose}\n")
        };
        say(result).await?;
        Ok(Outcome::Done)
    }
}

/// The panel "panel" draws: one of each block.
pub fn fare_panel() -> serde_json::Value {
    json!({
        "title": "Lisbon fare",
        "blocks": [
            {"kind": "section", "label": "LIS · 12–19 MAR", "right": "TAP"},
            {"kind": "stat", "items": [{"label": "NOW", "value": "$712"}, {"label": "LOW", "value": "$688"}, {"label": "GOAL", "value": "$700"}]},
            {"kind": "spark", "label": "LAST 7 DAYS", "values": [760, 741, 755, 730, 712, 698, 712], "right": "-6%"},
            {"kind": "meter", "label": "TO GOAL", "value": 12, "max": 60, "right": "$12 OVER"},
            {"kind": "kv", "items": [{"key": "OUT", "value": "Thu 12 Mar 07:40"}, {"key": "BACK", "value": "Thu 19 Mar 18:05"}]},
            {"kind": "rows", "items": [{"title": "TAP 1352", "sub": "direct · 7h05", "right": "$712", "tag": "BEST"}, {"title": "Iberia 3141", "sub": "1 stop · 9h40", "right": "$744"}]},
            {"kind": "text", "markdown": "Fares usually dip **mid-week**. I'll keep looking every 30 minutes."},
            {"kind": "choice", "id": "book", "question": "Book it now?", "options": [{"title": "Book TAP 1352", "right": "$712"}, {"title": "Keep watching"}], "recommend": 1}
        ]
    })
}
