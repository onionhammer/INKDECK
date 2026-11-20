//! The PIN in front of the sessions (`[agent] pin_sha256`). The token alone is on the board's
//! card, so a lost board or a copied card shouldn't reach the agent: with a PIN set, a
//! connection unlocks with it (or with a ticket from an earlier unlock) before it sees or does
//! anything with sessions, and HTTP requests send it.
//!
//! Wrong PINs back off as the board's own do: after 3 in a row (across connections), each
//! further try is refused for 30 s, then twice as long each time, up to 30 min. The count is kept
//! in `pin-tries.json` in the sessions' folder, so a restart doesn't clear it. Tickets are random,
//! 128 bits, last `ticket_minutes` from when they were issued, and live in memory only.

use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Mutex;

use anyhow::{Result, bail};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

/// Wrong PINs in a row before tries are refused for a while.
const FREE_TRIES: u32 = 3;
const FIRST_WAIT_S: u64 = 30;
const LONGEST_WAIT_S: u64 = 30 * 60;

/// How an unlock went.
#[derive(Debug, Clone, PartialEq)]
pub enum Unlock {
    Ok {
        ticket: String,
        ttl_s: u64,
    },
    /// The PIN or ticket was wrong. `wait_s`: the wrong PIN started a wait before the next try.
    Wrong {
        wait_s: Option<u64>,
    },
    /// Refused unchecked: too many wrong PINs, try again in `wait_s`.
    Wait {
        wait_s: u64,
    },
}

pub struct Lock {
    /// The PIN's SHA-256, lowercase hex.
    pin: Option<String>,
    ttl_s: u64,
    file: PathBuf,
    state: Mutex<LockState>,
}

#[derive(Default)]
struct LockState {
    backoff: Backoff,
    /// Ticket → when it expires (Unix seconds).
    tickets: HashMap<String, u64>,
}

/// What survives a restart.
#[derive(Debug, Default, Clone, Copy, PartialEq, Serialize, Deserialize)]
struct Backoff {
    /// Wrong PINs in a row.
    wrong: u32,
    /// No try is checked before this (Unix seconds).
    until: u64,
}

/// How long tries are refused after `wrong` wrong PINs in a row.
fn wait_after(wrong: u32) -> u64 {
    if wrong < FREE_TRIES {
        return 0;
    }
    let doublings = (wrong - FREE_TRIES).min(16);
    (FIRST_WAIT_S << doublings).min(LONGEST_WAIT_S)
}

