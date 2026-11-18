//! What a turn cost. Each stage records the requests it makes: the service's id for the
//! request (OpenRouter's generation id, which its /api/v1/generation endpoint prices) and the
//! cost when the response states it. The session logs them with the turn.
//!
//! The record is task-local, so the stages don't pass it around: a turn runs in one task, and
//! everything it awaits (its stages are joined, not spawned) records into it. Requests made
//! outside a turn (phrases synthesised at startup) go unrecorded.

use std::cell::RefCell;
use std::fmt::Write;
use std::future::Future;

#[derive(Debug, Clone, PartialEq)]
pub struct Charge {
    pub stage: &'static str,
    /// The service's id for the request, if it gave one.
    pub id: Option<String>,
    /// In US dollars, if the response said.
    pub cost: Option<f64>,
}

tokio::task_local! {
    static CHARGES: RefCell<Vec<Charge>>;
}

/// Records one request of the turn running in this task (none: nothing happens).
pub fn record(stage: &'static str, id: Option<&str>, cost: Option<f64>) {
    let charge = Charge {
        stage,
        id: id.filter(|i| !i.is_empty()).map(str::to_string),
        cost,
    };
    let _ = CHARGES.try_with(|c| c.borrow_mut().push(charge));
}

/// Runs `f`, returning what it recorded.
pub async fn measure<F: Future>(f: F) -> (F::Output, Vec<Charge>) {
    CHARGES
        .scope(RefCell::new(Vec::new()), async {
            let out = f.await;
            (out, CHARGES.with(|c| c.take()))
        })
        .await
}

/// What the turn running in this task has recorded so far.
pub fn so_far() -> Vec<Charge> {
    CHARGES.try_with(|c| c.borrow().clone()).unwrap_or_default()
}

/// The stated costs added up, and how many requests stated none.
pub fn total(charges: &[Charge]) -> (f64, usize) {
    let cost = charges.iter().filter_map(|c| c.cost).sum();
    (cost, charges.iter().filter(|c| c.cost.is_none()).count())
}

/// `stage:id` for each request, comma-separated (`stage:?` without an id).
pub fn ids(charges: &[Charge]) -> String {
    let mut out = String::new();
    for c in charges {
        if !out.is_empty() {
            out.push(',');
        }
        let _ = write!(out, "{}:{}", c.stage, c.id.as_deref().unwrap_or("?"));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn records_inside_a_turn_only() {
        record("tts", Some("outside"), None);
        let ((), charges) = measure(async {
            record("stt", Some("gen-1"), Some(0.5));
            let ((), ()) =
                tokio::join!(async { record("llm", Some("gen-2"), Some(0.25)) }, async {
                    record("tts", Some(""), None)
                });
        })
        .await;
        assert_eq!(ids(&charges), "stt:gen-1,llm:gen-2,tts:?");
        assert_eq!(total(&charges), (0.75, 1));
    }
}