/// SHA-256 of `text`, lowercase hex.
pub fn sha256_hex(text: &str) -> String {
    Sha256::digest(text.as_bytes())
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// Equal, in time that doesn't depend on where they differ.
fn same(a: &str, b: &str) -> bool {
    a.len() == b.len() && a.bytes().zip(b.bytes()).fold(0u8, |d, (x, y)| d | (x ^ y)) == 0
}

impl Lock {
    /// `file` keeps the wrong count between restarts.
    pub fn new(pin_sha256: Option<&str>, ticket_minutes: u64, file: PathBuf) -> Result<Lock> {
        let pin = match pin_sha256.map(str::trim) {
            None | Some("") => None,
            Some(hex) if hex.len() == 64 && hex.bytes().all(|b| b.is_ascii_hexdigit()) => {
                Some(hex.to_ascii_lowercase())
            }
            Some(_) => bail!(
                "[agent] pin_sha256 must be 64 hex digits: printf %s 1234 | sha256sum gives the one for 1234"
            ),
        };
        let backoff = std::fs::read(&file)
            .ok()
            .and_then(|bytes| serde_json::from_slice(&bytes).ok())
            .unwrap_or_default();
        Ok(Lock {
            pin,
            ttl_s: ticket_minutes * 60,
            file,
            state: Mutex::new(LockState {
                backoff,
                tickets: HashMap::new(),
            }),
        })
    }

    pub fn has_pin(&self) -> bool {
        self.pin.is_some()
    }

    /// Tries a PIN, and issues a ticket if it is right. With no PIN set, anything unlocks.
    pub fn pin(&self, pin: &str, now: u64) -> Unlock {
        match self.check(pin, now) {
            Some(refused) => refused,
            None => self.issue(now),
        }
    }

    /// Checks a PIN without issuing a ticket (an HTTP request carries it every time): `None`
    /// if it is right, else why not. Counts towards the backoff like `pin`.
    pub fn check(&self, pin: &str, now: u64) -> Option<Unlock> {
        let want = self.pin.as_ref()?;
        let mut state = self.state.lock().unwrap();
        if state.backoff.until > now {
            return Some(Unlock::Wait {
                wait_s: state.backoff.until - now,
            });
        }
        if same(&sha256_hex(pin), want) {
            if state.backoff != Backoff::default() {
                state.backoff = Backoff::default();
                self.save(state.backoff);
            }
            return None;
        }
        state.backoff.wrong += 1;
        let wait = wait_after(state.backoff.wrong);
        state.backoff.until = now + wait;
        self.save(state.backoff);
        tracing::warn!(
            wrong = state.backoff.wrong,
            wait_s = wait,
            "a wrong agent PIN"
        );
        Some(Unlock::Wrong {
            wait_s: (wait > 0).then_some(wait),
        })
    }

    /// Tries a ticket from an earlier unlock: the same ticket back, with the time it has left.
    /// A wrong one doesn't count towards the backoff (they can't be guessed, and an expired one
    /// is ordinary).
    pub fn ticket(&self, ticket: &str, now: u64) -> Unlock {
        if self.pin.is_none() {
            return self.issue(now); // nothing to unlock
        }
        let mut state = self.state.lock().unwrap();
        state.tickets.retain(|_, expires| *expires > now);
        match state.tickets.get(ticket) {
            Some(expires) => Unlock::Ok {
                ticket: ticket.to_string(),
                ttl_s: expires - now,
            },
            None => Unlock::Wrong { wait_s: None },
        }
    }

    fn issue(&self, now: u64) -> Unlock {
        let mut bytes = [0u8; 16];
        if let Err(e) = getrandom::fill(&mut bytes) {
            tracing::warn!("no randomness for a ticket: {e}");
            return Unlock::Wrong { wait_s: None };
        }
        let ticket: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
        let mut state = self.state.lock().unwrap();
        state.tickets.retain(|_, expires| *expires > now);
        state.tickets.insert(ticket.clone(), now + self.ttl_s);
        Unlock::Ok {
            ticket,
            ttl_s: self.ttl_s,
        }
    }

    fn save(&self, backoff: Backoff) {
        let write = serde_json::to_vec(&backoff)
            .map_err(std::io::Error::other)
            .and_then(|json| std::fs::write(&self.file, json));
        if let Err(e) = write {
            tracing::warn!(file = %self.file.display(), "couldn't keep the PIN count: {e}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lock(name: &str) -> (Lock, PathBuf) {
        let dir = crate::skills::tests::scratch(name);
        let file = dir.join("pin-tries.json");
        let pin = sha256_hex("1234");
        (Lock::new(Some(&pin), 60, file.clone()).unwrap(), file)
    }

    #[test]
    fn the_right_pin_unlocks_and_gives_a_ticket() {
        let (lock, _) = lock("lock-right");
        assert!(lock.has_pin());
        let Unlock::Ok { ticket, ttl_s } = lock.pin("1234", 1000) else {
            panic!("refused")
        };
        assert_eq!((ticket.len(), ttl_s), (32, 3600));
        assert_eq!(lock.pin("4321", 1000), Unlock::Wrong { wait_s: None });
        // The ticket lets a new connection in for the hour from when it was issued, no longer.
        assert_eq!(
            lock.ticket(&ticket, 1600),
            Unlock::Ok {
                ticket: ticket.clone(),
                ttl_s: 3000
            }
        );
        assert_eq!(lock.ticket(&ticket, 4600), Unlock::Wrong { wait_s: None });
        assert_eq!(lock.ticket("nope", 1000), Unlock::Wrong { wait_s: None });
    }

    #[test]
    fn wrong_pins_back_off_and_a_restart_remembers() {
        let (lock, file) = lock("lock-backoff");
        assert_eq!(lock.pin("0000", 0), Unlock::Wrong { wait_s: None });
        assert_eq!(lock.pin("0001", 1), Unlock::Wrong { wait_s: None });
        // The third starts the wait; tries during it aren't checked, the right PIN included.
        assert_eq!(lock.pin("0002", 2), Unlock::Wrong { wait_s: Some(30) });
        assert_eq!(lock.pin("1234", 12), Unlock::Wait { wait_s: 20 });
        // A restart: the count is still there.
        let lock = Lock::new(Some(&sha256_hex("1234")), 60, file.clone()).unwrap();
        assert_eq!(lock.pin("1234", 31), Unlock::Wait { wait_s: 1 });
        assert_eq!(lock.pin("0003", 32), Unlock::Wrong { wait_s: Some(60) });
        assert_eq!(lock.pin("0004", 92), Unlock::Wrong { wait_s: Some(120) });
        assert!(matches!(lock.pin("1234", 212), Unlock::Ok { .. }));
        // The right one clears the count, on disk too.
        let lock = Lock::new(Some(&sha256_hex("1234")), 60, file).unwrap();
        assert_eq!(lock.pin("0000", 300), Unlock::Wrong { wait_s: None });
    }

    #[test]
    fn waits_double_up_to_half_an_hour() {
        let waits: Vec<u64> = (1..=12).map(wait_after).collect();
        assert_eq!(
            waits,
            [0, 0, 30, 60, 120, 240, 480, 960, 1800, 1800, 1800, 1800]
        );
        assert_eq!(wait_after(u32::MAX), LONGEST_WAIT_S);
    }

    #[test]
    fn without_a_pin_anything_unlocks() {
        let dir = crate::skills::tests::scratch("lock-none");
        let lock = Lock::new(None, 60, dir.join("pin-tries.json")).unwrap();
        assert!(!lock.has_pin());
        assert!(matches!(lock.pin("", 0), Unlock::Ok { .. }));
        assert!(matches!(lock.ticket("old", 0), Unlock::Ok { .. }));
        assert!(Lock::new(Some("abc"), 60, dir.join("pin-tries.json")).is_err());
        assert_eq!(
            sha256_hex("1234"),
            "03ac674216f3e15c761ee1a5e255f067953623c8b388b4459e13f978d7c846f4"
        );
    }
}
